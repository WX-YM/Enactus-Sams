#pragma once

// The read path: one page of a reader's notifications, merged from two halves
// that are stored completely differently and have to come back looking the same.
//
//     targeted    notification_inbox, one row per recipient, read state per row
//     broadcast   notifications, ONE row for everybody, read state a watermark
//
// A reader does not know or care which of the two a notification came from, so
// the merge is the whole job. Both halves are ordered by a UUIDv7 `_id` on the
// same wall clock, which is what lets ONE cursor bound both: an inbox row's id is
// minted at fan-out and a broadcast row's at publish, and "everything older than
// this id" means the same thing in each.
//
// --- rendered HERE, never at publish ----------------------------------------
//
// A notification stores a template id and parameters. It is rendered in the
// READER'S CURRENT locale at the moment they read it, so a reader who switches to
// Arabic sees the notifications they already received in Arabic, and a typo fixed
// in the table is fixed retroactively for every notification ever sent.
//
// --- suppression is total, never redaction ----------------------------------
//
// A notification whose resource the reader can no longer see is hidden ENTIRELY.
// Not shown with the title removed, not shown as "a post you can no longer
// view" — a redacted placeholder still discloses that something existed, and for
// a staff-facing topic the existence IS the secret. For the targeted half the row
// is also deleted, because a row that will never be visible again is dead weight
// in the index the unread count rides.
//
// A suppressed row leaves the page SHORTER than the limit rather than triggering
// a backfill. Backfilling means re-querying until the page is full, which is a
// loop with no bound against a reader who lost access to a thousand resources —
// and a page that is occasionally short is the cheaper of the two, given the
// client is paging anyway.
//
// --- preferences are applied to the broadcast half AT READ -------------------
//
// The targeted half already had `should_deliver` applied when it fanned out, so
// its rows are ones the reader's preferences allowed at the time. The broadcast
// half writes no per-reader row at all, so there was no moment at which a
// preference could be consulted — muting a broadcast topic has to take effect
// here or it does nothing at all.
//
// --- threading --------------------------------------------------------------
//
// Every method BLOCKS on MongoDB. db_pool, never a Trantor loop thread.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/notifications/record.h"
#include "anvil/notifications/repository.h"
#include "anvil/notifications/template_spec.h"
#include "anvil/notifications/topic_spec.h"

namespace anvil::notifications {

// Can this reader still see the resource this notification points at?
//
// anvil cannot answer it: what a `ResourceKind` of 3 means is the application's,
// and so is what "can see" means for it. What anvil does is hand the ref back
// unchanged and act on the answer.
//
// BLOCKING: db_pool. Called at most once per entry on a page, so an
// implementation that issues a query per call is up to `limit` extra round trips
// on the read path — batch inside the hook if that matters.
//
// Absent means no notification on any topic points at a resource. A ref present
// with no probe to ask is refused rather than shown, for the same reason the
// publish path's permission probe fails closed: showing something nobody checked
// cannot be walked back once it has been read.
using VisibilityProbe =
    std::function<Result<bool>(mongocxx::client& client, const Uuid& reader,
                               const ResourceRef& ref)>;

struct InboxHooks final {
    VisibilityProbe may_see;
};

// One entry as the reader sees it. The rendered strings are OWNED: the row they
// were rendered from is released before this crosses back to the caller's thread,
// and a view into it would dangle (CLAUDE.md §2.2).
struct InboxEntry final {
    Rendered                   content;
    // The MERGE CURSOR, and the id the reader marks read by. It is the inbox
    // row's id for a targeted entry and the notification's for a broadcast one —
    // different collections, one time-ordered id space.
    Uuid                       id;
    // The canonical row, which is what `ref` and `actor` came from. Distinct from
    // `id` for a targeted entry, equal to it for a broadcast one.
    Uuid                       notification;
    std::optional<ResourceRef> ref;
    std::optional<Uuid>        actor;
    db::TimeMs                 created_at;
    // Coalesced repeats. Bound to `kCountParam` at render for a coalescing topic.
    std::int32_t               count;
    TopicCode                  kind;
    bool                       read;
};

struct InboxPageView final {
    std::vector<InboxEntry> entries;
    // Absent when this is the last page. A POSITION, never an authorization:
    // every query behind it is filtered by the reader's own id and by their own
    // subscriptions regardless of what a client sends here.
    std::optional<Uuid>     next_cursor;
};

class InboxService final {
public:
    InboxService(const NotificationRepository& repository,
                 std::span<const TemplateSpec> templates, InboxHooks hooks) noexcept
        : repository_{repository},
          templates_{templates},
          hooks_{std::move(hooks)} {}

    // One merged page, newest first, rendered in `locale`.
    //
    // `limit` is the number of rows read from EACH half, so the merged page is
    // at most `limit` entries and may be fewer once suppression has run. It is
    // clamped to [kMinInboxPageSize, kMaxInboxPageSize].
    [[nodiscard]] Result<InboxPageView> page(mongocxx::client& client, const Uuid& reader,
                                             Locale locale, const std::optional<Uuid>& cursor,
                                             std::int32_t limit, db::TimeMs now) const;

    // The badge. CAPPED at kUnreadCountLimit rather than exact, because a badge
    // shows "99+" anyway and an exact count grows with a reader who has ignored
    // notifications for a year. A returned value above kUnreadCap means "more
    // than kUnreadCap", not that number.
    //
    // It does not re-examine per-row channel narrowing on the broadcast half —
    // a broadcast publish that excluded the in-app channel is counted here and
    // then not shown on the page. Both are bounded by the cap, and an exact badge
    // is not what the cap makes available in the first place.
    [[nodiscard]] Result<std::int64_t> unread(mongocxx::client& client, const Uuid& reader,
                                              db::TimeMs now) const;

    // Mark everything at or below `up_to` read, in BOTH halves.
    //
    // `up_to` is a watermark the client SENT — the highest id it has actually
    // rendered — and never a server-side `now`. A notification that arrived
    // between the client's render and this request must stay unread rather than
    // be silently buried by a marker the reader never saw.
    [[nodiscard]] Status mark_read(mongocxx::client& client, const Uuid& reader,
                                   const Uuid& up_to, db::TimeMs at) const;

    // Mark specific entries read. Targeted only: a broadcast entry has no
    // per-reader row to mark, so its read state is the watermark and nothing
    // else. Ids that name no row of this reader's match nothing, which is what
    // makes this safe to call with a client-supplied list.
    [[nodiscard]] Result<std::int64_t> mark_read(mongocxx::client& client, const Uuid& reader,
                                                 std::span<const Uuid> ids,
                                                 db::TimeMs at) const;

private:
    // The subscriptions that are actually in play for the in-app channel: the
    // reader's, minus the topics they have muted. Filtering the SUBSCRIPTION list
    // rather than the rows means the same filter reaches the page and the count
    // through one path, and neither can drift from the other.
    [[nodiscard]] static SubscriptionSet effective_subscriptions(
        std::span<const TopicSpec> topics, const SubscriptionSet& subs,
        const Preferences& prefs);

    [[nodiscard]] Result<InboxEntry> render_entry(const NotificationRow& row,
                                                  const TopicSpec& spec, Locale locale,
                                                  const Uuid& cursor_id, bool read) const;

    const NotificationRepository& repository_;
    std::span<const TemplateSpec> templates_;
    InboxHooks                    hooks_;
};

}  // namespace anvil::notifications
