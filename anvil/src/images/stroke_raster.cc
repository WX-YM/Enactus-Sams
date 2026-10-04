#include "anvil/images/stroke_raster.h"

#include <algorithm>
#include <cmath>

namespace anvil::images {
namespace {

struct Box final {
    std::int64_t left;
    std::int64_t top;
    std::int64_t right;    // exclusive
    std::int64_t bottom;   // exclusive
};

// Distance from (px, py) to the segment a→b. A zero-length segment is a point,
// which is how a one-point stroke draws as a dot.
[[nodiscard]] double segment_distance(double px, double py, CanvasPoint a,
                                      CanvasPoint b) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length_sq = dx * dx + dy * dy;
    double t = 0.0;
    if (length_sq > 0.0) {
        t = std::clamp(((px - a.x) * dx + (py - a.y) * dy) / length_sq, 0.0, 1.0);
    }
    const double cx = a.x + t * dx - px;
    const double cy = a.y + t * dy - py;
    return std::sqrt(cx * cx + cy * cy);
}

// Coverage of the pixel whose centre is at distance `d` from the stroke's
// spine: a one-pixel ramp centred on the edge. 0–255.
[[nodiscard]] std::uint8_t coverage(double d, double radius) noexcept {
    const double c = std::clamp(radius + 0.5 - d, 0.0, 1.0);
    return static_cast<std::uint8_t>(std::lround(c * 255.0));
}

void blend(std::uint8_t* pixel, std::uint8_t bands, std::array<std::uint8_t, 4> rgba,
           std::uint8_t mask) noexcept {
    const double a = (static_cast<double>(mask) / 255.0) * (static_cast<double>(rgba[3]) / 255.0);
    if (bands == 4) {
        const double below = static_cast<double>(pixel[3]) / 255.0;
        const double out_alpha = a + below * (1.0 - a);
        if (out_alpha <= 0.0) { return; }
        for (std::size_t c = 0; c < 3; ++c) {
            const double value =
                (rgba[c] * a + pixel[c] * below * (1.0 - a)) / out_alpha;
            pixel[c] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
        }
        pixel[3] = static_cast<std::uint8_t>(std::lround(out_alpha * 255.0));
        return;
    }
    for (std::size_t c = 0; c < 3; ++c) {
        const double value = rgba[c] * a + pixel[c] * (1.0 - a);
        pixel[c] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
    }
}

}  // namespace

bool draw_stroke(PixelCanvas canvas, std::span<const CanvasPoint> points, double radius,
                 std::array<std::uint8_t, 4> rgba, std::vector<std::uint8_t>& mask_scratch,
                 std::chrono::steady_clock::time_point deadline) {
    if (points.empty() || canvas.width == 0 || canvas.height == 0) { return true; }
    radius = std::max(radius, 0.5);
    // Everything a pixel can be covered from: the spine's box grown by the
    // radius and the one-pixel ramp.
    const double reach = radius + 1.0;

    double min_x = points[0].x;
    double max_x = points[0].x;
    double min_y = points[0].y;
    double max_y = points[0].y;
    for (const CanvasPoint& point : points) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    const Box box{
        std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(min_x - reach))),
        std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(min_y - reach))),
        std::min<std::int64_t>(canvas.width, static_cast<std::int64_t>(std::ceil(max_x + reach)) + 1),
        std::min<std::int64_t>(canvas.height, static_cast<std::int64_t>(std::ceil(max_y + reach)) + 1),
    };
    if (box.right <= box.left || box.bottom <= box.top) { return true; }

    const auto box_width = static_cast<std::size_t>(box.right - box.left);
    const auto box_height = static_cast<std::size_t>(box.bottom - box.top);
    // Resized rather than reallocated: one scratch buffer serves every stroke of
    // a recipe, and it is never larger than the canvas.
    mask_scratch.assign(box_width * box_height, 0);

    std::uint32_t rows_since_check = 0;
    const std::size_t segments = points.size() == 1 ? 1 : points.size() - 1;
    for (std::size_t s = 0; s < segments; ++s) {
        const CanvasPoint a = points[s];
        const CanvasPoint b = points.size() == 1 ? points[0] : points[s + 1];
        const double seg_min_x = std::min(a.x, b.x) - reach;
        const double seg_max_x = std::max(a.x, b.x) + reach;
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double length = std::sqrt(dx * dx + dy * dy);

        const std::int64_t row_first = std::max<std::int64_t>(
            box.top, static_cast<std::int64_t>(std::floor(std::min(a.y, b.y) - reach)));
        const std::int64_t row_last = std::min<std::int64_t>(
            box.bottom, static_cast<std::int64_t>(std::ceil(std::max(a.y, b.y) + reach)) + 1);

        for (std::int64_t row = row_first; row < row_last; ++row) {
            // A clock read per row would dominate a thin stroke's cost; one per
            // few thousand rows still stops a pathological recipe within
            // milliseconds of its deadline.
            if (++rows_since_check >= 4096) {
                rows_since_check = 0;
                if (std::chrono::steady_clock::now() > deadline) { return false; }
            }
            const double yc = static_cast<double>(row) + 0.5;
            double from = seg_min_x;
            double to = seg_max_x;
            // Only the pixels inside the band around the segment's LINE can be
            // covered, so a long diagonal costs its own area rather than its
            // bounding box's. A near-horizontal segment is already bounded by
            // its box.
            if (std::abs(dy) > 1e-9) {
                const double centre = a.x + (yc - a.y) * dx / dy;
                const double half = reach * length / std::abs(dy);
                from = std::max(from, centre - half);
                to = std::min(to, centre + half);
            }
            const std::int64_t col_first =
                std::max<std::int64_t>(box.left, static_cast<std::int64_t>(std::floor(from)));
            const std::int64_t col_last =
                std::min<std::int64_t>(box.right, static_cast<std::int64_t>(std::ceil(to)) + 1);
            std::uint8_t* mask_row =
                mask_scratch.data() + static_cast<std::size_t>(row - box.top) * box_width;
            for (std::int64_t col = col_first; col < col_last; ++col) {
                const double d = segment_distance(static_cast<double>(col) + 0.5, yc, a, b);
                const std::uint8_t c = coverage(d, radius);
                std::uint8_t& cell = mask_row[col - box.left];
                // The MAXIMUM, never a sum: one path paints its own overlap once.
                if (c > cell) { cell = c; }
            }
        }
    }

    for (std::size_t row = 0; row < box_height; ++row) {
        const std::size_t y = static_cast<std::size_t>(box.top) + row;
        for (std::size_t col = 0; col < box_width; ++col) {
            const std::uint8_t m = mask_scratch[row * box_width + col];
            if (m == 0) { continue; }
            const std::size_t x = static_cast<std::size_t>(box.left) + col;
            blend(canvas.pixels.data() + (y * canvas.width + x) * canvas.bands, canvas.bands,
                  rgba, m);
        }
    }
    return true;
}

Status draw_recipe_strokes(PixelCanvas canvas, const Recipe& recipe, const EditPlan& plan,
                           std::chrono::steady_clock::time_point deadline) {
    if (recipe.strokes.empty()) { return ok(); }

    // Oriented fraction → oriented pixel → output pixel. Doubles, and that is
    // safe here in a way it is not in the codec: nothing drawn is hashed.
    const double ow = plan.oriented_width;
    const double oh = plan.oriented_height;
    const double sx = static_cast<double>(plan.out_width) / plan.crop.width;
    const double sy = static_cast<double>(plan.out_height) / plan.crop.height;
    const double short_edge = std::min(ow, oh);

    std::vector<CanvasPoint> mapped;
    std::vector<std::uint8_t> mask;
    for (const RecipeStroke& stroke : recipe.strokes) {
        mapped.clear();
        mapped.reserve(stroke.point_count);
        const std::size_t first = static_cast<std::size_t>(stroke.first_point) * 2;
        for (std::size_t p = 0; p < stroke.point_count; ++p) {
            const double x = recipe.points[first + p * 2] * ow / kFixedOne;
            const double y = recipe.points[first + p * 2 + 1] * oh / kFixedOne;
            mapped.push_back(CanvasPoint{(x - plan.crop.left) * sx, (y - plan.crop.top) * sy});
        }
        const double radius = stroke.width * short_edge / kFixedOne * sx / 2.0;
        if (!draw_stroke(canvas, mapped, radius, stroke.rgba, mask, deadline)) {
            return fail(ErrorCode::UnsupportedMedia, kRejectTimeout);
        }
    }
    return ok();
}

}  // namespace anvil::images
