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

inline constexpr std::array<DimensionSpec, 1> kEntityDimension{
    {{"which", kNone, DimensionKind::Entity}}};
// An Entity dimension declaring a closed set anyway — the value space and the
// kind now disagree, which is exactly the state well_formed() exists to
// refuse rather than pick a side of silently.
inline constexpr std::array<DimensionSpec, 1> kEntityWithValues{
    {{"which", kTwo, DimensionKind::Entity}}};
inline constexpr std::array<DimensionSpec, 2> kTwoEntityDimensions{
    {{"first", kNone, DimensionKind::Entity}, {"second", kNone, DimensionKind::Entity}}};

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

// An Entity dimension is accepted with an EMPTY value space — that is the
// point of the kind — and refused the moment it declares one anyway.
inline constexpr std::array<EventSpec, 1> kEntityOk{
    {{"Thing", kEntityDimension, 0, EventClass::Behaviour, true}}};
static_assert(event_table_is_well_formed(kEntityOk));

inline constexpr std::array<EventSpec, 1> kEntityDeclaringValues{
    {{"Thing", kEntityWithValues, 0, EventClass::Behaviour, true}}};
static_assert(!event_table_is_well_formed(kEntityDeclaringValues));

// Event carries exactly one entity slot (anvil/analytics/event.h), so a second
// Entity-kind dimension on one event has nowhere of its own to be stored.
inline constexpr std::array<EventSpec, 1> kTwoEntities{
    {{"Thing", kTwoEntityDimensions, 0, EventClass::Behaviour, true}}};
static_assert(!event_table_is_well_formed(kTwoEntities));

TEST(EventTable, EntityDimensionOfFindsTheOneEntitySlotAndNothingElse) {
    const EventSpec entity_event{"Thing", kEntityDimension, 0, EventClass::Behaviour, true};
    const DimensionSpec* found = entity_dimension_of(entity_event);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "which");
    EXPECT_EQ(found->kind, DimensionKind::Entity);

    const EventSpec enum_event{"Thing", kOneDimension, 0, EventClass::Behaviour, true};
    EXPECT_EQ(entity_dimension_of(enum_event), nullptr);
}

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
