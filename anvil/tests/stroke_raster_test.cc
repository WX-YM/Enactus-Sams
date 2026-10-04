// The stroke rasteriser (docs/21-image-edits.md §4.2).
//
// Each case pins one property the browser's SVG preview has, because the whole
// point of drawing it ourselves is that the server's pixels and the preview
// agree: round caps, overlap painted once, source-over in sRGB.

#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "anvil/images/recipe.h"
#include "anvil/images/stroke_raster.h"
#include "testapp/edit_vectors.h"

namespace {

namespace images = anvil::images;

[[nodiscard]] std::chrono::steady_clock::time_point far_future() {
    return std::chrono::steady_clock::now() + std::chrono::hours{1};
}

struct Canvas final {
    std::vector<std::uint8_t> pixels;
    std::uint32_t             width;
    std::uint32_t             height;
    std::uint8_t              bands;

    Canvas(std::uint32_t w, std::uint32_t h, std::uint8_t b, std::uint8_t fill)
        : pixels(static_cast<std::size_t>(w) * h * b, fill), width{w}, height{h}, bands{b} {}

    [[nodiscard]] images::PixelCanvas view() {
        return images::PixelCanvas{pixels, width, height, bands};
    }
    [[nodiscard]] const std::uint8_t* at(std::uint32_t x, std::uint32_t y) const {
        return pixels.data() + (static_cast<std::size_t>(y) * width + x) * bands;
    }
};

TEST(StrokeRaster, ADotIsARoundDiscOfTheGivenRadius) {
    Canvas canvas{41, 41, 3, 255};
    std::vector<std::uint8_t> mask;
    const images::CanvasPoint centre{20.5, 20.5};
    ASSERT_TRUE(images::draw_stroke(canvas.view(), std::span{&centre, 1}, 8.0,
                                    {0, 0, 0, 255}, mask, far_future()));
    EXPECT_EQ(canvas.at(20, 20)[0], 0);          // the centre, fully covered
    EXPECT_EQ(canvas.at(20, 13)[0], 0);          // 7 px from the centre, inside
    EXPECT_EQ(canvas.at(20, 10)[0], 255);        // 10 px from the centre, outside
    // Round, not square: the corner of the bounding square is 11 px away.
    EXPECT_EQ(canvas.at(28, 28)[0], 255);
    // Symmetric about the centre, which a half-pixel error in the sample point
    // would break.
    EXPECT_EQ(canvas.at(12, 20)[0], canvas.at(28, 20)[0]);
    EXPECT_EQ(canvas.at(20, 12)[0], canvas.at(20, 28)[0]);
}

TEST(StrokeRaster, AStrokeThatCrossesItselfDoesNotDarkenAtTheCrossing) {
    Canvas canvas{60, 60, 3, 255};
    std::vector<std::uint8_t> mask;
    // An X: the path passes through the centre twice.
    const std::vector<images::CanvasPoint> path{{10, 10}, {50, 50}, {50, 10}, {10, 50}};
    ASSERT_TRUE(images::draw_stroke(canvas.view(), path, 3.0, {0, 0, 0, 128}, mask,
                                    far_future()));
    const std::uint8_t on_one_arm = canvas.at(20, 20)[0];
    const std::uint8_t at_the_crossing = canvas.at(30, 30)[0];
    EXPECT_LT(on_one_arm, 255);
    // Half-transparent black over white, painted ONCE: a sum would be darker.
    EXPECT_EQ(at_the_crossing, on_one_arm);
}

TEST(StrokeRaster, AHairlineStillDrawsOnePixel) {
    Canvas canvas{20, 20, 3, 255};
    std::vector<std::uint8_t> mask;
    const std::vector<images::CanvasPoint> path{{2, 10.5}, {18, 10.5}};
    ASSERT_TRUE(
        images::draw_stroke(canvas.view(), path, 0.01, {0, 0, 0, 255}, mask, far_future()));
    EXPECT_EQ(canvas.at(10, 10)[0], 0);
}

TEST(StrokeRaster, SourceOverInSrgbWithStraightAlpha) {
    Canvas opaque{8, 8, 3, 200};
    std::vector<std::uint8_t> mask;
    const images::CanvasPoint centre{4, 4};
    ASSERT_TRUE(images::draw_stroke(opaque.view(), std::span{&centre, 1}, 3.0, {0, 0, 0, 128},
                                    mask, far_future()));
    // 200 × (1 − 128/255) = 99.6, in sRGB values rather than linear light.
    EXPECT_EQ(opaque.at(4, 4)[0], 100);

    Canvas clear{8, 8, 4, 0};
    ASSERT_TRUE(images::draw_stroke(clear.view(), std::span{&centre, 1}, 3.0,
                                    {10, 20, 30, 128}, mask, far_future()));
    // Over transparency the colour is the stroke's own, and the alpha is its.
    EXPECT_EQ(clear.at(4, 4)[0], 10);
    EXPECT_EQ(clear.at(4, 4)[2], 30);
    EXPECT_EQ(clear.at(4, 4)[3], 128);
}

TEST(StrokeRaster, TheMaskNeverExceedsTheStrokesClippedBox) {
    Canvas canvas{1000, 1000, 3, 255};
    std::vector<std::uint8_t> mask;
    const std::vector<images::CanvasPoint> path{{100, 100}, {120, 100}};
    ASSERT_TRUE(
        images::draw_stroke(canvas.view(), path, 2.0, {0, 0, 0, 255}, mask, far_future()));
    // A twenty-pixel line costs a box around twenty pixels, not the canvas.
    EXPECT_LT(mask.size(), 40U * 12U);

    // Entirely off the canvas is nothing at all, not a clamped smear on its edge.
    const std::vector<std::uint8_t> before = canvas.pixels;
    const std::vector<images::CanvasPoint> outside{{-500, -500}, {-400, -400}};
    ASSERT_TRUE(
        images::draw_stroke(canvas.view(), outside, 2.0, {0, 0, 0, 255}, mask, far_future()));
    EXPECT_EQ(canvas.pixels, before);
}

TEST(StrokeRaster, APassedDeadlineStopsTheStroke) {
    Canvas canvas{2000, 2000, 3, 255};
    std::vector<std::uint8_t> mask;
    std::vector<images::CanvasPoint> zigzag;
    for (int i = 0; i < 64; ++i) {
        zigzag.push_back({i % 2 == 0 ? 0.0 : 2000.0, i % 2 == 0 ? 0.0 : 2000.0});
    }
    EXPECT_FALSE(images::draw_stroke(canvas.view(), zigzag, 100.0, {0, 0, 0, 255}, mask,
                                     std::chrono::steady_clock::now()));
}

TEST(StrokeRaster, RecipeStrokesAreMappedThroughTheCropAndTheResize) {
    // A dot at the centre of the oriented master, with the crop taking the
    // right half: the dot lands on the crop's left edge, vertically centred.
    images::Recipe recipe{};
    recipe.crop = images::FixedRect{32768, 0, 32767, 65535};
    recipe.points = {32768, 32768};
    recipe.strokes.push_back(images::RecipeStroke{{0, 0, 0, 255}, 3277, 1, 0});
    const anvil::Result<images::EditPlan> plan = images::plan_edit(
        recipe, images::ImageInfo{2000, 1000, 1}, testapp::kEditVectorLimits);
    ASSERT_TRUE(plan.ok());
    ASSERT_EQ(plan.value().out_width, 1000U);

    Canvas canvas{plan.value().out_width, plan.value().out_height, 3, 255};
    ASSERT_TRUE(images::draw_recipe_strokes(canvas.view(), recipe, plan.value(), far_future())
                    .ok());
    EXPECT_EQ(canvas.at(1, 500)[0], 0);
    EXPECT_EQ(canvas.at(500, 500)[0], 255);
}

}  // namespace
