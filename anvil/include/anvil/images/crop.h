#pragma once

// Cropping, including the brief's "hot crop" during preview
// (docs/08-images.md §4).
//
// The crop is stored as PARAMETERS on the media-slot binding, never baked into
// a new master. That is what makes it non-destructive: it is re-croppable
// forever, the original framing is never lost, and "crop, change it, remove it"
// leaves the master byte-identical throughout.
//
// Applying a crop re-derives the variant set on cpu_pool. The slot keeps
// serving the previous variants until the new ones are ready, so a staff member
// never sees a broken image mid-job.

#include <cstdint>
#include <vector>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/paths.h"
#include "anvil/images/probe.h"
#include "anvil/images/variants.h"

namespace anvil::images {

// Normalised to [0,1] against the master's dimensions, so a crop survives the
// master being re-derived at a different size. 16 bytes, trivially copyable.
struct CropRect final {
    float x;
    float y;
    float width;
    float height;
};

static_assert(sizeof(CropRect) == 16);

inline constexpr std::string_view kRejectCropBounds = "crop.bounds";
inline constexpr std::string_view kRejectCropTooSmall = "crop.too_small";

// Inside bounds, non-zero, and at least as wide as the smallest target width —
// below that, every variant would be an upscale (docs/08-images.md §4).
[[nodiscard]] Status validate_crop(CropRect rect, ImageInfo master) noexcept;

// Re-derives the whole variant set from the master through the crop rectangle.
// The master file is opened read-only and is never written, which is the
// non-destructive guarantee expressed in code rather than in a comment.
//
// Blocking. cpu_pool only.
[[nodiscard]] Result<std::vector<VariantRecord>> apply_crop(const fs::Storage& storage,
                                                            fs::Ns ns, const Uuid& id,
                                                            int master_fd, ImageInfo master,
                                                            CropRect rect);

}  // namespace anvil::images
