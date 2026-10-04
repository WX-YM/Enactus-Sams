#pragma once

// An image edit, as a canonical recipe (docs/21-image-edits.md §2).
//
// An edit is a small document naming what to do to a master — orient, draw,
// crop, resize — and it is rendered ONCE, by the server, into a new object. The
// recipe is the thing that is hashed, stored and reopened, so it has exactly one
// encoding per edit: the decoder re-encodes what it read and refuses anything
// that is not byte-identical. That single comparison is what makes every
// canonical rule enforceable at once, and a hash of the bytes a sound key.
//
// Pure arithmetic: no libvips, no driver. The client's codec
// (hammer `src/edit/recipe.ts`) is the other implementation of this file, and
// the vectors in tests/testapp/edit_vectors.h are the contract between them.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "anvil/core/result.h"
#include "anvil/images/probe.h"
#include "anvil/input/fields.h"

namespace anvil::images {

inline constexpr std::uint8_t kRecipeVersion = 1;

// A coordinate is a u16 FRACTION of its frame's extent. At the 12 000 px
// dimension cap one step is 0.18 px, finer than any pointer. Floats are refused
// because 0.1 in a JSON body, a JS number and a C++ float are three values, and
// three values are three hashes of one edit.
inline constexpr std::uint32_t kFixedOne = 65535;

// A stroke is at most an eighth of the oriented short edge wide. Wider is a
// fill rather than a stroke, and it is what bounds the rasteriser's work: the
// cost of a stroke is its length times its width (§4.2).
inline constexpr std::uint16_t kMaxStrokeWidth = 8192;

// The bounds the descriptor publishes as `limits.edit`, so a client refuses
// exactly what the server does (docs/21-image-edits.md §5).
struct EditLimits final {
    std::uint16_t max_strokes;
    std::uint16_t max_points;
    // The widest and narrowest rungs of the ladder. The derived master is
    // capped at the first, and a result narrower than the second is refused,
    // because every variant of it would be an upscale.
    std::uint16_t max_edge_px;
    std::uint16_t min_edge_px;
};

inline constexpr std::uint16_t kMaxEditStrokes = 64;
inline constexpr std::uint16_t kMaxEditPoints = 4096;

// --- faults -------------------------------------------------------------------
//
// Carried in Failure::field, so the audit row records the precise cause. Unlike
// the upload path's UNSUPPORTED_MEDIA, a recipe is not a probe of which decoders
// exist, so the client is told too — through `field_error`, in the ordinary
// VALIDATION_FAILED vocabulary rather than in a second one.

inline constexpr std::string_view kFaultFormat = "edit.format";
inline constexpr std::string_view kFaultCanonical = "edit.canonical";
inline constexpr std::string_view kFaultEmpty = "edit.empty";
inline constexpr std::string_view kFaultCrop = "edit.crop";
inline constexpr std::string_view kFaultTooSmall = "edit.too_small";
inline constexpr std::string_view kFaultUpscale = "edit.upscale";
inline constexpr std::string_view kFaultStroke = "edit.stroke";
inline constexpr std::string_view kFaultBounds = "edit.bounds";
inline constexpr std::string_view kFaultNotSource = "edit.not_source";

// The wire half of a fault: which request field it is about and why. A schema
// constant in both halves, never text taken from the request. An unrecognised
// fault — which is a programming error, since only this file mints them — maps
// to the recipe as a whole being badly formed.
[[nodiscard]] input::FieldError field_error(std::string_view fault) noexcept;

// --- the recipe -----------------------------------------------------------------

struct FixedRect final {
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t w;
    std::uint16_t h;
};

struct RecipeStroke final {
    // sRGB, straight alpha, alpha >= 1.
    std::array<std::uint8_t, 4> rgba;
    // A fraction of the ORIENTED master's short edge.
    std::uint16_t               width;
    std::uint16_t               point_count;
    // Index of this stroke's first coordinate PAIR in Recipe::points.
    std::uint32_t               first_point;
};

struct Recipe final {
    // Every stroke's points in one array — x0 y0 x1 y1 … — as fractions of the
    // oriented master. One allocation for the whole drawing rather than one
    // per stroke.
    std::vector<std::uint16_t>   points;
    std::vector<RecipeStroke>    strokes;
    std::optional<FixedRect>     crop;
    std::optional<std::uint16_t> long_edge_px;
    // Quarter turns clockwise, then an optional horizontal flip. A vertical flip
    // is two turns and a horizontal flip, so it has no bit of its own — one
    // picture, one recipe.
    std::uint8_t                 turns;
    bool                         flip;
};

// Decodes, checks every canonical rule and every count bound, and refuses
// anything that does not re-encode to the identical bytes. Needs no source: the
// rules that depend on the master's size are plan_edit's.
[[nodiscard]] Result<Recipe> decode_recipe(std::span<const std::uint8_t> bytes,
                                           const EditLimits& limits);

// The canonical bytes. Appends to `out`.
void encode_recipe(const Recipe& recipe, std::vector<std::uint8_t>& out);

// --- geometry ------------------------------------------------------------------

struct PixelBox final {
    std::uint32_t left;
    std::uint32_t top;
    std::uint32_t width;
    std::uint32_t height;
};

// What rendering this recipe against this master produces, in pixels. Computed
// with integer arithmetic only — round half up of a rational — so the client
// computes the identical numbers and its size check agrees with this one.
struct EditPlan final {
    PixelBox      crop;          // in the oriented master
    std::uint32_t oriented_width;
    std::uint32_t oriented_height;
    std::uint32_t out_width;
    std::uint32_t out_height;
};

// The rules that need the master's dimensions: a crop that rounds to nothing,
// a resize that would upscale, a result narrower than the narrowest rung.
[[nodiscard]] Result<EditPlan> plan_edit(const Recipe& recipe, ImageInfo master,
                                         const EditLimits& limits);

// round(numerator / denominator), half up, in integers. Exposed because the
// client's arithmetic is checked against it.
[[nodiscard]] constexpr std::uint64_t round_div(std::uint64_t numerator,
                                                std::uint64_t denominator) noexcept {
    return (2 * numerator + denominator) / (2 * denominator);
}

}  // namespace anvil::images
