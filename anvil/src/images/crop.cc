#include "anvil/images/crop.h"

#include <cmath>
#include <utility>

#include <unistd.h>

#include "anvil/fs/upload.h"
#include "detail.h"

namespace anvil::images {
namespace {

// Pixel geometry from a normalised rectangle. Rounded rather than truncated so
// a rectangle that reaches the right edge is not silently one pixel short.
struct PixelRect final {
    int left;
    int top;
    int width;
    int height;
};

[[nodiscard]] PixelRect to_pixels(CropRect rect, ImageInfo master) noexcept {
    const auto scale = [](float value, std::uint32_t extent) {
        return static_cast<int>(std::lround(static_cast<double>(value) * extent));
    };
    PixelRect out{scale(rect.x, master.width), scale(rect.y, master.height),
                  scale(rect.width, master.width), scale(rect.height, master.height)};
    // Clamped to the image after rounding: rounding up on both the offset and
    // the extent can put the far edge one pixel outside, which libvips reports
    // as an error rather than clipping.
    if (out.left + out.width > static_cast<int>(master.width)) {
        out.width = static_cast<int>(master.width) - out.left;
    }
    if (out.top + out.height > static_cast<int>(master.height)) {
        out.height = static_cast<int>(master.height) - out.top;
    }
    return out;
}

}  // namespace

Status validate_crop(CropRect rect, ImageInfo master) noexcept {
    // NaN fails every comparison, so it is excluded by requiring the positive
    // cases rather than by rejecting the negative ones.
    const bool finite = std::isfinite(rect.x) && std::isfinite(rect.y) &&
                        std::isfinite(rect.width) && std::isfinite(rect.height);
    if (!finite) { return fail(ErrorCode::ValidationFailed, kRejectCropBounds); }

    if (rect.x < 0.0F || rect.y < 0.0F || rect.width <= 0.0F || rect.height <= 0.0F) {
        return fail(ErrorCode::ValidationFailed, kRejectCropBounds);
    }
    if (rect.x + rect.width > 1.0F || rect.y + rect.height > 1.0F) {
        return fail(ErrorCode::ValidationFailed, kRejectCropBounds);
    }

    const PixelRect pixels = to_pixels(rect, master);
    if (pixels.width <= 0 || pixels.height <= 0) {
        return fail(ErrorCode::ValidationFailed, kRejectCropBounds);
    }
    // Smaller than the narrowest variant means every derived size would be an
    // upscale, which is a worse picture than the uncropped one.
    if (pixels.width < static_cast<int>(fs::kVariantWidths.front())) {
        return fail(ErrorCode::ValidationFailed, kRejectCropTooSmall);
    }
    return ok();
}

#if ANVIL_HAS_VIPS

namespace {

constexpr int kAvifQuality = 50;
constexpr int kWebpQuality = 78;

[[nodiscard]] const char* suffix_for(fs::Format format) noexcept {
    return format == fs::Format::Avif ? ".avif" : ".webp";
}

[[nodiscard]] int quality_for(fs::Format format) noexcept {
    return format == fs::Format::Avif ? kAvifQuality : kWebpQuality;
}

[[nodiscard]] Result<VariantRecord> render_cropped(const fs::Storage& storage, fs::Ns ns,
                                                   const Uuid& id, int master_fd,
                                                   PixelRect area, fs::VariantKey key) {
    Result<fs::TempSlot> slot = fs::TempSlot::create(storage, id, key);
    if (!slot) { return slot.error(); }
    fs::TempSlot output = std::move(slot).value();

    detail::clear_errors();
    const detail::SourceRef source = detail::source_from_fd(master_fd);
    const detail::TargetRef target = detail::target_to_fd(output.fd());
    if (!source.valid() || !target.valid()) {
        return fail(ErrorCode::Internal, kRejectUnreadable);
    }

    // RANDOM rather than SEQUENTIAL: extract_area reads a window, and a
    // sequential source cannot seek backwards to serve it.
    detail::ImageRef loaded{vips_image_new_from_source(source.get(), "", "access",
                                                       VIPS_ACCESS_RANDOM, nullptr)};
    if (!loaded.valid()) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
    }

    const detail::Deadline deadline{loaded.get(), kOperationTimeoutSeconds};

    detail::ImageRef cropped;
    if (vips_extract_area(loaded.get(), cropped.out(), area.left, area.top, area.width,
                          area.height, nullptr) != 0) {
        detail::clear_errors();
        return fail(ErrorCode::ValidationFailed, kRejectCropBounds);
    }

    detail::ImageRef resized;
    const double scale =
        static_cast<double>(key.width) / static_cast<double>(area.width);
    if (vips_resize(cropped.get(), resized.out(), scale, nullptr) != 0) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia,
                    deadline.expired() ? kRejectTimeout : kRejectEncode);
    }

    if (vips_image_write_to_target(resized.get(), suffix_for(key.format), target.get(), "Q",
                                   quality_for(key.format), ANVIL_VIPS_KEEP_NOTHING,
                                   nullptr) != 0) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia,
                    deadline.expired() ? kRejectTimeout : kRejectEncode);
    }

    const std::uint64_t bytes = output.size();
    const int height = vips_image_get_height(resized.get());

    const Status published = output.publish(ns);
    if (!published) { return published.error(); }

    return VariantRecord{static_cast<std::uint32_t>(bytes), key.width,
                         static_cast<std::uint16_t>(height), key.format};
}

}  // namespace

Result<std::vector<VariantRecord>> apply_crop(const fs::Storage& storage, fs::Ns ns,
                                              const Uuid& id, int master_fd, ImageInfo master,
                                              CropRect rect) {
    if (!available()) { return fail(ErrorCode::Internal, kRejectUnreadable); }

    const Status valid = validate_crop(rect, master);
    if (!valid) { return valid.error(); }

    const PixelRect area = to_pixels(rect, master);

    std::vector<VariantRecord> produced;
    produced.reserve(fs::kVariantWidths.size() * fs::kVariantFormats.size());

    for (const std::uint16_t width : fs::kVariantWidths) {
        // The crop, not the master, is what the variants are derived from now,
        // so the no-upscale rule applies to the cropped extent.
        if (static_cast<int>(width) > area.width) { continue; }
        for (const fs::Format format : fs::kVariantFormats) {
            if (format == fs::Format::Avif && !can_encode_avif()) { continue; }

            const Result<VariantRecord> rendered =
                render_cropped(storage, ns, id, master_fd, area, fs::VariantKey{width, format});
            if (!rendered) {
                unlink_variants(storage, ns, id, produced);
                return rendered.error();
            }
            produced.push_back(rendered.value());
        }
    }

    return produced;
}

#else   // ANVIL_HAS_VIPS

Result<std::vector<VariantRecord>> apply_crop(const fs::Storage&, fs::Ns, const Uuid&, int,
                                              ImageInfo, CropRect) {
    return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
}

#endif  // ANVIL_HAS_VIPS

}  // namespace anvil::images
