#include "anvil/notifications/outbound.h"

#include <array>
#include <utility>

namespace anvil::notifications {
namespace {

namespace f = notification_fields;

// The reader's language when the application has not said. Correct for a
// single-locale deployment and wrong in a way somebody notices immediately in any
// other, which is why the hook exists at all.
[[nodiscard]] Locale locale_or_default(const ReaderLocale& hook, mongocxx::client& client,
                                       const std::optional<Uuid>& owner) {
    return hook ? hook(client, owner) : Locale{};
}

// A full ClientRow reduced to what a delivery decision looks at. Only the
// account-scoped path needs this: that audience is one account's handful of
// endpoints, so reading the whole row costs nothing, while the subscriber scan
// pages through hundreds and reads the projection instead.
[[nodiscard]] ClientTarget target_of(const ClientRow& row) {
    ClientTarget target{};
    target.id = row.id;
    target.addr = row.addr;
    target.keys = row.keys;
    target.owner = row.owner;
    target.prefs = row.prefs;
    target.type = row.type;
    target.digest = row.digest;
    return target;
}

[[nodiscard]] std::size_t borrow_params(const NotificationRow& row, const TopicSpec& spec,
                                        std::array<Param, kMaxParams>& out) noexcept {
    std::size_t count = 0;
    for (const StoredParam& stored : row.params.view()) {
        if (count >= out.size()) { break; }
        // Same rule the inbox render follows: on a coalescing topic the count
        // comes from the row and not from a parameter frozen at the first publish.
        if (spec.coalesce_window_s != 0 && stored.name == kCountParam) { continue; }
        out[count] = stored.type == ParamType::Number
                         ? Param::of(stored.name, stored.number)
                         : Param::of(stored.name, std::string_view{stored.text});
        ++count;
    }
    if (spec.coalesce_window_s != 0 && count < out.size()) {
        out[count] = Param::of(kCountParam, static_cast<std::int64_t>(row.count));
        ++count;
    }
    return count;
}

}  // namespace

const Transport* OutboundSender::transport_for(ClientType type) const noexcept {
    switch (type) {
        case ClientType::WebPush: return transports_.web_push ? &transports_.web_push : nullptr;
        case ClientType::Email:   return transports_.email ? &transports_.email : nullptr;
        case ClientType::Webhook: return transports_.webhook ? &transports_.webhook : nullptr;
        // Its delivery IS the inbox row, which was written before any of this
        // ran. There is nothing to send.
        case ClientType::InApp:   return nullptr;
    }
    return nullptr;
}

Status OutboundSender::record(mongocxx::client& client, const ClientTarget& target,
                              DeliveryVerdict verdict, db::TimeMs now,
                              OutboundSummary& summary) const {
    switch (verdict) {
        case DeliveryVerdict::Delivered:
            ++summary.delivered;
            // The streak resets. A transient failure that recovers must not
            // accumulate across weeks into a disable nobody can account for.
            return repository_.clear_delivery_failures(client, target.id, now);

        case DeliveryVerdict::Gone:
        case DeliveryVerdict::Rejected:
            // The endpoint told us it is finished, or its configuration cannot
            // produce a request at all. Neither has anything to retry into, so a
            // streak would only delay believing it.
            ++summary.disabled;
            return repository_.disable_client(client, target.id, now, verdict);

        case DeliveryVerdict::Transient: {
            const Result<std::int32_t> streak =
                repository_.record_delivery_failure(client, target.id, verdict, now);
            if (!streak) { return streak.error(); }
            if (streak.value() < kMaxSoftFailures) { return ok(); }
            // A permanently broken webhook that retries forever is a
            // self-inflicted outbound flood aimed at somebody else's
            // infrastructure.
            ++summary.disabled;
            return repository_.disable_client(client, target.id, now, verdict);
        }
    }
    return fail(ErrorCode::Internal, f::kLastVerdict);
}

Result<bool> OutboundSender::attempt(mongocxx::client& client, const NotificationRow& row,
                                     const TopicSpec& spec, const ClientTarget& target,
                                     ChannelMask channels, db::TimeMs now,
                                     OutboundSummary& summary) const {
    if (!should_deliver(spec, target.type, channels, target.prefs)) { return false; }

    const Transport* transport = transport_for(target.type);
    // A deployment with no SMTP configured has not got a broken mail endpoint, it
    // has no mail. Skipping is right; recording a failure would disable every
    // mailbox in the system after five notifications.
    if (transport == nullptr) { return false; }

    // The same question the publish path asked, asked again: a subscriber who
    // lost the topic's permission between the fan-out and the send must not be
    // mailed about it. A gated topic with no probe fails closed here too.
    if (spec.required.any()) {
        if (!hooks_.may_receive || !target.owner.has_value()) { return false; }
        const Result<bool> allowed =
            hooks_.may_receive(client, *target.owner, spec.required);
        if (!allowed) { return allowed.error(); }
        if (!allowed.value()) { return false; }
    }

    std::array<Param, kMaxParams> params{};
    const std::size_t count = borrow_params(row, spec, params);
    Result<Rendered> content =
        render(templates_, row.tpl, locale_or_default(hooks_.locale_of, client, target.owner),
               std::span<const Param>{params.data(), count});
    // A row that cannot be rendered cannot be sent, and it is the row that is
    // wrong rather than the endpoint — so nothing is recorded against the client.
    if (!content) { return content.error(); }

    Delivery delivery{};
    delivery.content = std::move(content).value();
    delivery.address = target.addr;
    delivery.keys = target.keys;
    delivery.client = target.id;
    delivery.notification = row.id;
    delivery.count = row.count;
    delivery.kind = row.kind;
    delivery.type = target.type;

    ++summary.attempted;
    const Result<DeliveryVerdict> verdict = (*transport)(delivery);
    // A transport that could not form an attempt at all. Recorded as Rejected:
    // a request that was never made cannot be blamed on the endpoint, and will
    // not start working on a retry either.
    const DeliveryVerdict outcome = verdict ? verdict.value() : DeliveryVerdict::Rejected;

    if (const Status recorded = record(client, target, outcome, now, summary); !recorded) {
        return recorded.error();
    }
    return true;
}

Result<OutboundSummary> OutboundSender::send(mongocxx::client& client, const Uuid& notification,
                                             ChannelMask channels, db::TimeMs now) const {
    OutboundSummary summary{};

    const Result<std::optional<NotificationRow>> found =
        repository_.find_notification(client, notification, now);
    if (!found) { return found.error(); }
    // Expired between the dispatch and this job. E-mailing somebody about
    // something their inbox no longer shows is worse than not mailing them.
    if (!found.value().has_value()) { return summary; }
    const NotificationRow& row = *found.value();

    const TopicSpec* spec = topic_spec(repository_.topics(), row.kind);
    // A stored code this build does not declare. Refusing to interpret it is the
    // correct direction — the alternative is guessing which channels and which
    // permission a topic has.
    if (spec == nullptr) { return fail(ErrorCode::Internal, f::kKind); }

    // Scope::Account: the audience is the subject's OWN endpoints, with no
    // subscription row required. The same reasoning as the inbox fan-out — an
    // account that never subscribed to anything must still receive its own
    // sign-in alert, and a scan for subscriptions would find nothing.
    if (spec->scope == Scope::Account && row.subject.has_value()) {
        const Result<std::vector<ClientRow>> mine =
            repository_.clients_of(client, *row.subject, kSubscriberPage);
        if (!mine) { return mine.error(); }
        for (const ClientRow& candidate : mine.value()) {
            // clients_of does NOT filter disabled endpoints — it is an operator's
            // listing as much as a delivery scan — so the filter is here. The
            // subscriber scan excludes them in the query instead.
            if (candidate.disabled_at.has_value()) { continue; }
            const Result<bool> attempted =
                attempt(client, row, *spec, target_of(candidate), channels, now, summary);
            if (!attempted) { return attempted.error(); }
            if (!attempted.value()) { ++summary.skipped; }
        }
        return summary;
    }

    const TopicRef topic = row.subject.has_value() ? scoped_topic(row.kind, *row.subject)
                                                   : global_topic(row.kind);
    std::optional<Uuid> after;
    for (;;) {
        const Result<std::vector<ClientTarget>> page =
            repository_.subscribers(client, topic, after, kSubscriberPage);
        if (!page) { return page.error(); }
        if (page.value().empty()) { break; }

        for (const ClientTarget& target : page.value()) {
            after = target.id;
            // A ceiling on the TOTAL, not on the page. One notification must not
            // occupy a job worker indefinitely; the row stays dispatched because
            // the inbox already has it and the transports are best-effort by
            // construction.
            if (summary.attempted >= kMaxDeliveriesPerSend) { return summary; }

            const Result<bool> attempted =
                attempt(client, row, *spec, target, channels, now, summary);
            if (!attempted) { return attempted.error(); }
            if (!attempted.value()) { ++summary.skipped; }
        }
        if (page.value().size() < static_cast<std::size_t>(kSubscriberPage)) { break; }
    }
    return summary;
}

}  // namespace anvil::notifications
