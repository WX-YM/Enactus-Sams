#include "anvil/notifications/publish.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "anvil/analytics/counters.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/digest.h"

namespace anvil::notifications {
namespace {

namespace f = notification_fields;

// Domain separation. A coalescing key and an idempotency key are derived from
// overlapping material, and without a distinguishing prefix a caller could — with
// enough contrivance — make one collide with the other and suppress a
// notification that should have been written. One byte, once, at the front.
constexpr std::uint8_t kCoalesceDomain = 'c';
constexpr std::uint8_t kIdempotencyDomain = 'i';

constexpr std::int64_t kMillisPerSecond = 1000;
constexpr std::int64_t kSecondsPerDay = 86400;

// Little-endian, low byte first, and explicit rather than a memcpy of the
// integer: this feeds a key that is PERSISTED and looked up again by a later
// process, so a value hashed on one host has to hash identically on any other.
// The same wire discipline PermSet and Preferences take.
void append_le(std::string& out, std::uint64_t value, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xFFU));
    }
}

void append_uuid_bytes(std::string& out, const Uuid& id) {
    out.append(reinterpret_cast<const char*>(id.data()), id.size());
}

[[nodiscard]] std::array<std::uint8_t, 16> truncate_to_128(const crypto::Digest256& digest) noexcept {
    std::array<std::uint8_t, 16> out{};
    std::copy_n(digest.begin(), out.size(), out.begin());
    return out;
}

// The placeholder letters the template actually uses.
//
// Recomputed from the table at runtime rather than stored on TemplateSpec: the
// set is already proven identical across every locale by
// `template_table_is_well_formed`, so any one locale answers for all of them, and
// a redundant stored copy is a second thing that can disagree with the strings.
//
// Title MERGED with body, which is the same union `template_table_is_well_formed`
// compares against `param_count`. Scanning one of the two would disagree with the
// table check for every template that puts its only placeholder in the body —
// which is most of them.
[[nodiscard]] detail::PlaceholderSet placeholders_of_template(const TemplateSpec& spec) noexcept {
    const Locale any{};
    return detail::merged(detail::placeholders_of(spec.title.get(any)),
                          detail::placeholders_of(spec.body.get(any)));
}

[[nodiscard]] const Param* find_param(std::span<const Param> params, char name) noexcept {
    for (const Param& param : params) {
        if (param.name == name) { return &param; }
    }
    return nullptr;
}

// The publish-time half of the parameter contract. The renderer degrades a
// missing placeholder to nothing, which is right for a row an older build wrote —
// and exactly wrong for a row being written now, where it is a sentence with a
// hole in it that nobody will ever be able to repair.
[[nodiscard]] Status check_params(const TemplateSpec& spec, std::span<const Param> params) {
    if (params.size() > kMaxParams) { return fail(ErrorCode::ValidationFailed, f::kParams); }
    if (params.size() != spec.param_count) {
        return fail(ErrorCode::ValidationFailed, f::kParams);
    }

    const detail::PlaceholderSet placeholders = placeholders_of_template(spec);
    // Unreachable through a table that passed `template_table_is_well_formed`,
    // which is why it is Internal and not ValidationFailed: reaching it means the
    // table compiled and the scanner disagrees with itself.
    if (!placeholders.well_formed) { return fail(ErrorCode::Internal, f::kTemplate); }

    for (std::uint8_t i = 0; i < placeholders.count; ++i) {
        if (find_param(params, placeholders.names[i]) == nullptr) {
            return fail(ErrorCode::ValidationFailed, f::kParams);
        }
    }
    for (const Param& param : params) {
        // Every supplied parameter must be one the template asks for. An extra is
        // refused rather than dropped, for the reason an unknown JSON field is:
        // dropping hides both a caller's bug and a probe, and the caller believes
        // it sent something that arrived.
        if (!detail::contains(placeholders, param.name)) {
            return fail(ErrorCode::ValidationFailed, f::kParams);
        }
        if (const Status usable = check_param(param); !usable) { return usable.error(); }
    }
    return ok();
}

// A topic's subject has to agree with what the topic's scope says a subject IS.
// Both directions are silence rather than an error when they are wrong: a
// Scope::Account publish with no subject fans out to nobody, and a Scope::Global
// publish with one writes a row no subscriber's `$or` branch will ever match.
[[nodiscard]] Status check_scope(const TopicSpec& spec, const TopicRef& topic) noexcept {
    const bool scoped = is_scoped(topic);
    if (spec.scope == Scope::Global) {
        return scoped ? fail(ErrorCode::ValidationFailed, f::kSubject) : ok();
    }
    return scoped ? ok() : fail(ErrorCode::ValidationFailed, f::kSubject);
}

[[nodiscard]] ParamSet own_params(std::span<const Param> params) {
    ParamSet out{};
    for (const Param& param : params) {
        StoredParam stored{};
        stored.name = param.name;
        stored.type = param.type;
        if (param.type == ParamType::Number) {
            stored.number = param.number;
        } else {
            // COPIED, not viewed. The row outlives this call and crosses a pool
            // boundary before it is rendered; a view into the caller's buffer is
            // the dangling string_view ENGINEERING_RULES.md §2.2 names as the most likely
            // crash in code built on this library.
            stored.text.assign(param.text);
        }
        // Cannot fail: check_params already bounded the count at kMaxParams,
        // which is ParamSet's capacity.
        static_cast<void>(out.push(std::move(stored)));
    }
    return out;
}

[[nodiscard]] db::TimeMs retention_expiry(db::TimeMs created_at, std::uint32_t days) noexcept {
    // Widened to int64 before the multiply. `retention_days` is a uint32 from a
    // compile-time table, and days * 86400 * 1000 overflows int32 at 25 days —
    // the arithmetic is checked because §5 says size and offset arithmetic is,
    // not because this particular table is likely to be hostile.
    const std::int64_t seconds = static_cast<std::int64_t>(days) * kSecondsPerDay;
    return created_at + std::chrono::milliseconds{seconds * kMillisPerSecond};
}

}  // namespace

std::array<std::uint8_t, 16> PublishService::dedupe_key(const TopicSpec& spec,
                                                        const TopicRef& topic, TemplateId tpl,
                                                        std::string_view idempotency_key,
                                                        db::TimeMs now) noexcept {
    // A stack string would be better still, but the material is variable-length
    // (the idempotency key) and this runs once per publish rather than per
    // recipient. One small allocation against one hash.
    std::string material;
    material.reserve(64 + idempotency_key.size());

    if (spec.coalesce_window_s != 0) {
        material.push_back(static_cast<char>(kCoalesceDomain));
        material.push_back(static_cast<char>(spec.code));
        append_uuid_bytes(material, topic.subject);
        material.push_back(static_cast<char>(tpl));

        const std::int64_t window_ms =
            static_cast<std::int64_t>(spec.coalesce_window_s) * kMillisPerSecond;
        const std::int64_t at = now.time_since_epoch().count();
        // Floor division, so an instant before the epoch — a badly set clock —
        // still maps monotonically instead of folding two buckets into one. The
        // same arithmetic, for the same reason, as timer::recurrence_bucket.
        const std::int64_t quotient = at / window_ms;
        const std::int64_t bucket = (at % window_ms < 0) ? quotient - 1 : quotient;
        append_le(material, static_cast<std::uint64_t>(bucket), 8);
    } else {
        material.push_back(static_cast<char>(kIdempotencyDomain));
        material.push_back(static_cast<char>(spec.code));
        append_uuid_bytes(material, topic.subject);
        material.append(idempotency_key);
    }

    return truncate_to_128(crypto::sha256(material));
}

Result<NotificationRow> PublishService::build_row(const PublishRequest& request,
                                                  const TopicSpec& spec, db::TimeMs now) const {
    if (const Status scope = check_scope(spec, request.topic); !scope) { return scope.error(); }

    const TemplateSpec* tpl = template_of(templates_, request.tpl);
    if (tpl == nullptr) { return fail(ErrorCode::ValidationFailed, f::kTemplate); }
    if (const Status params = check_params(*tpl, request.params); !params) {
        return params.error();
    }

    // Required rather than optional, and refused rather than defaulted. Every
    // queue in this system is at-least-once, so a publish IS retried; a key this
    // call invented would be different on the retry and the retry would be a
    // second notification. A coalescing topic derives its key from the window
    // instead and has no use for one.
    if (spec.coalesce_window_s == 0 && request.idempotency_key.empty()) {
        return fail(ErrorCode::ValidationFailed, f::kDedupe);
    }

    NotificationRow row{};
    // UUIDv7: the id is the inbox cursor and the broadcast sort key, so a
    // time-ordered value appends to the index rather than splitting random B-tree
    // pages on every insert.
    row.id = uuid::generate_v7();
    row.params = own_params(request.params);
    row.subject = is_scoped(request.topic) ? std::optional<Uuid>{request.topic.subject}
                                           : std::optional<Uuid>{};
    row.ref = request.ref;
    row.actor = request.actor;
    row.dispatched_at = std::nullopt;
    row.created_at = now;
    row.expires_at = retention_expiry(now, spec.retention_days);
    row.dedupe = dedupe_key(spec, request.topic, request.tpl, request.idempotency_key, now);
    row.count = 1;
    row.kind = spec.code;
    row.tpl = request.tpl;
    row.channels = request.channels;
    return row;
}

Result<PublishOutcome> PublishService::publish(mongocxx::client& client,
                                               const PublishRequest& request,
                                               db::TimeMs now) const {
    const TopicSpec* spec = topic_spec(repository_.topics(), request.topic.kind);
    if (spec == nullptr) { return fail(ErrorCode::ValidationFailed, f::kKind); }

    // ONE sample, before anything is written, deciding both gates. Two samples a
    // few microseconds apart could disagree, and a publish admitted under one
    // number and deferred under another is a decision nobody made.
    const ShedVerdict verdict = shed_decision(*spec);
    if (verdict == ShedVerdict::Drop) {
        // Counted before it is returned, because a shed nobody counts is silent
        // data loss — which is the whole reason this gate ships with its metric
        // rather than acquiring one later.
        analytics::count(analytics::Internal::NotificationsShed,
                         analytics::ShedOutcome::Dropped);
        // "Shed deliberately, may be retried", which is what this code is for.
        // Nothing has been written, so a retry is a fresh publish rather than one
        // the dedupe index will refuse.
        return fail(ErrorCode::ServiceUnavailable, f::kKind);
    }

    Result<NotificationRow> built = build_row(request, *spec, now);
    if (!built) { return built.error(); }
    const NotificationRow row = std::move(built).value();

    PublishOutcome outcome{row.id, 0, 1, true};
    if (spec->coalesce_window_s != 0) {
        const Result<CoalesceOutcome> coalesced = repository_.coalesce_notification(client, row);
        if (!coalesced) { return coalesced.error(); }
        outcome.id = coalesced.value().id;
        outcome.count = coalesced.value().count;
        outcome.created = coalesced.value().created;
    } else if (const Status inserted = repository_.insert_notification(client, row); !inserted) {
        if (inserted.error().code != ErrorCode::Conflict) { return inserted.error(); }
        // The unique dedupe index refused a retry. That is the idempotency
        // guarantee working, not a failure — but the caller still needs the id of
        // the row that won, and that row may be the one whose publish died before
        // it dispatched.
        const Result<std::optional<NotificationRow>> existing =
            repository_.find_by_dedupe(client, row.dedupe, now);
        if (!existing) { return existing.error(); }
        // Refused by the index and then absent from the read: the row expired in
        // between. There is nothing to return and nothing useful to retry into.
        if (!existing.value().has_value()) { return fail(ErrorCode::Conflict, f::kDedupe); }
        outcome.id = existing.value()->id;
        outcome.count = existing.value()->count;
        outcome.created = false;
    }

    // Gate 2. The row is committed and `dispatched_at` is unset, which is
    // byte-for-byte the state a process killed between commit and enqueue leaves
    // — so this needs no recovery path of its own, it reuses the one that has
    // been there since the subsystem landed.
    //
    // `delivered` stays 0, and that means "not yet" rather than "nobody".
    if (verdict == ShedVerdict::DeferDispatch) {
        analytics::count(analytics::Internal::NotificationsShed,
                         analytics::ShedOutcome::Deferred);
        return outcome;
    }

    // A failure here is NOT a failed publish. The row is committed, which is the
    // part that had to be durable; the sweeper finds it and finishes the tail.
    // Reporting an error to the caller would invite a retry that the dedupe index
    // would refuse anyway.
    const Result<std::int64_t> delivered = dispatch(client, outcome.id, now);
    if (delivered) { outcome.delivered = delivered.value(); }
    return outcome;
}

Result<PublishOutcome> PublishService::publish(mongocxx::client& client,
                                               mongocxx::client_session& session,
                                               const PublishRequest& request,
                                               db::TimeMs now) const {
    const TopicSpec* spec = topic_spec(repository_.topics(), request.topic.kind);
    if (spec == nullptr) { return fail(ErrorCode::ValidationFailed, f::kKind); }

    // Coalescing and transactions do not compose, and the failure is worth naming
    // rather than papering over. Coalescing is an upsert with `$inc` whose whole
    // purpose is that two concurrent publishes BOTH land on one row; inside a
    // transaction the second is a write conflict that aborts the caller's
    // transaction — so the notification would take the row it was about down with
    // it. Refusing at the boundary is the only honest answer: the caller either
    // publishes this topic outside its transaction, or declares a topic that does
    // not coalesce.
    if (spec->coalesce_window_s != 0) { return fail(ErrorCode::ValidationFailed, f::kKind); }

    Result<NotificationRow> built = build_row(request, *spec, now);
    if (!built) { return built.error(); }
    const NotificationRow row = std::move(built).value();

    if (const Status inserted = repository_.insert_notification(client, session, row);
        !inserted) {
        return inserted.error();
    }

    // Deliberately NOT dispatched. The row is invisible to any other process
    // until the caller commits, and a transport enqueued for a transaction that
    // then aborts is a notification about something that never happened. The
    // caller dispatches after the commit; if it dies first, the sweeper does.
    return PublishOutcome{row.id, 0, 1, true};
}

ShedVerdict PublishService::shed_decision(const TopicSpec& spec) const noexcept {
    if (!hooks_.pressure) { return ShedVerdict::Proceed; }
    float pressure = 0.0F;
    try {
        pressure = hooks_.pressure();
    } catch (...) {
        // A probe that throws is a consumer's bug, and the safe answer to a
        // number nobody can read is the same as the answer to a NaN one: never
        // throw a notification away over it. Not logged, because the storm that
        // makes a probe misbehave is exactly the traffic under which a line per
        // publish is itself the outage (docs/00-architecture.md §9).
        return ShedVerdict::Proceed;
    }
    return shed_verdict(spec, shedding_, pressure);
}

Status PublishService::sample_outbox_depth(mongocxx::client& client, db::TimeMs now,
                                           std::chrono::seconds grace) const {
    const db::TimeMs older_than =
        now - std::chrono::duration_cast<std::chrono::milliseconds>(grace);
    const Result<std::int64_t> depth =
        repository_.count_undispatched(client, older_than, kOutboxDepthCap);
    if (!depth) { return depth.error(); }
    // A failed sample leaves the previous value standing rather than writing a
    // zero: a gauge that reports "empty" because the read failed is the one
    // reading an operator must never get from this metric.
    analytics::sample(analytics::Internal::NotificationsOutboxRows,
                      static_cast<std::uint64_t>(depth.value()));
    return ok();
}

Result<std::int64_t> PublishService::fan_out_targeted(mongocxx::client& client,
                                                      const TopicSpec& spec,
                                                      const NotificationRow& row) const {
    // A permission-gated topic with no probe to ask is a configuration mistake,
    // and it fails CLOSED. Delivering to everyone because nobody supplied the
    // check is the one outcome that cannot be walked back — the recipients have
    // already read it.
    if (spec.required.any() && !hooks_.may_receive) {
        return fail(ErrorCode::Internal, f::kKind);
    }

    const TopicRef topic = row.subject.has_value() ? scoped_topic(spec.code, *row.subject)
                                                   : global_topic(spec.code);

    // Sized to the write batch rather than to the audience: this buffer is
    // flushed and reused, so a topic with thousands of subscribers never
    // materialises thousands of ids at once.
    std::vector<Uuid> batch;
    batch.reserve(kFanoutBatch);
    std::int64_t created = 0;

    const auto flush = [&]() -> Status {
        if (batch.empty()) { return ok(); }
        const Result<std::int64_t> written =
            repository_.fan_out(client, row.id, row.kind, batch, row.expires_at);
        if (!written) { return written.error(); }
        created += written.value();
        batch.clear();
        return ok();
    };

    // Scope::Account is the one audience that needs no scan: the subject IS the
    // reader. Resolving it by querying for a subscription would also LOSE it —
    // an account with no client row has never subscribed to anything, and a
    // security alert it cannot receive is the precise state an attacker wants.
    if (spec.scope == Scope::Account) {
        const Uuid& recipient = *row.subject;
        if (spec.required.any()) {
            const Result<bool> allowed = hooks_.may_receive(client, recipient, spec.required);
            if (!allowed) { return allowed.error(); }
            if (!allowed.value()) { return std::int64_t{0}; }
        }
        // The preference is consulted only when the topic permits it. A topic
        // that is not user_optional cannot be muted by a stored mask, which is
        // what `should_deliver` exists to guarantee — and an account with no
        // client row has expressed no preference, so it is delivered to.
        if (spec.user_optional) {
            const Result<std::optional<ClientRow>> inapp =
                repository_.inapp_client(client, recipient);
            if (!inapp) { return inapp.error(); }
            if (inapp.value().has_value() &&
                !should_deliver(spec, ClientType::InApp, row.channels, inapp.value()->prefs)) {
                return std::int64_t{0};
            }
        }
        batch.push_back(recipient);
        if (const Status flushed = flush(); !flushed) { return flushed.error(); }
        return created;
    }

    // Paged on the client `_id`, bounded per round trip. The total is bounded
    // only by the subscription count, which is what an application declares when
    // it marks a topic FanOut::Write — topic_spec.h states the cost of that
    // choice and this is where it is paid.
    std::optional<Uuid> after;
    for (;;) {
        const Result<std::vector<ClientTarget>> page =
            repository_.subscribers(client, topic, after, kSubscriberPage);
        if (!page) { return page.error(); }
        if (page.value().empty()) { break; }

        for (const ClientTarget& target : page.value()) {
            after = target.id;
            // An inbox row is keyed by a USER, so an endpoint with no owner — a
            // system-level webhook — has no inbox to write to. It still receives
            // the transport; that is the outbound sender's scan, not this one.
            if (target.type != ClientType::InApp || !target.owner.has_value()) { continue; }
            if (!should_deliver(spec, ClientType::InApp, row.channels, target.prefs)) {
                continue;
            }
            if (spec.required.any()) {
                const Result<bool> allowed =
                    hooks_.may_receive(client, *target.owner, spec.required);
                if (!allowed) { return allowed.error(); }
                if (!allowed.value()) { continue; }
            }
            batch.push_back(*target.owner);
            if (batch.size() >= kFanoutBatch) {
                if (const Status flushed = flush(); !flushed) { return flushed.error(); }
            }
        }
        if (page.value().size() < static_cast<std::size_t>(kSubscriberPage)) { break; }
    }

    if (const Status flushed = flush(); !flushed) { return flushed.error(); }
    return created;
}

Result<std::int64_t> PublishService::dispatch(mongocxx::client& client, const Uuid& id,
                                              db::TimeMs now) const {
    const Result<std::optional<NotificationRow>> found =
        repository_.find_notification(client, id, now);
    if (!found) { return found.error(); }
    // Expired between the sweep's read and this call. Not an error: there is
    // nothing left to deliver and nothing a retry would recover.
    if (!found.value().has_value()) { return std::int64_t{0}; }
    const NotificationRow& row = *found.value();

    const TopicSpec* spec = topic_spec(repository_.topics(), row.kind);
    // A stored code this build does not declare — the rolling-deploy state a
    // newer process writing a topic an older one has never heard of produces.
    // Internal rather than ValidationFailed, and deliberately NOT marked
    // dispatched: the row stays in the outbox for a process that does declare it.
    if (spec == nullptr) { return fail(ErrorCode::Internal, f::kKind); }

    std::int64_t delivered = 0;
    if (spec->fanout == FanOut::Write) {
        const Result<std::int64_t> written = fan_out_targeted(client, *spec, row);
        if (!written) { return written.error(); }
        delivered = written.value();
    }

    // Broadcast topics write no inbox rows at all — a subscriber's view is a
    // range query over the topics they subscribe to, which is the entire point of
    // FanOut::Read.

    if (hooks_.enqueue_transports) {
        if (const Status enqueued = hooks_.enqueue_transports(row.id, row.channels); !enqueued) {
            return enqueued.error();
        }
    }

    // LAST, and only once everything above has succeeded. A sweeper re-enqueuing
    // a delivery is harmless because every step above is idempotent; marking
    // first and then failing loses the delivery with no record that it was owed.
    if (const Status marked = repository_.mark_dispatched(client, row.id, now); !marked) {
        return marked.error();
    }
    return delivered;
}

Result<std::size_t> PublishService::sweep_outbox(mongocxx::client& client, db::TimeMs now,
                                                 std::int32_t limit,
                                                 std::chrono::seconds grace) const {
    const db::TimeMs older_than = now - std::chrono::duration_cast<std::chrono::milliseconds>(grace);
    const Result<std::vector<Uuid>> stranded = repository_.undispatched(client, older_than, limit);
    if (!stranded) { return stranded.error(); }

    std::size_t dispatched = 0;
    for (const Uuid& id : stranded.value()) {
        // One failure does not abandon the rest of the batch: these rows are
        // already stranded, and a single unresolvable one — a topic this build
        // does not declare — would otherwise block every row behind it forever.
        // A row that fails stays undispatched and the next sweep sees it again.
        if (dispatch(client, id, now)) { ++dispatched; }
    }
    return dispatched;
}

}  // namespace anvil::notifications
