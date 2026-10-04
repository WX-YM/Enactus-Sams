// The edit recipe (docs/21-image-edits.md §2).
//
// The codec is checked against vectors produced by an implementation that is
// neither this one nor the client's, because the recipe is a byte-exact
// contract and the thing it has to be checked against is the document.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "anvil/crypto/base64url.h"
#include "anvil/images/recipe.h"
#include "testapp/edit_vectors.h"

namespace {

namespace images = anvil::images;
using anvil::ErrorCode;

[[nodiscard]] std::vector<std::uint8_t> bytes_of(std::string_view b64) {
    const auto decoded = anvil::crypto::base64url_decode(b64);
    EXPECT_TRUE(decoded.has_value()) << b64;
    return decoded.value_or(std::vector<std::uint8_t>{});
}

TEST(EditRecipe, EveryVectorDecodesAndPlansToItsExpectedOutcome) {
    for (const testapp::EditVector& vector : testapp::kEditVectors) {
        SCOPED_TRACE(std::string{vector.name});
        const std::vector<std::uint8_t> bytes = bytes_of(vector.recipe);
        const anvil::Result<images::Recipe> decoded =
            images::decode_recipe(bytes, testapp::kEditVectorLimits);
        if (!decoded) {
            EXPECT_EQ(decoded.error().field, vector.fault);
            EXPECT_EQ(decoded.error().code, ErrorCode::ValidationFailed);
            continue;
        }
        const anvil::Result<images::EditPlan> plan = images::plan_edit(
            decoded.value(), images::ImageInfo{vector.source_width, vector.source_height, 1},
            testapp::kEditVectorLimits);
        if (!plan) {
            EXPECT_EQ(plan.error().field, vector.fault);
            continue;
        }
        EXPECT_TRUE(vector.fault.empty()) << "expected " << vector.fault;
        EXPECT_EQ(plan.value().out_width, vector.out_width);
        EXPECT_EQ(plan.value().out_height, vector.out_height);
    }
}

TEST(EditRecipe, EveryAcceptedRecipeReencodesToItsOwnBytes) {
    for (const testapp::EditVector& vector : testapp::kEditVectors) {
        SCOPED_TRACE(std::string{vector.name});
        const std::vector<std::uint8_t> bytes = bytes_of(vector.recipe);
        const anvil::Result<images::Recipe> decoded =
            images::decode_recipe(bytes, testapp::kEditVectorLimits);
        if (!decoded) { continue; }
        std::vector<std::uint8_t> again;
        images::encode_recipe(decoded.value(), again);
        EXPECT_EQ(again, bytes);
    }
}

TEST(EditRecipe, TheVectorsCoverEveryFault) {
    // A fault no vector exercises is a fault whose wire spelling only one side
    // has ever produced.
    for (const std::string_view fault :
         {images::kFaultFormat, images::kFaultCanonical, images::kFaultEmpty, images::kFaultCrop,
          images::kFaultTooSmall, images::kFaultUpscale, images::kFaultStroke,
          images::kFaultBounds}) {
        const bool covered = std::any_of(
            testapp::kEditVectors.begin(), testapp::kEditVectors.end(),
            [fault](const testapp::EditVector& vector) { return vector.fault == fault; });
        EXPECT_TRUE(covered) << fault;
    }
}

TEST(EditRecipe, FaultsReachTheWireAsAFieldAndAReason) {
    using anvil::input::Reason;
    EXPECT_EQ(images::field_error(images::kFaultBounds).field, "recipe.strokes");
    EXPECT_EQ(images::field_error(images::kFaultBounds).reason, Reason::TooLong);
    EXPECT_EQ(images::field_error(images::kFaultCrop).field, "recipe.crop");
    EXPECT_EQ(images::field_error(images::kFaultUpscale).field, "recipe.resize");
    EXPECT_EQ(images::field_error(images::kFaultEmpty).reason, Reason::Required);
    EXPECT_EQ(images::field_error(images::kFaultNotSource).field, "id");
    // A fault only this file mints, misspelled, is still a refusal of the
    // recipe as a whole rather than something that reads as success.
    EXPECT_EQ(images::field_error("edit.nonsense").reason, Reason::BadFormat);
}

TEST(EditRecipe, EveryPrefixOfAValidRecipeIsRefused) {
    // Truncation anywhere must be a format fault, never a read past the end or
    // a shorter recipe that happens to be valid.
    const std::vector<std::uint8_t> bytes = bytes_of(testapp::kEditVectors[5].recipe);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const anvil::Result<images::Recipe> decoded = images::decode_recipe(
            std::span{bytes}.first(length), testapp::kEditVectorLimits);
        EXPECT_FALSE(decoded.ok()) << length;
    }
}

}  // namespace
