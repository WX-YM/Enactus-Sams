#pragma once

// One recorded moment, in memory and on the wire.
//
// An event is a ROW, and a row per request is N x request_rate inserts against
// the cluster the request path is already using. Everything about this type is
// sized by that: it is 60 bytes, trivially copyable, and carries INDICES rather
// than strings — with one exception, the entity dimension's id, which is an
// application-minted UUID rather than a request byte and is bounded a
// different way (docs/17-analytics.md §19) — so a buffer of eight thousand of
// them is under half a megabyte rather than an unbounded pile of allocations
// (docs/17-analytics.md §2).
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

// Ordered largest-alignment-first (CLAUDE.md §2.3). Only `code` needs more than
// byte alignment, so it goes first and the rest pack behind it. `entity` is
// LAST rather than beside `dimensions`, purely so a positional aggregate-init
// written against the four-member struct keeps compiling: the fifth value
// value-initialises to kNilUuid exactly as an omitted one would.
struct Event final {
    EventCode           code;        //  4
    DimensionValues     dimensions;  //  4
    VisitorId           session;     // 16
    // The account this row is ABOUT, when there is one. Present only so erasure
    // can be a point query on a partial index anonymous rows never enter
    // (docs/17-analytics.md §12); nothing reads it back.
    std::optional<Uuid> subject;     // 17
    // The Entity-kind dimension's value, for an event whose spec declares one
    // (event_spec.h). kNilUuid means "not supplied for this occurrence" — the
    // same absent-slot idea kNoDimensionValue gives an enum dimension's byte,
    // chosen because a real id is never nil in practice (core/types.h), so the
    // sentinel costs no state an id could actually collide with. An event whose
    // spec declares no Entity dimension always carries kNilUuid here.
    Uuid                entity = kNilUuid;  // 16
};

static_assert(sizeof(Event) == 60, "Event must not grow padding");
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

static_assert(sizeof(EventRow) == 72, "EventRow must not grow padding");

// What makes two consecutive behaviour rows the same event repeated.
//
// The SESSION and not the address, for the reason above; the dimension indices
// and not their names, because the indices are what the row carries. `entity`
// is part of the key for the same reason `dimensions` is: two rows that differ
// only in which project they are about are not the same occurrence repeated,
// and folding them would silently merge one project's count into another's.
struct EventFoldKey final {
    VisitorId       session;
    DimensionValues dimensions;
    Uuid            entity;
    EventCode       code;

    [[nodiscard]] bool operator==(const EventFoldKey& other) const noexcept = default;
};

[[nodiscard]] constexpr EventFoldKey fold_key_of(const Event& event) noexcept {
    return EventFoldKey{event.session, event.dimensions, event.entity, event.code};
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
// OMITTED, not written as a nil placeholder, for every row that carries no
// entity value — which is most of them, including every row written before
// this field existed. A reader that finds it absent decodes kNilUuid, so an
// old row reads identically to a new one that simply has no entity dimension.
inline constexpr std::string_view kEntity = "ent";
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
// Same OMITTED-when-absent shape as event_fields::kEntity, and for the same
// backward-compatibility reason: a rollup document written before this field
// existed has no "ent" key, and decodes identically to one whose row simply
// carries no entity.
inline constexpr std::string_view kEntity = "ent";
inline constexpr std::string_view kCount = "count";
inline constexpr std::string_view kSessions = "sessions";
inline constexpr std::string_view kComputedAt = "computed_at";

}  // namespace rollup_fields

}  // namespace anvil::analytics
