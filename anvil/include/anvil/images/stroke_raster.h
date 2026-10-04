#pragma once

// Drawing a recipe's strokes into decoded pixels (docs/21-image-edits.md §4.2).
//
// Our own arithmetic rather than an SVG document handed to librsvg. SVG is
// refused on upload by name because it is a document format with a parser
// behind it (docs/07-filesystem.md §5), and writing one server-side from request
// data would put that parser back on the request path with the escaping of
// every number as the only thing in between.
//
// The shape is chosen so the server's pixels match the browser's preview, which
// draws each stroke as one SVG <path> with round caps, round joins and
// stroke-opacity:
//
//   * a stroke is the UNION of capsules, one per segment, at constant width;
//   * coverage is the MAXIMUM over segments, so a stroke that crosses itself
//     does not darken where it overlaps — one path paints its overlap once;
//   * each stroke is composited once, source-over, in sRGB and not linear light,
//     with straight alpha.
//
// Edge anti-aliasing may differ from a browser's by a level. Geometry, colour
// and overlap may not.
//
// Pure CPU, no libvips: the caller hands in a decoded buffer. Memory is the
// buffer plus ONE stroke's coverage mask at a time, sized to that stroke's
// bounding box and freed before the next is built.

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include "anvil/core/result.h"
#include "anvil/images/recipe.h"

namespace anvil::images {

// Interleaved 8-bit sRGB, three bands or four (straight alpha).
struct PixelCanvas final {
    std::span<std::uint8_t> pixels;
    std::uint32_t           width;
    std::uint32_t           height;
    std::uint8_t            bands;
};

struct CanvasPoint final {
    double x;
    double y;
};

// One stroke, already in canvas pixels. `radius` is half the stroke width and
// is never below half a pixel: a hairline narrower than a pixel is drawn as one
// pixel, which is what a browser does with it too.
//
// Returns false when `deadline` passed mid-stroke. The cost of a stroke is its
// length times its width, and the recipe bounds both; the deadline is what
// bounds a recipe that is legal and still expensive.
[[nodiscard]] bool draw_stroke(PixelCanvas canvas, std::span<const CanvasPoint> points,
                               double radius, std::array<std::uint8_t, 4> rgba,
                               std::vector<std::uint8_t>& mask_scratch,
                               std::chrono::steady_clock::time_point deadline);

// Every stroke of `recipe`, mapped from the oriented master through the crop and
// the resize in `plan` onto a canvas of plan.out_width × plan.out_height.
// Never ValidationFailed: the recipe was checked before it got here. Fails with
// UnsupportedMedia and image.timeout when the deadline passed, the answer every
// other image operation gives for the same thing.
[[nodiscard]] Status draw_recipe_strokes(PixelCanvas canvas, const Recipe& recipe,
                                         const EditPlan& plan,
                                         std::chrono::steady_clock::time_point deadline);

}  // namespace anvil::images
