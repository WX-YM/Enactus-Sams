#include "anvil/notifications/inbox.h"

#include <algorithm>
#include <array>
#include <utility>

#include "anvil/core/uuid.h"

namespace anvil::notifications {
namespace {

namespace f = notification_fields;

// A row's stored parameters as the renderer wants them.
//
// The Params BORROW from `row`, which is why this returns into a caller-owned
// array rather than a container of its own: every view here is valid only while
// the NotificationRow it came from is, and the render that consumes them happens
// before the row is released.
[[nodiscard]] std::size_t borrow_params(const NotificationRow& row, const TopicSpec& spec,
                                        std::array<Param, kMaxParams>& out) noexcept {
    std::size_t count = 0;
    for (const StoredParam& stored : row.params.view()) {
        if (count >= out.size()) { break; }
        // The reserved letter on a coalescing topic is bound below, from the
        // row's own count. A stored one is whatever the FIRST publish in the
        // window wrote, which is 1 forever.
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

// Newest first. Both id spaces are UUIDv7 on the same wall clock, so one
// comparison orders entries that came out of two different collections.
[[nodiscard]] bool newer_first(const InboxEntry& a, const InboxEntry& b) noexcept {
    return a.id > b.id;
}

}  // namespace

SubscriptionSet InboxService::effective_subscriptions(std::span<const TopicSpec> topics,
                                                      const SubscriptionSet& subs,
                                                      const Preferences& prefs) {
    SubscriptionSet out{};
    for (const Subscription& sub : subs.view()) {
        const TopicSpec* spec = topic_spec(topics, sub.kind);
        // A subscription to a topic this build does not declare is dropped rather
        // than guessed at — the same direction publish takes for an unknown code.
        if (spec == nullptr) { continue; }
        // kDefaultChannels, so the topic's own declaration decides whether the
        // in-app channel is even one of its channels, and the stored preference
        // decides the rest. A topic that is not user_optional is kept regardless,
        // which is `should_deliver` doing the job it exists for.
        if (!should_deliver(*spec, ClientType::InApp, kDefaultChannels, prefs)) { continue; }
        static_cast<void>(out.push(sub));
    }
    return out;
}

Result<InboxEntry> InboxService::render_entry(const NotificationRow& row, const TopicSpec& spec,
                                              Locale locale, const Uuid& cursor_id,
                                              bool read) const {
    std::array<Param, kMaxParams> params{};
    const std::size_t count = borrow_params(row, spec, params);

    Result<Rendered> content =
        render(templates_, row.tpl, locale, std::span<const Param>{params.data(), count});
    if (!content) { return content.error(); }

    InboxEntry entry{};
    entry.content = std::move(content).value();
    entry.id = cursor_id;
    entry.notification = row.id;
    entry.ref = row.ref;
    entry.actor = row.actor;
    entry.created_at = row.created_at;
    entry.count = row.count;
    entry.kind = row.kind;
    entry.read = read;
    return entry;
}

Result<InboxPageView> InboxService::page(mongocxx::client& client, const Uuid& reader,
                                         Locale locale, const std::optional<Uuid>& cursor,
                                         std::int32_t limit, db::TimeMs now) const {
    const std::int32_t bounded = std::clamp(limit, kMinInboxPageSize, kMaxInboxPageSize);

    // One read for the subscriptions, the preferences and the watermark. They
    // live on the same row precisely so the read path needs one round trip for
    // all three rather than three for one each.
    const Result<std::optional<ClientRow>> inapp = repository_.inapp_client(client, reader);
    if (!inapp) { return inapp.error(); }

    SubscriptionSet subs{};
    std::optional<Uuid> watermark;
    if (inapp.value().has_value()) {
        const ClientRow& row = *inapp.value();
        subs = effective_subscriptions(repository_.topics(), row.subs, row.prefs);
        watermark = row.broadcast_watermark;
    }

    std::vector<InboxEntry> entries;
    entries.reserve(static_cast<std::size_t>(bounded) * 2);
    bool more_behind = false;

    // --- the targeted half ---------------------------------------------------
    const Result<InboxPage> targeted =
        repository_.page_inbox(client, reader, cursor, bounded, now);
    if (!targeted) { return targeted.error(); }
    more_behind = more_behind || targeted.value().next_cursor.has_value();

    if (!targeted.value().rows.empty()) {
        std::vector<Uuid> ids;
        ids.reserve(targeted.value().rows.size());
        for (const InboxRow& row : targeted.value().rows) { ids.push_back(row.nid); }

        // ONE query with an `$in`, never one per row: a page of 25 point reads is
        // 25 round trips for a response budgeted in single-digit milliseconds.
        const Result<std::vector<NotificationRow>> canonical =
            repository_.find_notifications(client, ids, now);
        if (!canonical) { return canonical.error(); }

        for (const InboxRow& inbox_row : targeted.value().rows) {
            const auto found = std::find_if(
                canonical.value().begin(), canonical.value().end(),
                [&inbox_row](const NotificationRow& row) { return row.id == inbox_row.nid; });
            // The canonical row expired while its inbox row has not. The TTLs are
            // set from the same retention, so this is the monitor reaching one
            // collection before the other — there is nothing to render and the
            // entry is simply absent.
            if (found == canonical.value().end()) { continue; }
            const TopicSpec* spec = topic_spec(repository_.topics(), found->kind);
            if (spec == nullptr) { continue; }

            Result<InboxEntry> entry = render_entry(*found, *spec, locale, inbox_row.id,
                                                    inbox_row.read_at.has_value());
            if (!entry) { return entry.error(); }
            entries.push_back(std::move(entry).value());
        }
    }

    // --- the broadcast half --------------------------------------------------
    const Result<BroadcastPage> broadcast =
        repository_.page_broadcast(client, subs.view(), cursor, bounded, now);
    if (!broadcast) { return broadcast.error(); }
    more_behind = more_behind || broadcast.value().next_cursor.has_value();

    for (const NotificationRow& row : broadcast.value().rows) {
        const TopicSpec* spec = topic_spec(repository_.topics(), row.kind);
        if (spec == nullptr) { continue; }
        // The publish's own narrowing, which the subscription filter above cannot
        // see because it is per row rather than per topic.
        if (!has_channel(row.channels == kDefaultChannels ? spec->default_channels
                                                          : row.channels,
                         ClientType::InApp)) {
            continue;
        }
        // At or below the watermark is read. A watermark rather than a row per
        // reader per broadcast, because per-row read state would reintroduce
        // exactly the fan-out-on-write cost this strategy exists to avoid.
        const bool read = watermark.has_value() && row.id <= *watermark;

        Result<InboxEntry> entry = render_entry(row, *spec, locale, row.id, read);
        if (!entry) { return entry.error(); }
        entries.push_back(std::move(entry).value());
    }

    // --- merge, truncate, then suppress --------------------------------------
    //
    // In that order. Suppressing first would run the visibility probe over rows
    // the page was never going to show, which is up to `limit` extra round trips
    // to hide something nobody was going to see.
    std::sort(entries.begin(), entries.end(), newer_first);
    if (entries.size() > static_cast<std::size_t>(bounded)) {
        entries.resize(static_cast<std::size_t>(bounded));
        more_behind = true;
    }

    InboxPageView view{};
    // Taken from the last entry BEFORE suppression removes any of them: a cursor
    // derived from a surviving entry would re-serve every suppressed row on the
    // next page, and the page after that, forever.
    if (more_behind && !entries.empty()) { view.next_cursor = entries.back().id; }

    std::vector<Uuid> forget;
    view.entries.reserve(entries.size());
    for (InboxEntry& entry : entries) {
        if (!entry.ref.has_value()) {
            view.entries.push_back(std::move(entry));
            continue;
        }
        // A ref with no probe to ask is refused rather than shown. Showing
        // something nobody checked cannot be walked back once it has been read.
        bool visible = false;
        if (hooks_.may_see) {
            const Result<bool> allowed = hooks_.may_see(client, reader, *entry.ref);
            if (!allowed) { return allowed.error(); }
            visible = allowed.value();
        }
        if (visible) {
            view.entries.push_back(std::move(entry));
            continue;
        }
        // Only the targeted half has a row to delete. A broadcast entry is one
        // row shared by everybody, so it is hidden from this reader and left
        // exactly where it is.
        if (entry.id != entry.notification) { forget.push_back(entry.id); }
    }

    if (!forget.empty()) {
        // Best effort, deliberately: the rows are already hidden, and failing the
        // whole page because a cleanup write did not land would turn a tidying
        // operation into an outage. The next page sees them again and tries once
        // more.
        static_cast<void>(repository_.forget_inbox_rows(client, reader, forget));
    }
    return view;
}

Result<std::int64_t> InboxService::unread(mongocxx::client& client, const Uuid& reader,
                                          db::TimeMs now) const {
    const Result<std::int64_t> targeted = repository_.count_targeted_unread(client, reader, now);
    if (!targeted) { return targeted.error(); }

    const Result<std::optional<ClientRow>> inapp = repository_.inapp_client(client, reader);
    if (!inapp) { return inapp.error(); }
    // No client row means no subscriptions, so the broadcast half is empty. It
    // does not mean no notifications: a targeted topic delivers to a reader who
    // has never registered anything.
    if (!inapp.value().has_value()) { return std::min(targeted.value(), kUnreadCountLimit); }

    const ClientRow& row = *inapp.value();
    const SubscriptionSet subs =
        effective_subscriptions(repository_.topics(), row.subs, row.prefs);
    const Result<std::int64_t> broadcast = repository_.count_broadcast_unread(
        client, subs.view(), row.broadcast_watermark, now);
    if (!broadcast) { return broadcast.error(); }

    // Each half is already capped, so the sum is bounded whatever the backlog.
    return std::min(targeted.value() + broadcast.value(), kUnreadCountLimit);
}

Status InboxService::mark_read(mongocxx::client& client, const Uuid& reader, const Uuid& up_to,
                               db::TimeMs at) const {
    const Result<std::int64_t> marked = repository_.mark_read_up_to(client, reader, up_to, at);
    if (!marked) { return marked.error(); }

    // The SAME watermark for both halves. They are two storage strategies for one
    // stream, and a reader who marked the stream read up to a point has marked
    // both halves of it — advancing one and not the other leaves a badge that
    // will not clear and no way for the reader to find what is keeping it lit.
    //
    // Unconditional: the watermark write is a `$max`, so a request carrying an
    // older marker cannot walk it backwards.
    return repository_.advance_watermark(client, reader, up_to);
}

Result<std::int64_t> InboxService::mark_read(mongocxx::client& client, const Uuid& reader,
                                             std::span<const Uuid> ids, db::TimeMs at) const {
    return repository_.mark_read_ids(client, reader, ids, at);
}

}  // namespace anvil::notifications
