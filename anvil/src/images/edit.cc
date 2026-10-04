#include "anvil/images/edit.h"

#include <chrono>
#include <memory>
#include <span>
#include <utility>

#include "anvil/fs/upload.h"
#include "anvil/images/probe.h"
#include "anvil/images/stroke_raster.h"
#include "detail.h"

namespace anvil::images {

#if ANVIL_HAS_VIPS

namespace {

[[nodiscard]] VipsAngle angle_for(std::uint8_t turns) noexcept {
    switch (turns & 3) {
        case 1: return VIPS_ANGLE_D90;
        case 2: return VIPS_ANGLE_D180;
        case 3: return VIPS_ANGLE_D270;
        default: return VIPS_ANGLE_D0;
    }
}

struct GFree final {
    void operator()(void* pointer) const noexcept { g_free(pointer); }
};

// Draws the strokes over `image` and hands back an image wrapping the drawn
// pixels. `pixels` owns the buffer and must outlive every use of the result.
//
// The one place an edit holds the whole picture: the output frame, capped at
// the widest rung, as 8-bit sRGB — 2560² × 3 ≈ 20 MB — plus one stroke's mask
// at a time. It is capped by construction (plan_edit), not by a check here.
[[nodiscard]] Status draw_onto(detail::ImageRef& image, const Recipe& recipe,
                               const EditPlan& plan, std::unique_ptr<void, GFree>& pixels,
                               std::chrono::steady_clock::time_point deadline) {
    // The master is normalised to sRGB at upload, but a 16-bit PNG stays 16-bit
    // and a greyscale one stays one band; the rasteriser speaks 8-bit sRGB with
    // three or four bands and nothing else.
    detail::ImageRef srgb;
    if (vips_colourspace(image.get(), srgb.out(), VIPS_INTERPRETATION_sRGB, nullptr) != 0) {
        return fail(ErrorCode::UnsupportedMedia, kRejectEncode);
    }
    detail::ImageRef bytes;
    if (vips_cast_uchar(srgb.get(), bytes.out(), nullptr) != 0) {
        return fail(ErrorCode::UnsupportedMedia, kRejectEncode);
    }
    const int bands = vips_image_get_bands(bytes.get());
    if (bands != 3 && bands != 4) { return fail(ErrorCode::UnsupportedMedia, kRejectEncode); }

    std::size_t size = 0;
    pixels.reset(vips_image_write_to_memory(bytes.get(), &size));
    if (pixels == nullptr) { return fail(ErrorCode::UnsupportedMedia, kRejectEncode); }

    const auto width = static_cast<std::uint32_t>(vips_image_get_width(bytes.get()));
    const auto height = static_cast<std::uint32_t>(vips_image_get_height(bytes.get()));
    const PixelCanvas canvas{
        std::span<std::uint8_t>{static_cast<std::uint8_t*>(pixels.get()), size}, width, height,
        static_cast<std::uint8_t>(bands)};
    const Status drawn = draw_recipe_strokes(canvas, recipe, plan, deadline);
    if (!drawn) { return drawn; }

    detail::ImageRef wrapped{vips_image_new_from_memory(pixels.get(), size,
                                                        static_cast<int>(width),
                                                        static_cast<int>(height), bands,
                                                        VIPS_FORMAT_UCHAR)};
    if (!wrapped.valid()) { return fail(ErrorCode::UnsupportedMedia, kRejectEncode); }
    detail::ImageRef tagged;
    if (vips_copy(wrapped.get(), tagged.out(), "interpretation", VIPS_INTERPRETATION_sRGB,
                  nullptr) != 0) {
        return fail(ErrorCode::UnsupportedMedia, kRejectEncode);
    }
    image = std::move(tagged);
    return ok();
}

}  // namespace

Result<RenderedEdit> render_edit(const fs::Storage& storage, fs::Ns ns, const Uuid& id,
                                 int source_master_fd, fs::Mime mime, const Recipe& recipe,
                                 const EditPlan& plan) {
    if (!available()) { return fail(ErrorCode::Internal, kRejectUnreadable); }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{kOperationTimeoutSeconds};

    Result<fs::TempSlot> slot = fs::TempSlot::create(storage, id, fs::kMasterVariant);
    if (!slot) { return slot.error(); }
    fs::TempSlot master = std::move(slot).value();

    detail::clear_errors();
    const detail::SourceRef source = detail::source_from_fd(source_master_fd);
    const detail::TargetRef target = detail::target_to_fd(master.fd());
    if (!source.valid() || !target.valid()) {
        return fail(ErrorCode::Internal, kRejectUnreadable);
    }

    // Declared FIRST so it is destroyed last: once strokes are drawn, the image
    // below wraps this buffer without copying it.
    std::unique_ptr<void, GFree> pixels;

    // RANDOM rather than SEQUENTIAL: a quarter turn reads the source in column
    // order, and a sequential source cannot seek backwards to serve it.
    const detail::ImageRef loaded{vips_image_new_from_source(source.get(), "", "access",
                                                             VIPS_ACCESS_RANDOM, nullptr)};
    if (!loaded.valid()) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
    }
    // The kill is armed on the LOADED image, and `loaded` holds its own
    // reference for as long as the deadline does. Drawing replaces the pipeline
    // with an image over plain memory, which references nothing upstream — so
    // without this reference the loaded image is freed mid-function and the
    // deadline disconnects its handler from a dead object.
    const detail::Deadline armed{loaded.get(), kOperationTimeoutSeconds};
    detail::ImageRef image{g_object_ref(loaded.get())};
    const auto failed = [&armed](std::string_view reason) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia, armed.expired() ? kRejectTimeout : reason);
    };

    // Orient: exact, no resampling. Turns first, then the flip — the one order
    // the recipe names, and the one the client's preview transform applies.
    if ((recipe.turns & 3) != 0) {
        detail::ImageRef turned;
        if (vips_rot(image.get(), turned.out(), angle_for(recipe.turns), nullptr) != 0) {
            return failed(kRejectEncode);
        }
        image = std::move(turned);
    }
    if (recipe.flip) {
        detail::ImageRef flipped;
        if (vips_flip(image.get(), flipped.out(), VIPS_DIRECTION_HORIZONTAL, nullptr) != 0) {
            return failed(kRejectEncode);
        }
        image = std::move(flipped);
    }

    if (plan.crop.width != plan.oriented_width || plan.crop.height != plan.oriented_height) {
        detail::ImageRef cropped;
        if (vips_extract_area(image.get(), cropped.out(), static_cast<int>(plan.crop.left),
                              static_cast<int>(plan.crop.top), static_cast<int>(plan.crop.width),
                              static_cast<int>(plan.crop.height), nullptr) != 0) {
            return failed(kRejectEncode);
        }
        image = std::move(cropped);
    }

    if (plan.out_width != plan.crop.width || plan.out_height != plan.crop.height) {
        // Two scales rather than one, so each axis lands on exactly the pixel
        // count plan_edit computed — the count the client was told.
        const double hscale = static_cast<double>(plan.out_width) / plan.crop.width;
        const double vscale = static_cast<double>(plan.out_height) / plan.crop.height;
        detail::ImageRef resized;
        if (vips_resize(image.get(), resized.out(), hscale, "vscale", vscale, nullptr) != 0) {
            return failed(kRejectEncode);
        }
        image = std::move(resized);
    }
    if (static_cast<std::uint32_t>(vips_image_get_width(image.get())) != plan.out_width ||
        static_cast<std::uint32_t>(vips_image_get_height(image.get())) != plan.out_height) {
        // Drawing onto a frame one pixel off the plan would put every stroke a
        // fraction of a pixel away from where the preview put it. Refused rather
        // than tolerated, because nothing downstream would ever notice.
        return failed(kRejectEncode);
    }

    // Drawing is last: orient → draw → crop → resize is the order the recipe
    // describes, and drawing at the output is the same picture for bounded work.
    if (!recipe.strokes.empty()) {
        const Status drawn = draw_onto(image, recipe, plan, pixels, deadline);
        if (!drawn) {
            detail::clear_errors();
            return drawn.error();
        }
    }

    if (detail::save_master(image.get(), target.get(), mime) != 0) {
        return failed(kRejectEncode);
    }
    const std::uint64_t master_bytes = master.size();
    if (master_bytes == 0) { return fail(ErrorCode::Internal, kRejectEncode); }
    const Status published = master.publish(ns);
    if (!published) { return published.error(); }

    // Reopened through the descriptor walk every read uses, and the variants
    // derived from the EDITED master exactly as an upload's are from its own.
    const fs::Fd written = storage.open_media(ns, id, fs::kMasterVariant);
    if (!written.valid()) { return fail(ErrorCode::Internal); }
    Result<std::vector<VariantRecord>> variants = generate_variants(
        storage, ns, id, written.get(), ImageInfo{plan.out_width, plan.out_height, 1});
    if (!variants) { return variants.error(); }

    return RenderedEdit{std::move(variants).value(), master_bytes, plan.out_width,
                        plan.out_height};
}

#else   // ANVIL_HAS_VIPS

Result<RenderedEdit> render_edit(const fs::Storage&, fs::Ns, const Uuid&, int, fs::Mime,
                                 const Recipe&, const EditPlan&) {
    return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
}

#endif  // ANVIL_HAS_VIPS

}  // namespace anvil::images
