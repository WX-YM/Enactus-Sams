// Fuzzing the edit recipe decoder, and what a refusal costs.
//
// decode_recipe is a parser of attacker bytes on a request path
// (docs/21-image-edits.md §2), and edit_recipe_test covers it with a prefix
// sweep and one vector per refusal. That proves each named rule; it does not
// prove the rules are the whole list. The property that does is the canonical
// one: anything the decoder ACCEPTS re-encodes to exactly the bytes it was
// given, and plans inside the frame it names. Noise and mutated vectors are
// thrown at that property here.
//
// It lives in anvil_alloc_tests rather than beside edit_recipe_test because the
// second claim is about allocation: a recipe DECLARING four thousand points is
// refused before a byte of storage is reserved for them, so the cost of lying
// about a count is a comparison. That is only checkable in the binary that
// counts operator new.
//
// Fixed seeds, as in validation_fuzz_test.cc: a failure is a reproduction, not
// an anecdote. std::mt19937 is banned for anything security-relevant and is
// exactly right for reproducible test input.

#include <gtest/gtest.h>

#include "alloc_counter.h"
#include "testapp/edit_vectors.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "anvil/crypto/base64url.h"
#include "anvil/images/recipe.h"

namespace {

using anvil::images::decode_recipe;
using anvil::images::encode_recipe;
using anvil::images::plan_edit;
using anvil::images::Recipe;
using anvil::testing::AllocationCounter;

constexpr std::uint32_t kSeed = 0x45444954;   // "EDIT"
constexpr int kFuzzIterations = 200000;

constexpr anvil::images::EditLimits kLimits = testapp::kEditVectorLimits;

// Masters a surviving recipe is planned against: square, both landscape and
// portrait, one at the dimension cap and one below the narrowest rung.
constexpr std::array<anvil::images::ImageInfo, 5> kMasters{{
    {.width = 1000, .height = 1000, .pages = 1},
    {.width = 4000, .height = 3000, .pages = 1},
    {.width = 600, .height = 2400, .pages = 1},
    {.width = 12000, .height = 12000, .pages = 1},
    {.width = 200, .height = 150, .pages = 1},
}};

// The properties every accepted recipe must have, whatever produced it.
void expect_sound(std::span<const std::uint8_t> input, const Recipe& recipe) {
    std::vector<std::uint8_t> again;
    encode_recipe(recipe, again);
    ASSERT_TRUE(std::equal(again.begin(), again.end(), input.begin(), input.end()))
        << "an accepted recipe did not re-encode to its own bytes";

    ASSERT_LE(recipe.strokes.size(), kLimits.max_strokes);
    std::uint32_t points = 0;
    for (const anvil::images::RecipeStroke& stroke : recipe.strokes) {
        ASSERT_EQ(stroke.first_point, points);
        ASSERT_GT(stroke.point_count, 0);
        ASSERT_GT(stroke.rgba[3], 0);
        ASSERT_GT(stroke.width, 0);
        ASSERT_LE(stroke.width, anvil::images::kMaxStrokeWidth);
        points += stroke.point_count;
    }
    ASSERT_LE(points, kLimits.max_points);
    ASSERT_EQ(recipe.points.size(), static_cast<std::size_t>(points) * 2);
    ASSERT_LT(recipe.turns, 4);

    for (const anvil::images::ImageInfo master : kMasters) {
        const auto plan = plan_edit(recipe, master, kLimits);
        if (!plan) { continue; }
        const anvil::images::EditPlan& p = plan.value();
        // The crop is inside the oriented frame, never empty, and the output
        // is never larger than the crop, never above the widest rung and
        // never below the narrowest.
        ASSERT_GT(p.crop.width, 0U);
        ASSERT_GT(p.crop.height, 0U);
        ASSERT_LE(p.crop.left + p.crop.width, p.oriented_width);
        ASSERT_LE(p.crop.top + p.crop.height, p.oriented_height);
        ASSERT_LE(p.out_width, p.crop.width);
        ASSERT_LE(p.out_height, p.crop.height);
        ASSERT_LE(std::max(p.out_width, p.out_height), kLimits.max_edge_px);
        ASSERT_GE(p.out_width, kLimits.min_edge_px);
    }
}

void decode_and_check(std::span<const std::uint8_t> input) {
    const auto decoded = decode_recipe(input, kLimits);
    if (decoded) { expect_sound(input, decoded.value()); }
}

// Uniform noise almost never passes the version byte. Half the inputs start
// with a valid version and only known flag bits, which is what reaches the
// crop, resize and stroke readers at all.
[[nodiscard]] std::vector<std::uint8_t> random_recipe(std::mt19937& rng) {
    std::uniform_int_distribution<std::size_t> length{0, 160};
    std::uniform_int_distribution<int> byte{0, 255};
    std::vector<std::uint8_t> out(length(rng));
    for (std::uint8_t& b : out) { b = static_cast<std::uint8_t>(byte(rng)); }
    if (out.size() >= 2 && (rng() & 1) != 0) {
        out[0] = anvil::images::kRecipeVersion;
        out[1] = static_cast<std::uint8_t>(out[1] & 0x1F);
        // Small stroke counts, so the reader gets past the count bound.
        const std::size_t count_at = std::size_t{2} + ((out[1] & 0x08) != 0 ? 8U : 0U) +
                                     ((out[1] & 0x10) != 0 ? 2U : 0U);
        if (count_at < out.size()) { out[count_at] = static_cast<std::uint8_t>(out[count_at] & 3); }
    }
    return out;
}

[[nodiscard]] std::vector<std::vector<std::uint8_t>> vector_seeds() {
    std::vector<std::vector<std::uint8_t>> seeds;
    for (const testapp::EditVector& vector : testapp::kEditVectors) {
        std::optional<std::vector<std::uint8_t>> bytes =
            anvil::crypto::base64url_decode(vector.recipe);
        if (bytes.has_value()) { seeds.push_back(std::move(*bytes)); }
    }
    return seeds;
}

// One structural mutation of a known recipe: the decoder's interesting edges
// are one byte away from a valid encoding, where noise never lands.
void mutate(std::mt19937& rng, std::vector<std::uint8_t>& bytes) {
    std::uniform_int_distribution<int> byte{0, 255};
    const std::size_t size = bytes.size();
    const std::size_t at = size == 0 ? 0 : rng() % size;
    switch (rng() % 6) {
        case 0:
            if (size != 0) { bytes[at] = static_cast<std::uint8_t>(bytes[at] ^ (1U << (rng() % 8))); }
            break;
        case 1:
            if (size != 0) { bytes[at] = static_cast<std::uint8_t>(byte(rng)); }
            break;
        case 2:
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                         static_cast<std::uint8_t>(byte(rng)));
            break;
        case 3:
            if (size != 0) { bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at)); }
            break;
        case 4:
            bytes.resize(at);
            break;
        default:
            // A u16 nudged by one: the off-by-one of every bound and every
            // "the whole frame" and "equal to the source" spelling.
            if (size >= 2) {
                const std::size_t hi = at == size - 1 ? at - 1 : at;
                auto value = static_cast<std::uint16_t>((bytes[hi] << 8) | bytes[hi + 1]);
                value = static_cast<std::uint16_t>((rng() & 1) != 0 ? value + 1 : value - 1);
                bytes[hi] = static_cast<std::uint8_t>(value >> 8);
                bytes[hi + 1] = static_cast<std::uint8_t>(value & 0xFF);
            }
            break;
    }
}

}  // namespace

// --- fuzz -----------------------------------------------------------------

TEST(RecipeFuzz, DecoderSurvivesArbitraryBytes) {
    std::mt19937 rng{kSeed};
    for (int i = 0; i < kFuzzIterations; ++i) {
        const std::vector<std::uint8_t> input = random_recipe(rng);
        decode_and_check(input);
        if (::testing::Test::HasFatalFailure()) { return; }
    }
}

TEST(RecipeFuzz, MutatedVectorsAreCanonicalOrRefused) {
    const std::vector<std::vector<std::uint8_t>> seeds = vector_seeds();
    ASSERT_EQ(seeds.size(), testapp::kEditVectors.size());
    std::mt19937 rng{kSeed + 1};
    std::size_t accepted = 0;
    for (int i = 0; i < kFuzzIterations; ++i) {
        std::vector<std::uint8_t> input = seeds[static_cast<std::size_t>(i) % seeds.size()];
        const int rounds = 1 + static_cast<int>(rng() % 3);
        for (int r = 0; r < rounds; ++r) { mutate(rng, input); }
        const auto decoded = decode_recipe(input, kLimits);
        if (decoded) {
            ++accepted;
            expect_sound(input, decoded.value());
            if (::testing::Test::HasFatalFailure()) { return; }
        }
    }
    // A mutator that only ever produces refusals is testing the first byte.
    EXPECT_GT(accepted, static_cast<std::size_t>(kFuzzIterations / 100));
}

// --- what a lie costs --------------------------------------------------------

namespace {

// Version, no flags, one freehand stroke claiming `points` points, and none of
// them present.
[[nodiscard]] std::vector<std::uint8_t> claiming(std::uint16_t points) {
    return {anvil::images::kRecipeVersion, 0x00, 0x01,
            0x00, 0xFF, 0x00, 0x00, 0xFF,     // kind, rgba
            0x01, 0x00,                       // width
            static_cast<std::uint8_t>(points >> 8), static_cast<std::uint8_t>(points & 0xFF)};
}

}  // namespace

TEST(RecipeAllocations, APointClaimTheBodyDoesNotCarryReservesNothingForIt) {
    const std::vector<std::uint8_t> input = claiming(kLimits.max_points);
    const AllocationCounter counter;
    const auto decoded = decode_recipe(input, kLimits);
    ASSERT_FALSE(decoded);
    // The one stroke record; not the 8192 coordinates it claimed.
    EXPECT_LE(counter.count(), 1U);
}

TEST(RecipeAllocations, APointClaimOverTheBoundIsRefusedBeforeAnyPointIsRead) {
    const std::vector<std::uint8_t> input = claiming(static_cast<std::uint16_t>(kLimits.max_points + 1));
    const AllocationCounter counter;
    const auto decoded = decode_recipe(input, kLimits);
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().field, anvil::images::kFaultBounds);
    EXPECT_LE(counter.count(), 1U);
}

TEST(RecipeAllocations, AStrokeCountOverTheBoundAllocatesNothing) {
    const std::vector<std::uint8_t> input{anvil::images::kRecipeVersion, 0x00,
                                          static_cast<std::uint8_t>(kLimits.max_strokes + 1)};
    const AllocationCounter counter;
    const auto decoded = decode_recipe(input, kLimits);
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().field, anvil::images::kFaultBounds);
    EXPECT_EQ(counter.count(), 0U);
}
