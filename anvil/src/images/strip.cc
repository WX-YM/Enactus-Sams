#include "anvil/images/strip.h"

#include "detail.h"

namespace anvil::images {

#if ANVIL_HAS_VIPS

namespace {

// Master quality. High enough that the normalisation re-encode is not visible
// next to the original, low enough that the master is not larger than what was
// uploaded. Variants are generated from this file, never from each other, so
// this is the only lossy step they inherit.
//
// Which is exactly why these sit ABOVE the variant qualities in variants.cc: the
// master is the ceiling on every variant derived from it, and a master encoded
// no better than its own output spends the whole quality budget twice. They were
// raised alongside the variant settings for that reason.
constexpr int kMasterJpegQuality = 94;
constexpr int kMasterWebpQuality = 94;
constexpr int kMasterAvifQuality = 80;

[[nodiscard]] const char* container_suffix(fs::Mime mime) noexcept {
    switch (mime) {
        case fs::Mime::Jpeg: return ".jpg";
        case fs::Mime::Png:  return ".png";
        case fs::Mime::Webp: return ".webp";
        case fs::Mime::Avif: return ".avif";
        case fs::Mime::Unknown: break;
    }
    return nullptr;
}

// One saver call per container. The four take different option NAMES for the
// same two decisions — quality and chroma — and libvips ignores an unknown
// option rather than refusing it, so a call merged across formats is a setting
// that reads as applied and is not.
//
// Chroma is held at full resolution everywhere it can be. jpegsave's default is
// `auto`, which subsamples below Q90 and not at or above it, so the old Q90
// master was 4:4:4 only by standing exactly on the threshold — one point lower
// would have silently halved the colour resolution of every variant derived
// from it. Said explicitly, it is a decision rather than a coincidence.
//
// PNG is lossless: it takes neither option, which is the split the previous
// quality-or-not branch already drew.
[[nodiscard]] int save_master(VipsImage* image, VipsTarget* target, fs::Mime mime) {
    switch (mime) {
        case fs::Mime::Jpeg:
            return vips_image_write_to_target(image, ".jpg", target, "Q", kMasterJpegQuality,
                                              "subsample_mode", VIPS_FOREIGN_SUBSAMPLE_OFF,
                                              ANVIL_VIPS_KEEP_NOTHING, nullptr);
        case fs::Mime::Webp:
            return vips_image_write_to_target(image, ".webp", target, "Q", kMasterWebpQuality,
                                              "smart_subsample", TRUE,
                                              ANVIL_VIPS_KEEP_NOTHING, nullptr);
        case fs::Mime::Avif:
            return vips_image_write_to_target(image, ".avif", target, "Q", kMasterAvifQuality,
#if ANVIL_VIPS_HAS_HEIF_SUBSAMPLE
                                              "subsample_mode", VIPS_FOREIGN_SUBSAMPLE_OFF,
#endif
                                              ANVIL_VIPS_KEEP_NOTHING, nullptr);
        case fs::Mime::Png:
            return vips_image_write_to_target(image, ".png", target, ANVIL_VIPS_KEEP_NOTHING,
                                              nullptr);
        case fs::Mime::Unknown: break;
    }
    // Unreachable: the caller rejects an unsupported container before it gets
    // here. Reported as a failed write rather than defaulted to some container,
    // because guessing one would write a file whose bytes disagree with the
    // extension every reader will resolve it by.
    return -1;
}

}  // namespace

Result<ImageInfo> normalise_master(int source_fd, fs::Mime mime, int output_fd) {
    if (!available()) { return fail(ErrorCode::Internal, kRejectUnreadable); }

    const char* suffix = container_suffix(mime);
    if (suffix == nullptr) { return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable); }

    detail::clear_errors();
    const detail::SourceRef source = detail::source_from_fd(source_fd);
    const detail::TargetRef target = detail::target_to_fd(output_fd);
    if (!source.valid() || !target.valid()) {
        return fail(ErrorCode::Internal, kRejectUnreadable);
    }

    // Sequential access: the pipeline is demand-driven and the whole image is
    // never resident (docs/08-images.md §6). Never load into a std::string or a vector.
    detail::ImageRef loaded{vips_image_new_from_source(source.get(), "", "access",
                                                       VIPS_ACCESS_SEQUENTIAL, nullptr)};
    if (!loaded.valid()) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
    }

    const detail::Deadline deadline{loaded.get(), kOperationTimeoutSeconds};

    // Applies the EXIF orientation to the PIXELS and removes the tag. Without
    // this, stripping metadata renders every phone portrait sideways.
    detail::ImageRef rotated;
    if (vips_autorot(loaded.get(), rotated.out(), nullptr) != 0) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia,
                    deadline.expired() ? kRejectTimeout : kRejectUnreadable);
    }

    // An embedded profile is honoured and then discarded; an image without one
    // is merely tagged sRGB, which is what an untagged image already means in
    // practice. Either way the output carries no profile and needs none.
    detail::ImageRef converted;
    const bool has_profile =
        vips_image_get_typeof(rotated.get(), VIPS_META_ICC_NAME) != 0;
    const int colour_status =
        has_profile
            ? vips_icc_transform(rotated.get(), converted.out(), "srgb", "embedded", TRUE,
                                 "intent", VIPS_INTENT_PERCEPTUAL, nullptr)
            : vips_colourspace(rotated.get(), converted.out(), VIPS_INTERPRETATION_sRGB,
                               nullptr);
    if (colour_status != 0) {
        // A broken or unsupported profile must not fail the upload: fall back to
        // interpreting the pixels as sRGB, which is what a viewer would do.
        detail::clear_errors();
        converted.reset();
        if (vips_colourspace(rotated.get(), converted.out(), VIPS_INTERPRETATION_sRGB,
                             nullptr) != 0) {
            detail::clear_errors();
            return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
        }
    }

    const int written = save_master(converted.get(), target.get(), mime);
    if (written != 0) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia,
                    deadline.expired() ? kRejectTimeout : kRejectEncode);
    }

    // Dimensions AFTER rotation: a portrait shot on a phone is 3024x4032 once
    // the orientation is applied, and every variant width is computed from this.
    return ImageInfo{static_cast<std::uint32_t>(vips_image_get_width(converted.get())),
                     static_cast<std::uint32_t>(vips_image_get_height(converted.get())), 1U};
}

#else   // ANVIL_HAS_VIPS

Result<ImageInfo> normalise_master(int, fs::Mime, int) {
    return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
}

#endif  // ANVIL_HAS_VIPS

}  // namespace anvil::images
