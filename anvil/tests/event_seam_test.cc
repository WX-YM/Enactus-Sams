// Phase 8 — the event seam, asserted from outside anvil.
//
// Half of this seam is a BUILD rather than a test: event_table_is_well_formed
// and events_are_dense_from_zero are static_asserted over tests/testapp/events.h,
// so a sparse table or a duplicated code never links. What is left for a test is
// the set of malformed tables the check must REFUSE, and the two directions an
// unknown code has to fail in.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "anvil/analytics/event_spec.h"

#include "events.h"

namespace anvil::analytics {
namespace {

inline constexpr std::array<std::string_view, 2> kTwo{"a", "b"};
inline constexpr std::array<DimensionSpec, 1> kOneDimension{{{"which", kTwo}}};
inline constexpr std::array<std::string_view, 0> kNone{};
inline constexpr std::array<DimensionSpec, 1> kEmptyDimension{{{"which", kNone}}};
inline constexpr std::array<DimensionSpec, 2> kDuplicateDimension{{{"which", kTwo},
                                                                  {"which", kTwo}}};

inline constexpr std::array<EventSpec, 1> kEmptyName{
    {{"", kOneDimension, 0, EventClass::Behaviour, true}}};
static_assert(!event_table_is_well_formed(kEmptyName));

inline constexpr std::array<EventSpec, 1> kNegativeCode{
    {{"Thing", kOneDimension, -1, EventClass::Behaviour, true}}};
static_assert(!event_table_is_well_formed(kNegativeCode));

inline constexpr std::array<EventSpec, 2> kDuplicateCode{
    {{"One", kOneDimension, 0, EventClass::Behaviour, true},
     {"Two", kOneDimension, 0, EventClass::Conversion, false}}};
static_assert(!event_table_is_well_formed(kDuplicateCode));

// A dimension with no values contributes nothing and makes every offer against
// it silently drop the dimension.
inline constexpr std::array<EventSpec, 1> kEmptyValues{
    {{"Thing", kEmptyDimension, 0, EventClass::Behaviour, true}}};
static_assert(!event_table_is_well_formed(kEmptyValues));

inline constexpr std::array<EventSpec, 1> kDuplicateName{
    {{"Thing", kDuplicateDimension, 0, EventClass::Behaviour, true}}};
static_assert(!event_table_is_well_formed(kDuplicateName));

// Sparse codes turn the lookup from an index into a scan, on a path that runs
// once per request.
inline constexpr std::array<EventSpec, 2> kSparse{
    {{"One", kOneDimension, 0, EventClass::Behaviour, true},
     {"Two", kOneDimension, 7, EventClass::Conversion, false}}};
static_assert(event_table_is_well_formed(kSparse));
static_assert(!events_are_dense_from_zero(kSparse));

TEST(EventTable, TheReferenceApplicationsTableIsAccepted) {
    EXPECT_TRUE(event_table_is_well_formed(testapp::kEvents));
    EXPECT_TRUE(events_are_dense_from_zero(testapp::kEvents));
}

TEST(EventTable, AnUnknownCodeFailsTowardsRecordingLess) {
    // The two unknown-code defaults point in opposite directions deliberately: a
    // row this build cannot interpret must not be able to evict one it can, and
    // it must not be recorded without consent either.
    EXPECT_EQ(event_class_of(testapp::kEvents, 99), EventClass::Behaviour);
    EXPECT_TRUE(event_requires_consent(testapp::kEvents, 99));
}

}  // namespace
}  // namespace anvil::analytics
