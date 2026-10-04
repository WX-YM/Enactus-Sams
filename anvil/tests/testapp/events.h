#pragma once

// The reference application's analytics events.
//
// anvil ships the buffer, the shedding policy, sessionisation, the rollup
// arithmetic and the collections' shape; WHAT is worth recording is a list of
// this product's own moments (docs/01-seams.md §12). Compiled by every build of
// the test suite, so the worked example in the seam doc is a file that must keep
// compiling rather than a snippet that can rot.
//
// STORED as int32 and read back by rollups computed months ago. APPEND ONLY:
// never renumber, never reuse a retired value. The same rule the locale index,
// the permission bit, the namespace index, the field-type code, the audit action
// and the notification template id live under, and it fails the same way —
// silently, by reinterpreting rows that are already written.

#include <array>
#include <cstdint>
#include <string_view>

#include "anvil/analytics/event_spec.h"

namespace testapp {

namespace a = anvil::analytics;

// A dimension is a CLOSED set, for the same reason a metric label is: an event
// carrying a free-text dimension is a collection whose index cardinality is
// chosen by a visitor.
inline constexpr std::array<std::string_view, 3> kSurfaceValues{"web", "ios", "android"};
inline constexpr std::array<std::string_view, 2> kReferrerValues{"direct", "search"};

inline constexpr std::array<a::DimensionSpec, 1> kSurface{{{"surface", kSurfaceValues}}};
inline constexpr std::array<a::DimensionSpec, 2> kSurfaceAndReferrer{
    {{"surface", kSurfaceValues}, {"referrer", kReferrerValues}}};

// An ENTITY dimension: no closed set, because "which project" is an id the
// application mints elsewhere (anvil/entries) rather than a value this table
// could enumerate — a project added at runtime must not need a deploy to be
// counted separately (docs/17-analytics.md §19). `values` is empty; the empty
// array itself is the well-formed check, not a placeholder for one to fill in.
inline constexpr std::array<std::string_view, 0> kNoValues{};
inline constexpr std::array<a::DimensionSpec, 1> kProject{
    {{"project", kNoValues, a::DimensionKind::Entity}}};

enum class Event : a::EventCode {
    PageViewed = 0,
    SignupStarted = 1,
    SignupCompleted = 2,
    ProjectViewed = 3,
};

// The CLASSIFICATION is the load-bearing column, not a label. SignupCompleted is
// the only Conversion here, and that is what makes a refresh storm compressible
// and the signup undroppable — get it backwards and a flood of page views evicts
// the one row the whole funnel is about (anvil/analytics/buffer.h).
//
// `requires_consent` is refused at the door, not filtered later. The completed
// signup does not require it because it is recorded against an account that has
// just been created deliberately; the two behaviour rows are observation of a
// visitor who has agreed to nothing.
// ProjectViewed's own consent story: it is a page view like PageViewed, so it
// requires consent for the same reason — it is observation of a visitor who
// has agreed to nothing.
inline constexpr std::array<a::EventSpec, 4> kEvents{{
    {"PageViewed", kSurfaceAndReferrer, 0, a::EventClass::Behaviour, true},
    {"SignupStarted", kSurface, 1, a::EventClass::Behaviour, true},
    {"SignupCompleted", kSurface, 2, a::EventClass::Conversion, false},
    {"ProjectViewed", kProject, 3, a::EventClass::Behaviour, true},
}};

static_assert(a::event_table_is_well_formed(kEvents),
              "an empty or duplicate name; a duplicate or negative code; more than four "
              "dimensions; an empty, oversized or duplicated dimension value space; a "
              "non-empty value space on an entity dimension; more than one entity dimension "
              "on one event");

static_assert(a::events_are_dense_from_zero(kEvents),
              "the lookup is a direct index; a sparse table turns it into a scan per event, "
              "and an event is offered on a request path");

static_assert(kEvents.size() == 4,
              "adding an event is a deliberate act: the code is stored on disk and can never "
              "be renumbered or reused");

// The dimension value spaces, as enums. What a row stores is the INDEX, so these
// enumerators are what a call site names and the strings are what a rollup is
// read back with.
enum class Surface : std::uint8_t { Web = 0, Ios = 1, Android = 2 };
enum class Referrer : std::uint8_t { Direct = 0, Search = 1 };

}  // namespace testapp
