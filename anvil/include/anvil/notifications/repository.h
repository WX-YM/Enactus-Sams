#pragma once

// The `notifications`, `notification_inbox` and `notification_clients`
// collections.
//
// Three collections, ONE repository, because they are one subsystem and every
// interesting operation touches two of them: a publish inserts a canonical row
// and then either fans out to inboxes or does not, and a read merges the two.
// Splitting them across three classes would put the seam in the wrong place.
//
// This layer encodes and decodes. It does not decide what may be published, who
// may subscribe, or what a reader may still see — those are the publisher's, and
// the read-time access recheck is the application's.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/repository.h"
#include "anvil/notifications/record.h"
#include "anvil/notifications/topic_spec.h"

namespace anvil::notifications {

// The field names are PUBLISHED so the application's index catalogue can name
// them. An index over a column anvil does not write is an index the planner never
// uses, and the symptom is a collection scan rather than a compile error.
namespace notification_fields {

// --- notifications -----------------------------------------------------------
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kKind = "kind";
inline constexpr std::string_view kSubject = "subject";
inline constexpr std::string_view kTemplate = "tpl";
inline constexpr std::string_view kParams = "params";
inline constexpr std::string_view kRef = "ref";
inline constexpr std::string_view kRefKind = "k";
inline constexpr std::string_view kRefId = "id";
inline constexpr std::string_view kActor = "actor";
inline constexpr std::string_view kDedupe = "dedupe";
inline constexpr std::string_view kCount = "count";
inline constexpr std::string_view kCreatedAt = "created_at";
inline constexpr std::string_view kExpiresAt = "expires_at";
inline constexpr std::string_view kDispatchedAt = "disp_at";
inline constexpr std::string_view kChannels = "ch";

// --- notification_inbox ------------------------------------------------------
inline constexpr std::string_view kUid = "uid";
inline constexpr std::string_view kNid = "nid";
inline constexpr std::string_view kReadAt = "read_at";

// --- notification_clients ----------------------------------------------------
inline constexpr std::string_view kOwner = "owner";
inline constexpr std::string_view kType = "type";
inline constexpr std::string_view kAddr = "addr";
inline constexpr std::string_view kKeys = "keys";
inline constexpr std::string_view kSubs = "subs";
inline constexpr std::string_view kSubKind = "kind";
inline constexpr std::string_view kSubSubject = "subject";
inline constexpr std::string_view kSubSince = "since";
inline constexpr std::string_view kPrefs = "prefs";
inline constexpr std::string_view kFailures = "fail_n";
inline constexpr std::string_view kDisabledAt = "disabled_at";
inline constexpr std::string_view kLastDeliveryAt = "l_at";
inline constexpr std::string_view kLastVerdict = "l_v";
inline constexpr std::string_view kDigest = "digest";
inline constexpr std::string_view kWatermark = "bcast";

}  // namespace notification_fields

// What a coalescing upsert did. `created` is what decides whether anything is
// fanned out or delivered: a merged repeat is a count on a row somebody has
// already been told about.
struct CoalesceOutcome final {
    Uuid         id;
    std::int32_t count;
    bool         created;
};

class NotificationRepository final : public repo::RepositoryBase {
public:
    // `topics` is a view of the application's `constexpr` table. The repository
    // needs it to build the broadcast read-merge, which must include only the
    // fan-out-on-read topics a reader is subscribed to — a targeted topic in that
    // `$or` would return rows that were already delivered as inbox entries.
    NotificationRepository(std::string database, NotificationCollections collections,
                           std::span<const TopicSpec> topics) noexcept
        : RepositoryBase{std::move(database), collections.notifications},
          inbox_{collections.inbox},
          clients_{collections.clients},
          topics_{topics} {}

    [[nodiscard]] std::string_view inbox_collection() const noexcept { return inbox_; }
    [[nodiscard]] std::string_view clients_collection() const noexcept { return clients_; }
    [[nodiscard]] std::span<const TopicSpec> topics() const noexcept { return topics_; }

    // --- notifications -------------------------------------------------------

    // One insert. Returns Conflict when the dedupe key already exists, which is
    // the unique sparse index turning a retried publish into a no-op rather than
    // a second notification.
    //
    // The session overload exists because a publish participates in the caller's
    // transaction: a row that fails to save must not produce a notification about
    // it. Two overloads rather than a defaulted pointer, for the reason
    // db/versioned.h gives — a session is not optional to the correctness of the
    // write for any caller that has one.
    [[nodiscard]] Status insert_notification(mongocxx::client& client,
                                             const NotificationRow& row) const;
    [[nodiscard]] Status insert_notification(mongocxx::client& client,
                                             mongocxx::client_session& session,
                                             const NotificationRow& row) const;

    // Coalescing. ONE find_one_and_update on the dedupe key with `$inc` and an
    // upsert — atomic, so it needs no read-then-write and two concurrent
    // publishes inside the window produce one row with a count of two.
    [[nodiscard]] Result<CoalesceOutcome> coalesce_notification(mongocxx::client& client,
                                                                const NotificationRow& row) const;

    [[nodiscard]] Result<std::optional<NotificationRow>> find_notification(
        mongocxx::client& client, const Uuid& id, db::TimeMs now) const;

    // The idempotent-retry read, covered by the same unique index that refused
    // the second insert. A publish that loses to `ntf_dedupe_unique` has to be
    // able to answer with the id of the row that won: "already published" and
    // "published as this id" are the same answer to a caller that is itself being
    // retried, and a bare Conflict would make it impossible to dispatch the row
    // the winner left undispatched.
    [[nodiscard]] Result<std::optional<NotificationRow>> find_by_dedupe(
        mongocxx::client& client, const std::array<std::uint8_t, 16>& dedupe,
        db::TimeMs now) const;

    // The targeted half of the inbox merge needs the canonical row behind each
    // entry — the template id and the params live there, never on the inbox row.
    // ONE query per page with an `$in`, never one per row: a page issuing 25
    // point reads would be 25 round trips for a response budgeted at 8 ms.
    [[nodiscard]] Result<std::vector<NotificationRow>> find_notifications(
        mongocxx::client& client, std::span<const Uuid> ids, db::TimeMs now) const;

    // The broadcast half of the inbox merge. One indexed range per subscribed
    // (kind, subject) pair, each bounded by its own `since` marker and by the
    // cursor, all ordered by the time-ordered `_id`.
    [[nodiscard]] Result<BroadcastPage> page_broadcast(mongocxx::client& client,
                                                       std::span<const Subscription> subs,
                                                       const std::optional<Uuid>& cursor,
                                                       std::int32_t limit, db::TimeMs now) const;

    // Capped count of broadcast rows past the watermark, so the cost does not
    // grow with the backlog.
    [[nodiscard]] Result<std::int64_t> count_broadcast_unread(
        mongocxx::client& client, std::span<const Subscription> subs,
        const std::optional<Uuid>& watermark, db::TimeMs now) const;

    // The outbox sweeper's read: rows committed but never marked dispatched — the
    // window a process killed between commit and enqueue leaves open.
    // `older_than` keeps the sweeper from racing a publish that is about to
    // enqueue its own transports.
    [[nodiscard]] Result<std::vector<Uuid>> undispatched(mongocxx::client& client,
                                                         db::TimeMs older_than,
                                                         std::int32_t limit) const;

    // How many rows `undispatched` would have to work through, CAPPED at
    // `cap` so the cost does not grow with the backlog — the same shape as
    // count_broadcast_unread, and for the same reason. A returned value at the
    // cap means "at least that many".
    //
    // Exactly the filter `undispatched` uses, so the number describes the queue
    // the sweeper actually walks rather than a near-miss of it.
    [[nodiscard]] Result<std::int64_t> count_undispatched(mongocxx::client& client,
                                                          db::TimeMs older_than,
                                                          std::int64_t cap) const;

    // Marked AFTER the transports are enqueued, never before: a sweeper
    // re-enqueuing a delivery is harmless because every transport is idempotent,
    // while marking first loses the delivery entirely.
    [[nodiscard]] Status mark_dispatched(mongocxx::client& client, const Uuid& id,
                                         db::TimeMs at) const;

    // --- notification_inbox --------------------------------------------------

    // Idempotent through the `{uid, nid}` unique index, which is what lets a
    // partially completed fan-out simply be re-run — exactly what the outbox
    // sweeper does after a crash. Returns how many rows were NEW.
    [[nodiscard]] Result<std::int64_t> fan_out(mongocxx::client& client, const Uuid& nid,
                                               TopicCode kind, std::span<const Uuid> recipients,
                                               db::TimeMs expires_at) const;

    [[nodiscard]] Result<InboxPage> page_inbox(mongocxx::client& client, const Uuid& uid,
                                               const std::optional<Uuid>& cursor,
                                               std::int32_t limit, db::TimeMs now) const;

    [[nodiscard]] Result<std::int64_t> count_targeted_unread(mongocxx::client& client,
                                                             const Uuid& uid,
                                                             db::TimeMs now) const;

    // A WATERMARK, never a server-side `now`: the client sends the highest id it
    // has actually SEEN, so a notification that arrived between the client's
    // render and this request stays unread instead of being silently buried.
    [[nodiscard]] Result<std::int64_t> mark_read_up_to(mongocxx::client& client, const Uuid& uid,
                                                       const Uuid& up_to, db::TimeMs at) const;

    [[nodiscard]] Result<std::int64_t> mark_read_ids(mongocxx::client& client, const Uuid& uid,
                                                     std::span<const Uuid> ids,
                                                     db::TimeMs at) const;

    // Suppression at read time: a notification whose resource the reader can no
    // longer see is hidden ENTIRELY, not shown redacted — a redacted placeholder
    // still discloses that something existed. The visibility check itself is the
    // application's; this only drops the rows it rejected, because a row that
    // will never be visible again is dead weight in the index the unread count
    // rides.
    [[nodiscard]] Result<std::int64_t> forget_inbox_rows(mongocxx::client& client,
                                                         const Uuid& uid,
                                                         std::span<const Uuid> ids) const;

    // --- notification_clients ------------------------------------------------

    [[nodiscard]] Status insert_client(mongocxx::client& client, const ClientRow& row) const;

    [[nodiscard]] Result<std::optional<ClientRow>> find_client(mongocxx::client& client,
                                                               const Uuid& id) const;

    [[nodiscard]] Result<std::vector<ClientRow>> clients_of(mongocxx::client& client,
                                                            const Uuid& owner,
                                                            std::int32_t limit) const;

    // The reader's own in-app client: it holds the subscriptions, the preferences
    // and the broadcast watermark, so every read path needs it.
    [[nodiscard]] Result<std::optional<ClientRow>> inapp_client(mongocxx::client& client,
                                                                const Uuid& owner) const;

    // Versioned: two tabs editing one client's subscriptions must not lose a
    // write, and a subscription silently dropped is a notification nobody
    // receives and nobody can explain.
    [[nodiscard]] Result<std::int64_t> replace_subscriptions(mongocxx::client& client,
                                                             const Uuid& id,
                                                             std::int64_t expected_version,
                                                             const SubscriptionSet& subs) const;

    [[nodiscard]] Result<std::int64_t> replace_preferences(mongocxx::client& client,
                                                           const Uuid& id,
                                                           std::int64_t expected_version,
                                                           const Preferences& prefs) const;

    [[nodiscard]] Result<std::int64_t> replace_enabled(mongocxx::client& client, const Uuid& id,
                                                       std::int64_t expected_version,
                                                       bool enabled, db::TimeMs at) const;

    // The fan-out scan. Bounded and cursor-paged on `_id`, so a topic with 5 000
    // subscribers is forty bounded reads rather than one unbounded one.
    [[nodiscard]] Result<std::vector<ClientTarget>> subscribers(
        mongocxx::client& client, const TopicRef& topic, const std::optional<Uuid>& after,
        std::int32_t limit) const;

    // The enabled WebPush endpoints of a page of accounts, in ONE `$in` on the
    // owner/type index. For a sender whose audience is a list of accounts rather
    // than a topic's subscribers — a chat nudge (chat/push.h) — so a page of a
    // hundred recipients is one read and not a hundred. Projected as the
    // subscriber scan is.
    [[nodiscard]] Result<std::vector<ClientTarget>> push_targets(mongocxx::client& client,
                                                                 std::span<const Uuid> owners,
                                                                 std::int32_t limit) const;

    // The soft-failure streak, and the last attempt, in ONE write. A second write
    // beside it would double the delivery path's write volume to record when
    // something the first write already described happened.
    [[nodiscard]] Result<std::int32_t> record_delivery_failure(mongocxx::client& client,
                                                               const Uuid& id,
                                                               DeliveryVerdict verdict,
                                                               db::TimeMs at) const;

    [[nodiscard]] Status clear_delivery_failures(mongocxx::client& client, const Uuid& id,
                                                 db::TimeMs at) const;

    [[nodiscard]] Status disable_client(mongocxx::client& client, const Uuid& id, db::TimeMs at,
                                        DeliveryVerdict verdict) const;

    [[nodiscard]] Result<bool> delete_client(mongocxx::client& client, const Uuid& id,
                                             const std::optional<Uuid>& owner) const;

    // Every webhook, for an operator's screen. A projection that HAS NO FIELD for
    // the sealed secret, which is what makes "the secret is shown once" a property
    // of the type rather than of the handler.
    [[nodiscard]] Result<std::vector<WebhookRow>> list_webhooks(mongocxx::client& client,
                                                                std::int32_t limit) const;

    // --- read state ----------------------------------------------------------

    [[nodiscard]] Status advance_watermark(mongocxx::client& client, const Uuid& owner,
                                           const Uuid& up_to) const;

    [[nodiscard]] Result<ReadState> read_state(mongocxx::client& client,
                                               const Uuid& owner) const;

    // Insert-if-absent, tolerating the duplicate-key error rather than reading
    // first: a read-then-write races two concurrent first reads and the loser
    // silently replaces the winner's subscriptions.
    //
    // `defaults` is what a fresh client is subscribed to. anvil cannot guess it —
    // which topics a new account should receive is the application's policy — and
    // a client subscribed to NOTHING receives nothing, which presents as an inbox
    // that comes back empty because the read had no branch to build rather than
    // because there was no news.
    [[nodiscard]] Result<Uuid> ensure_client(mongocxx::client& client, ClientType type,
                                             const Uuid& owner, std::string_view addr,
                                             std::span<const TopicCode> defaults,
                                             db::TimeMs now) const;

private:
    [[nodiscard]] mongocxx::collection bind_notifications(mongocxx::client& client) const {
        return bind(client);
    }
    [[nodiscard]] mongocxx::collection bind_inbox(mongocxx::client& client) const {
        return client[std::string{database()}][std::string{inbox_}];
    }
    [[nodiscard]] mongocxx::collection bind_clients(mongocxx::client& client) const {
        return client[std::string{database()}][std::string{clients_}];
    }

    // Declaration order is initialisation order; all three outlive every instance.
    std::string_view           inbox_;
    std::string_view           clients_;
    std::span<const TopicSpec> topics_;
};

}  // namespace anvil::notifications
