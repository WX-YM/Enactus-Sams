#pragma once

// One recorded moment, in memory and on the wire.
//
// An event is a ROW, and a row per request is N x request_rate inserts against
// the cluster the request path is already using. Everything about this type is
// sized by that: it is 44 bytes, trivially copyable, and carries INDICES rather
// than strings, so a buffer of eight thousand of them is a third of a megabyte
// rather than an unbounded pile of allocations (docs/17-analytics.md §2).
//
// The address is not here, and that is the point. What identifies a visitor is a
// peppered, day-rotating digest computed in anvil/analytics/sessions.h; the
// packed address never reaches a row, because an IPv4 address is a 32-bit input
// space and an unkeyed digest of one is reversible by anybody holding a database
// dump in the time it takes to enumerate it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "anvil/analytics/event_spec.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"

namespace anvil::analytics {

// The 16-byte visitor digest. NOT an address, not a truncation of one, and not
// stable across days — see anvil/analytics/sessions.h for all three reasons.
using VisitorId = std::array<std::uint8_t, 16>;

// A dimension's value indices, one slot per declared dimension, kNoDimensionValue
// for a slot the event's spec does not declare.
using DimensionValues = std::array<std::uint8_t, kMaxDimensions>;

[[nodiscard]] constexpr DimensionValues no_dimensions() noexcept {
    return DimensionValues{kNoDimensionValue, kNoDimensionValue, kNoDimensionValue,
                           kNoDimensionValue};
}

// Ordered largest-alignment-first (ENGINEERING_RULES.md §2.3). Only `code` needs more than
// byte alignment, so it goes first and the rest pack behind it.
struct Event final {
    EventCode           code;        //  4
    DimensionValues     dimensions;  //  4
    VisitorId           session;     // 16
    // The account this row is ABOUT, when there is one. Present only so erasure
    // can be a point query on a partial index anonymous rows never enter
    // (docs/17-analytics.md §12); nothing reads it back.
    std::optional<Uuid> subject;     // 17
};

static_assert(sizeof(Event) == 44, "Event must not grow padding");
static_assert(std::is_trivially_copyable_v<Event>);

// A buffered row: the event, the instant it HAPPENED, and how many identical
// ones folded into it.
struct EventRow final {
    db::TimeMs    at;       //  8
    Event         event;    // 44
    // SATURATING on write, because an unsigned wrap would report a refresh storm
    // as a handful of visits — the one reading this collection exists to produce.
    std::uint32_t repeats;  //  4
};

static_assert(sizeof(EventRow) == 56, "EventRow must not grow padding");

// What makes two consecutive behaviour rows the same event repeated.
//
// The SESSION and not the address, for the reason above; the dimension indices
// and not their names, because the indices are what the row carries.
struct EventFoldKey final {
    VisitorId       session;
    DimensionValues dimensions;
    EventCode       code;

    [[nodiscard]] bool operator==(const EventFoldKey& other) const noexcept = default;
};

[[nodiscard]] constexpr EventFoldKey fold_key_of(const Event& event) noexcept {
    return EventFoldKey{event.session, event.dimensions, event.code};
}

// The field names the rows carry. Published rather than spelled at each call
// site so an index over a column anvil does not write is a compile error rather
// than a COLLSCAN nobody notices (tests/testapp/indexes.h).
namespace event_fields {

inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kCode = "code";
inline constexpr std::string_view kAt = "at";
inline constexpr std::string_view kSession = "sess";
inline constexpr std::string_view kSubject = "subj";
inline constexpr std::string_view kDimensions = "dims";
inline constexpr std::string_view kRepeats = "reps";
inline constexpr std::string_view kExpiresAt = "expires_at";

}  // namespace event_fields

namespace session_fields {

inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kVisitor = "vis";
inline constexpr std::string_view kDay = "day";
inline constexpr std::string_view kStartedAt = "started_at";
inline constexpr std::string_view kExpiresAt = "expires_at";

}  // namespace session_fields

namespace rollup_fields {

inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kCode = "code";
inline constexpr std::string_view kBucket = "bucket";
inline constexpr std::string_view kGranularity = "gran";
inline constexpr std::string_view kDimensions = "dims";
inline constexpr std::string_view kCount = "count";
inline constexpr std::string_view kSessions = "sessions";
inline constexpr std::string_view kComputedAt = "computed_at";

}  // namespace rollup_fields

}  // namespace anvil::analytics
