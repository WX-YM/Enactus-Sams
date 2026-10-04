#pragma once

// The success bodies of the two image edit routes (anvil/media/edit_routes.h),
// in a header of their own so an application's route descriptions — which a
// descriptor emitter compiles against the foundation alone — can name them
// without pulling in the driver or the image subsystem.

#include <array>

#include "anvil/http/response_spec.h"

namespace anvil::media {

// The two success bodies, published so the application's route descriptions
// declare exactly what these handlers write: the handlers write through
// write_object over these same arrays, so a description naming them cannot
// disagree with the bytes.
inline constexpr std::array<http::ResponseField, 3> kEditResponse{{
    {"id", http::FieldKind::Uuid, false},
    {"width", http::FieldKind::Int, false},
    {"height", http::FieldKind::Int, false},
}};

inline constexpr std::array<http::ResponseField, 4> kEditStateResponse{{
    // The object an editor opens on: this one, or the one it was rendered from.
    {"source", http::FieldKind::Uuid, false},
    // The SOURCE's pixels, which is what a crop's size check is a statement
    // about.
    {"width", http::FieldKind::Int, false},
    {"height", http::FieldKind::Int, false},
    // The canonical recipe as base64url, null when the object is not an edit.
    {"recipe", http::FieldKind::String, true},
}};

static_assert(http::response_shape_is_well_formed(kEditResponse));
static_assert(http::response_shape_is_well_formed(kEditStateResponse));

}  // namespace anvil::media
