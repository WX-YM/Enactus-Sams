#include "anvil/images/recipe.h"

#include <algorithm>

namespace anvil::images {
namespace {

constexpr std::uint8_t kFlagTurns = 0x03;
constexpr std::uint8_t kFlagFlip = 0x04;
constexpr std::uint8_t kFlagCrop = 0x08;
constexpr std::uint8_t kFlagResize = 0x10;
constexpr std::uint8_t kFlagReserved = 0xE0;

constexpr std::uint8_t kStrokeFreehand = 0;

// A bounds-checked big-endian reader. Every read reports whether it had the
// bytes, so a truncated recipe is a format fault and never a read past the end.
class Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_{bytes}, at_{0} {}

    [[nodiscard]] bool u8(std::uint8_t& out) noexcept {
        if (at_ + 1 > bytes_.size()) { return false; }
        out = bytes_[at_];
        at_ += 1;
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& out) noexcept {
        if (at_ + 2 > bytes_.size()) { return false; }
        out = static_cast<std::uint16_t>((bytes_[at_] << 8) | bytes_[at_ + 1]);
        at_ += 2;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - at_; }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t                   at_;
};

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

[[nodiscard]] std::uint32_t scale(std::uint32_t fraction, std::uint32_t extent) noexcept {
    return static_cast<std::uint32_t>(
        round_div(static_cast<std::uint64_t>(fraction) * extent, kFixedOne));
}

}  // namespace

input::FieldError field_error(std::string_view fault) noexcept {
    using input::Reason;
    if (fault == kFaultEmpty) { return {"recipe", Reason::Required}; }
    if (fault == kFaultCrop) { return {"recipe.crop", Reason::OutOfRange}; }
    if (fault == kFaultTooSmall) { return {"recipe", Reason::TooShort}; }
    if (fault == kFaultUpscale) { return {"recipe.resize", Reason::OutOfRange}; }
    if (fault == kFaultStroke) { return {"recipe.strokes", Reason::BadFormat}; }
    if (fault == kFaultBounds) { return {"recipe.strokes", Reason::TooLong}; }
    if (fault == kFaultNotSource) { return {"id", Reason::NotAllowed}; }
    return {"recipe", Reason::BadFormat};
}

void encode_recipe(const Recipe& recipe, std::vector<std::uint8_t>& out) {
    out.reserve(out.size() + 3 + 8 + 2 + recipe.strokes.size() * 9 + recipe.points.size() * 2);
    out.push_back(kRecipeVersion);
    std::uint8_t flags = static_cast<std::uint8_t>(recipe.turns & kFlagTurns);
    if (recipe.flip) { flags |= kFlagFlip; }
    if (recipe.crop.has_value()) { flags |= kFlagCrop; }
    if (recipe.long_edge_px.has_value()) { flags |= kFlagResize; }
    out.push_back(flags);
    if (recipe.crop.has_value()) {
        put_u16(out, recipe.crop->x);
        put_u16(out, recipe.crop->y);
        put_u16(out, recipe.crop->w);
        put_u16(out, recipe.crop->h);
    }
    if (recipe.long_edge_px.has_value()) { put_u16(out, *recipe.long_edge_px); }
    out.push_back(static_cast<std::uint8_t>(recipe.strokes.size()));
    for (const RecipeStroke& stroke : recipe.strokes) {
        out.push_back(kStrokeFreehand);
        out.insert(out.end(), stroke.rgba.begin(), stroke.rgba.end());
        put_u16(out, stroke.width);
        put_u16(out, stroke.point_count);
        const std::size_t first = static_cast<std::size_t>(stroke.first_point) * 2;
        for (std::size_t i = 0; i < static_cast<std::size_t>(stroke.point_count) * 2; ++i) {
            put_u16(out, recipe.points[first + i]);
        }
    }
}

Result<Recipe> decode_recipe(std::span<const std::uint8_t> bytes, const EditLimits& limits) {
    Reader in{bytes};
    std::uint8_t version = 0;
    std::uint8_t flags = 0;
    if (!in.u8(version) || !in.u8(flags)) { return fail(ErrorCode::ValidationFailed, kFaultFormat); }
    // A bit this build ignores is a bit the next build reads, and the two would
    // then render one recipe two ways.
    if (version != kRecipeVersion || (flags & kFlagReserved) != 0) {
        return fail(ErrorCode::ValidationFailed, kFaultFormat);
    }

    Recipe recipe{};
    recipe.turns = static_cast<std::uint8_t>(flags & kFlagTurns);
    recipe.flip = (flags & kFlagFlip) != 0;

    if ((flags & kFlagCrop) != 0) {
        FixedRect rect{};
        if (!in.u16(rect.x) || !in.u16(rect.y) || !in.u16(rect.w) || !in.u16(rect.h)) {
            return fail(ErrorCode::ValidationFailed, kFaultFormat);
        }
        if (rect.w == 0 || rect.h == 0 ||
            static_cast<std::uint32_t>(rect.x) + rect.w > kFixedOne ||
            static_cast<std::uint32_t>(rect.y) + rect.h > kFixedOne) {
            return fail(ErrorCode::ValidationFailed, kFaultCrop);
        }
        // The whole frame is the ABSENT crop spelled a second way.
        if (rect.x == 0 && rect.y == 0 && rect.w == kFixedOne && rect.h == kFixedOne) {
            return fail(ErrorCode::ValidationFailed, kFaultCanonical);
        }
        recipe.crop = rect;
    }

    if ((flags & kFlagResize) != 0) {
        std::uint16_t long_edge = 0;
        if (!in.u16(long_edge)) { return fail(ErrorCode::ValidationFailed, kFaultFormat); }
        if (long_edge == 0) { return fail(ErrorCode::ValidationFailed, kFaultUpscale); }
        recipe.long_edge_px = long_edge;
    }

    std::uint8_t stroke_count = 0;
    if (!in.u8(stroke_count)) { return fail(ErrorCode::ValidationFailed, kFaultFormat); }
    if (stroke_count > limits.max_strokes) {
        return fail(ErrorCode::ValidationFailed, kFaultBounds);
    }

    recipe.strokes.reserve(stroke_count);
    std::uint32_t total_points = 0;
    for (std::uint8_t s = 0; s < stroke_count; ++s) {
        std::uint8_t kind = 0;
        RecipeStroke stroke{};
        if (!in.u8(kind) || !in.u8(stroke.rgba[0]) || !in.u8(stroke.rgba[1]) ||
            !in.u8(stroke.rgba[2]) || !in.u8(stroke.rgba[3]) || !in.u16(stroke.width) ||
            !in.u16(stroke.point_count)) {
            return fail(ErrorCode::ValidationFailed, kFaultFormat);
        }
        if (kind != kStrokeFreehand) { return fail(ErrorCode::ValidationFailed, kFaultFormat); }
        // Invisible content is still content that is hashed, and a stroke nobody
        // can see is a second recipe for the picture without it.
        if (stroke.rgba[3] == 0 || stroke.width == 0 || stroke.width > kMaxStrokeWidth ||
            stroke.point_count == 0) {
            return fail(ErrorCode::ValidationFailed, kFaultStroke);
        }
        total_points += stroke.point_count;
        // Checked BEFORE the points are read, so a recipe claiming a million
        // points costs a comparison and not an allocation.
        if (total_points > limits.max_points) {
            return fail(ErrorCode::ValidationFailed, kFaultBounds);
        }
        if (in.remaining() < static_cast<std::size_t>(stroke.point_count) * 4) {
            return fail(ErrorCode::ValidationFailed, kFaultFormat);
        }
        stroke.first_point = total_points - stroke.point_count;
        for (std::uint32_t p = 0; p < static_cast<std::uint32_t>(stroke.point_count) * 2; ++p) {
            std::uint16_t coordinate = 0;
            if (!in.u16(coordinate)) { return fail(ErrorCode::ValidationFailed, kFaultFormat); }
            recipe.points.push_back(coordinate);
        }
        recipe.strokes.push_back(stroke);
    }

    // A recipe with a tail is two recipes.
    if (in.remaining() != 0) { return fail(ErrorCode::ValidationFailed, kFaultFormat); }

    // The identity edit is the source itself; the caller binds that.
    if (recipe.turns == 0 && !recipe.flip && !recipe.crop.has_value() &&
        !recipe.long_edge_px.has_value() && recipe.strokes.empty()) {
        return fail(ErrorCode::ValidationFailed, kFaultEmpty);
    }

    // The canonical check, stated once rather than rule by rule: whatever was
    // read must be exactly what this recipe encodes to. The rules above give a
    // refusal its name; this is what makes the list complete.
    std::vector<std::uint8_t> again;
    encode_recipe(recipe, again);
    if (!std::equal(again.begin(), again.end(), bytes.begin(), bytes.end())) {
        return fail(ErrorCode::ValidationFailed, kFaultCanonical);
    }
    return recipe;
}

Result<EditPlan> plan_edit(const Recipe& recipe, ImageInfo master, const EditLimits& limits) {
    EditPlan plan{};
    const bool sideways = (recipe.turns & 1) != 0;
    plan.oriented_width = sideways ? master.height : master.width;
    plan.oriented_height = sideways ? master.width : master.height;

    if (recipe.crop.has_value()) {
        const FixedRect rect = *recipe.crop;
        // Both edges are rounded, never an edge and an extent: rounding an
        // extent can put the far edge one pixel outside the image, which libvips
        // reports as an error rather than clipping.
        const std::uint32_t left = scale(rect.x, plan.oriented_width);
        const std::uint32_t top = scale(rect.y, plan.oriented_height);
        const std::uint32_t right = scale(rect.x + rect.w, plan.oriented_width);
        const std::uint32_t bottom = scale(rect.y + rect.h, plan.oriented_height);
        if (right <= left || bottom <= top) {
            return fail(ErrorCode::ValidationFailed, kFaultCrop);
        }
        plan.crop = PixelBox{left, top, right - left, bottom - top};
    } else {
        plan.crop = PixelBox{0, 0, plan.oriented_width, plan.oriented_height};
    }

    const std::uint32_t cropped_long = std::max(plan.crop.width, plan.crop.height);
    std::uint32_t target_long = cropped_long;
    if (recipe.long_edge_px.has_value()) {
        // Never upscaled, the rule every variant already obeys. Equal is the
        // absent resize spelled a second way, so it is refused with the same
        // word: a client that offers it is offering nothing.
        if (*recipe.long_edge_px >= cropped_long) {
            return fail(ErrorCode::ValidationFailed, kFaultUpscale);
        }
        target_long = *recipe.long_edge_px;
    }
    // The derived master is capped at the widest rung: nothing ever serves an
    // edit above it (docs/21-image-edits.md §4.1).
    target_long = std::min<std::uint32_t>(target_long, limits.max_edge_px);

    if (target_long == cropped_long) {
        plan.out_width = plan.crop.width;
        plan.out_height = plan.crop.height;
    } else {
        plan.out_width = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(round_div(
                   static_cast<std::uint64_t>(plan.crop.width) * target_long, cropped_long)));
        plan.out_height = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(round_div(
                   static_cast<std::uint64_t>(plan.crop.height) * target_long, cropped_long)));
    }

    // Narrower than the narrowest rung, every variant would be an upscale — a
    // worse picture than the one the person started from.
    if (plan.out_width < limits.min_edge_px) {
        return fail(ErrorCode::ValidationFailed, kFaultTooSmall);
    }
    return plan;
}

}  // namespace anvil::images
