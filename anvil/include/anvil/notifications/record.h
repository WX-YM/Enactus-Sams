#pragma once

// What the three notification collections store.
//
//   notifications        the canonical event: ONE row per publish, whatever the
//                        audience size. Template id and parameters, never
//                        rendered text.
//   notification_inbox   targeted delivery plus read state: one row per
//                        recipient, and only for fan-out-on-write topics.
//   notification_clients every delivery endpoint — an in-app inbox, a browser
//                        push subscription, a mailbox, an external webhook —
//                        because they share lifecycle, preferences and delivery
//                        bookkeeping.
//
// --- both TTLs express a LIFETIME, not a retention policy -------------------
//
// `notifications` and `notification_inbox` carry `expires_at` TTL indexes, and
// the monitor runs roughly every 60 seconds — so an expired notification stays
// READABLE for up to a minute after it should have gone. Every read filters
// `expires_at` explicitly as well, which is what anvil/db/collections.h's
// lifetime_expiry_field and tools/check-db-discipline.sh exist to enforce.
//
// --- every parameter key is a single ASCII letter ---------------------------
//
// `params` is a subdocument whose keys come from the template's placeholders, so
// they reach a document's key space. The single-letter grammar in
// template_spec.h is what makes `$set` and `a.b` unrepresentable there rather
// than merely rejected — the same position anvil/forms/fid.h takes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/notifications/template_spec.h"
#include "anvil/notifications/topic_spec.h"

namespace anvil::notifications {

// What a notification points at, for the read-time access recheck.
//
// The KIND is the application's numbering — an article, an order, a ticket —
// so anvil carries it as a stored int32 and never interprets it. What anvil does
// with it is hand it back to the application's visibility hook; what it must
// never do is guess what kind 3 means.
using ResourceKind = std::int32_t;

// Zero is "no resource". A notification with no ref — a system announcement —
// skips the visibility check entirely, so the default must mean exactly that.
inline constexpr ResourceKind kNoResource = 0;

struct ResourceRef final {
    Uuid         id;
    ResourceKind kind;
};

static_assert(std::is_trivially_copyable_v<ResourceRef>);

// A parameter as STORED. Owns its text, because a row read out of BSON crosses a
// pool boundary before it is rendered and a view into the driver's buffer would
// dangle (ENGINEERING_RULES.md §2.2).
struct StoredParam final {
    std::string  text;     // Text only
    std::int64_t number;   // Number only
    char         name;
    ParamType    type;
};

// Fixed capacity, no vector: the cap is a compile-time property of the subsystem,
// so the storage is too.
struct ParamSet final {
    std::array<StoredParam, kMaxParams> items;
    std::uint8_t                        count;

    [[nodiscard]] std::span<const StoredParam> view() const noexcept {
        return std::span<const StoredParam>{items.data(), count};
    }
    [[nodiscard]] bool push(StoredParam param) {
        if (count >= items.size()) { return false; }
        items[count] = std::move(param);
        ++count;
        return true;
    }
};

// The canonical event: one row per publish, whatever the audience size.
struct NotificationRow final {
    Uuid                        id;            // UUIDv7 — cursor, sort key, index locality
    ParamSet                    params;
    std::optional<Uuid>         subject;
    std::optional<ResourceRef>  ref;
    std::optional<Uuid>         actor;
    // The outbox marker. Set AFTER the transport jobs are enqueued, never before:
    // a sweeper re-enqueuing a delivery is harmless because every transport is
    // idempotent, while marking first loses the delivery entirely.
    std::optional<db::TimeMs>   dispatched_at;
    db::TimeMs                  created_at;
    db::TimeMs                  expires_at;
    std::array<std::uint8_t, 16> dedupe;
    std::int32_t                count;         // coalesced repeats
    TopicCode                   kind;
    TemplateId                  tpl;
    // The channels this publish asked for, PERSISTED rather than recomputed.
    //
    // A publish that deliberately narrowed its channels — a security alert that
    // must not be mailed to an address the attacker may already control — is
    // re-dispatched by the outbox sweeper after a crash, and a sweeper that fell
    // back to the topic's defaults would WIDEN it on the replay. The one place
    // that decision is recorded has to be the row, because the sweeper is a
    // different process from the publish and has nothing else to read.
    //
    // kDefaultChannels means "whatever the topic declares" and is the normal
    // value; it is not the same as kNoChannels, which is a publish that asked for
    // no transports at all.
    ChannelMask                 channels;
};

// A targeted delivery plus its read state.
struct InboxRow final {
    Uuid                      id;      // UUIDv7 — the inbox cursor
    Uuid                      nid;
    std::optional<db::TimeMs> read_at;
    // Denormalised onto the row so a preference filter needs no second read.
    TopicCode                 kind;
};

enum class DigestMode : std::uint8_t { None = 0, Hourly = 1, Daily = 2 };
inline constexpr DigestMode kMaxDigestMode = DigestMode::Daily;

// How a delivery attempt ended. STORED on the client row beside the instant.
enum class DeliveryVerdict : std::uint8_t {
    Delivered = 0,
    // The endpoint is gone for good: a 404 or 410 from a push service, a webhook
    // answering 410. Disable the client immediately rather than after a streak.
    Gone = 1,
    // Worth another attempt: a timeout, a 5xx, a network error.
    Transient = 2,
    // Malformed configuration — an unusable address, a missing key, an SSRF
    // refusal. Retrying cannot fix any of them, so it is not retried.
    Rejected = 3,
};

inline constexpr DeliveryVerdict kMaxDeliveryVerdict = DeliveryVerdict::Rejected;

// What a soft failure streak costs. At the threshold the endpoint is disabled: a
// permanently broken webhook that retries forever is a self-inflicted outbound
// flood, and a dead push endpoint is one the service already told us about.
inline constexpr std::int32_t kMaxSoftFailures = 5;

// A (client, topic) binding.
//
// `since` is what stops a new subscriber seeing years of history on first read: a
// subscription grants visibility FORWARD, not retroactively.
//
// It must be a `uuid::v7_boundary` — the floor of the subscribing millisecond —
// and never a generated UUIDv7. A v7 id carries 74 random bits after its 48-bit
// timestamp, so two ids from the same millisecond order by those random bits
// rather than by time: a generated watermark makes `_id > since` a coin flip for
// anything published in the millisecond somebody subscribed, and loses the
// notification outright whenever it comes up the wrong way. The boundary ties
// deterministically, and it ties towards DELIVERING — a notification published in
// the millisecond a reader subscribed is one they asked for, and dropping it is
// the worse of the two errors.
//
// The failure mode is worth naming because of how it presents: at millisecond
// resolution the race is narrow, so in production it looks like an occasional
// unreproducible "I never got that notification", and in CI it looks like an
// unrelated test being flaky.
struct Subscription final {
    Uuid      subject;   // nil for an unscoped topic
    Uuid      since;
    TopicCode kind;
};

// Past roughly this many, the topic model is being misused as a per-resource
// follow list and a separate follow collection is the answer. Refusing at the
// limit keeps the read merge two queries rather than one per topic.
inline constexpr std::size_t kMaxSubscriptions = 64;

struct SubscriptionSet final {
    std::array<Subscription, kMaxSubscriptions> items;
    std::uint8_t                                count;

    [[nodiscard]] std::span<const Subscription> view() const noexcept {
        return std::span<const Subscription>{items.data(), count};
    }
    [[nodiscard]] bool push(const Subscription& sub) noexcept {
        if (count >= items.size()) { return false; }
        items[count] = sub;
        ++count;
        return true;
    }
    [[nodiscard]] const Subscription* find(TopicCode kind, const Uuid& subject) const noexcept {
        for (std::uint8_t i = 0; i < count; ++i) {
            if (items[i].kind == kind && items[i].subject == subject) { return &items[i]; }
        }
        return nullptr;
    }
    [[nodiscard]] bool remove(TopicCode kind, const Uuid& subject) noexcept {
        for (std::uint8_t i = 0; i < count; ++i) {
            if (items[i].kind != kind || items[i].subject != subject) { continue; }
            items[i] = items[count - 1];
            items[count - 1] = Subscription{};
            --count;
            return true;
        }
        return false;
    }
};

// A delivery endpoint.
struct ClientRow final {
    // UUIDv4: a client id is a handle somebody deletes by, so it is unguessable
    // rather than time-ordered.
    Uuid                      id;
    std::string               addr;   // push endpoint / mailbox / webhook URL
    // Web-push keys, or the webhook HMAC secret — SEALED, never plaintext. The
    // secret is shown once at creation and there is no read path that decrypts it
    // back.
    std::vector<std::uint8_t> keys;
    SubscriptionSet           subs;
    std::optional<Uuid>       owner;  // absent for a system-level webhook client
    // The broadcast read watermark: every broadcast notification at or below it
    // is read. A watermark rather than a row per reader per broadcast, because
    // per-row read state would reintroduce exactly the fan-out-on-write cost the
    // read strategy exists to avoid.
    std::optional<Uuid>       broadcast_watermark;
    std::optional<db::TimeMs> disabled_at;
    // The last delivery ATTEMPT and how it ended. Absent until one has been made,
    // which is why the INSTANT is the optional and the verdict is not: "never
    // tried" and "tried and it worked" are different rows on an operator's
    // screen, and a default-constructed verdict would render them the same.
    std::optional<db::TimeMs> last_delivery_at;
    db::TimeMs                created_at;
    std::int64_t              version;
    std::int32_t              fail_n;
    Preferences               prefs;
    ClientType                type;
    DigestMode                digest;
    DeliveryVerdict           last_verdict;
};

// The projection the fan-out scan reads.
//
// A full ClientRow carries a 64-entry subscription array; loading hundreds of
// them to deliver one notification would be two kilobytes each of data the
// delivery decision never looks at (ENGINEERING_RULES.md §7).
struct ClientTarget final {
    Uuid                      id;
    std::string               addr;
    // SEALED, exactly as stored — the web-push keys or the webhook signing
    // secret. Carried on the projection because a transport needs them and the
    // alternative is a second read per endpoint on a path that already runs once
    // per subscriber; it is at most a few dozen bytes, unlike the 64-entry
    // subscription array this projection exists to leave behind.
    std::vector<std::uint8_t> keys;
    std::optional<Uuid>       owner;
    Preferences               prefs;
    ClientType                type;
    DigestMode                digest;
};

// One row of an operator's webhook screen.
//
// A projection and NOT a ClientRow, for one reason that matters: a ClientRow
// carries `keys`, which is the sealed signing secret. The secret is shown once at
// registration and there is no read path that decrypts it back — so the type the
// listing hands around does not have a field to put it in.
struct WebhookRow final {
    std::string               url;
    SubscriptionSet           subs;
    std::optional<db::TimeMs> last_delivery_at;
    Uuid                      id;
    db::TimeMs                created_at;
    std::int64_t              version;
    std::int32_t              fail_n;
    bool                      enabled;
    DeliveryVerdict           last_verdict;
};

// Read-state bookkeeping for broadcast topics.
struct ReadState final {
    std::optional<Uuid> broadcast_watermark;
};

// Cursor pages. `next_cursor` is absent when the page is the last one; it is a
// POSITION and never an authorization — every query is filtered by the reader's
// own id and by their subscriptions regardless.
struct InboxPage final {
    std::vector<InboxRow> rows;
    std::optional<Uuid>   next_cursor;
};

struct BroadcastPage final {
    std::vector<NotificationRow> rows;
    std::optional<Uuid>          next_cursor;
};

// The unread count is CAPPED, not exact: a badge shows "99+" anyway, and an
// uncapped count grows with a reader who ignores notifications for a year.
inline constexpr std::int64_t kUnreadCap = 99;
// Counted with a limit one past the cap, so "is it more than 99" is answerable
// without scanning the rest.
inline constexpr std::int64_t kUnreadCountLimit = kUnreadCap + 1;

inline constexpr std::int32_t kMinInboxPageSize = 1;
inline constexpr std::int32_t kMaxInboxPageSize = 50;

// What a fan-out writes in one round trip. Bounded so a topic with 5 000
// subscribers becomes forty writes rather than one 5 000-document insert that
// occupies a db_pool connection for its whole duration.
inline constexpr std::size_t kFanoutBatch = 128;

// The three collections, named by the application. anvil never derives either a
// collection or a database name from request data.
struct NotificationCollections final {
    std::string_view notifications;
    std::string_view inbox;
    std::string_view clients;
};

}  // namespace anvil::notifications
