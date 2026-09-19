// Layout invariants. These fail the build rather than the test run when broken
// at compile time; the runtime cases cover behaviour the compiler cannot check.

#include <gtest/gtest.h>

#include "perms.h"

#include "anvil/core/perm_catalogue.h"
#include "anvil/core/types.h"
#include "anvil/core/user_context.h"

namespace anvil {

// The application supplies the enum; anvil supplies everything it is used with.
using testapp::Perm;

TEST(CoreTypes, UserContextFitsOneCacheLine) {
    EXPECT_EQ(sizeof(UserContext), 64U);
    EXPECT_EQ(alignof(UserContext), 8U);
    EXPECT_TRUE(std::is_trivially_copyable_v<UserContext>);
}

TEST(CoreTypes, PermSetIsSixteenBytesOnTheStack) {
    EXPECT_EQ(sizeof(PermSet), 16U);
    EXPECT_EQ(alignof(PermSet), 8U);
}

TEST(PermSet, SetTestAndReset) {
    PermSet set;
    EXPECT_TRUE(set.none());

    set.set(0).set(63).set(64).set(127);
    EXPECT_EQ(set.count(), 4U);
    EXPECT_TRUE(set.test(0));
    EXPECT_TRUE(set.test(63));   // last bit of the low word
    EXPECT_TRUE(set.test(64));   // first bit of the high word
    EXPECT_TRUE(set.test(127));
    EXPECT_FALSE(set.test(1));

    set.reset(64);
    EXPECT_FALSE(set.test(64));
    EXPECT_EQ(set.count(), 3U);
}

TEST(PermSet, OutOfRangeBitsAreIgnoredNotUndefined) {
    // Every real caller passes a Perm enumerator; ignoring keeps the whole type
    // noexcept and usable in constant expressions.
    PermSet set;
    set.set(128);
    set.set(1000);
    EXPECT_TRUE(set.none());
    EXPECT_FALSE(set.test(128));
}

TEST(PermSet, WireFormatIsExplicitAndStable) {
    // Permission bits are persisted. The byte order is fixed by the type, not
    // by the standard library or the platform.
    PermSet set;
    set.set(0);    // low word, bit 0  -> byte 0, value 0x01
    set.set(9);    // low word, bit 9  -> byte 1, value 0x02
    set.set(64);   // high word, bit 0 -> byte 8, value 0x01

    const std::array<std::uint8_t, 16> bytes = set.to_bytes();
    EXPECT_EQ(bytes[0], 0x01U);
    EXPECT_EQ(bytes[1], 0x02U);
    EXPECT_EQ(bytes[8], 0x01U);

    EXPECT_EQ(PermSet::from_bytes(bytes), set);
}

TEST(PermSet, RoundTripsAllOneHundredAndTwentyEightBits) {
    for (std::size_t bit = 0; bit < PermSet::kBits; ++bit) {
        PermSet set;
        set.set(bit);
        const PermSet decoded = PermSet::from_bytes(set.to_bytes());
        EXPECT_EQ(decoded, set) << "bit=" << bit;
        EXPECT_TRUE(decoded.test(bit)) << "bit=" << bit;
        EXPECT_EQ(decoded.count(), 1U) << "bit=" << bit;
    }
}

TEST(PermSet, UnionAndIntersection) {
    const PermSet a = perm_mask(Perm::ContentWrite, Perm::MediaUpload);
    const PermSet b = perm_mask(Perm::MediaUpload, Perm::FormRead);

    EXPECT_EQ((a | b).count(), 3U);
    EXPECT_EQ((a & b).count(), 1U);
    EXPECT_TRUE((a & b).test(static_cast<std::size_t>(Perm::MediaUpload)));
}

TEST(PermSet, EverythingIsUsableInAConstantExpression) {
    // This is the property std::bitset could not provide under C++20, and the
    // reason route masks cost nothing at runtime.
    constexpr PermSet kMask = perm_mask(Perm::ContentWrite, Perm::ContentDelete);
    static_assert(kMask.count() == 2);
    static_assert(kMask.test(static_cast<std::size_t>(Perm::ContentWrite)));
    static_assert(!kMask.test(static_cast<std::size_t>(Perm::FormRead)));
    static_assert(kMask.to_bytes()[0] != 0);
    static_assert(PermSet::from_bytes(kMask.to_bytes()) == kMask);
    SUCCEED();
}

TEST(CoreTypes, UuidIsSixteenBytes) {
    // A 36-character string id would be 2.25x the storage and turn every
    // comparison into a string compare.
    EXPECT_EQ(sizeof(Uuid), 16U);
    EXPECT_TRUE(is_nil(kNilUuid));
}

TEST(CoreTypes, PermMaskIsConstexpr) {
    constexpr PermSet kEdit = perm_mask(Perm::ContentWrite);
    constexpr PermSet kEditAndPublish =
        perm_mask(Perm::ContentWrite, Perm::ContentDelete);

    static_assert(kEdit.count() == 1);
    static_assert(kEditAndPublish.count() == 2);

    EXPECT_TRUE(has_all(kEditAndPublish, kEdit));   // superset satisfies
    EXPECT_FALSE(has_all(kEdit, kEditAndPublish));  // subset does not
}

TEST(CoreTypes, EmptyRequirementIsAlwaysSatisfied) {
    EXPECT_TRUE(has_all(PermSet{}, PermSet{}));
}

// Everything below this point in the original was about a specific application's
// permission TABLE: that its bit indices had particular values, that its name
// table was complete and unique, and that later additions had not renumbered
// earlier blocks. None of it is anvil's to assert.
//
// The equivalents now live in two better places. The structural properties —
// complete, unique, in-range, and visited in bit order — are PermCatalogue's and
// are covered in seams_test.cc against the catalogue API. The value properties —
// that ContentWrite is bit 1 and always will be — belong to whoever declares the
// permissions, and are static_asserted in tests/testapp/perms.h, where adding a
// permission without naming it fails the build rather than shipping a bit the
// dashboard receives and cannot render.
//
// LocalizedView moved for the same reason: it is dimensioned by the
// application's locale table now, so its tests live beside that seam.

}  // namespace anvil
