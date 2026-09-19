#include "anvil/images/variants.h"

#include <utility>

#include <unistd.h>

#include "anvil/fs/upload.h"
#include "detail.h"

namespace anvil::images {

#if ANVIL_HAS_VIPS

namespace {

// Tuned by measurement rather than by taste (docs/08-images.md §3). AVIF is roughly 30%
// smaller than WebP at equal perceptual quality, which is why it is preferred
// and why its Q is lower for the same result.
//
// Both numbers were raised after the shipped photographs turned out to be
// visibly blocky in flat, saturated areas — the orange wall behind the mural is
// the worst case. Measured at the 1600 rung against a lossless reference, Q50
// carried three times the error of Q68 while spending barely half the byte
// budget of the WebP it is supposed to be cheaper than: it was undershooting,
// not economising.
constexpr int kAvifQuality = 68;
constexpr int kWebpQuality = 85;

// Chroma is the reason for the artefact this fixes. Both savers subsample by
// default, and 4:2:0 quarters the resolution of exactly the red and orange
// edges this photography is full of. Held at full resolution in both formats:
// "off" for AVIF, and WebP's own high-quality path, which is what it calls the
// same decision.
//
// WebP's effort is worth 4.5% of the file for 0.4s. AVIF's is NOT: effort 6
// costs 2.5x the encode time for 0.7% of the file, and there are ten variants
// per upload sharing one cpu_pool and a 20-second per-operation deadline. It is
// left at the saver's default deliberately, and this comment is why.
constexpr int kWebpEffort = 6;

// One saver call per format rather than a shared one parameterised by suffix:
// the two take different option NAMES for the same decisions, and a shared call
// could only express the settings both happen to share. libvips ignores an
// unknown option silently, so a merged call is a setting that looks applied and
// is not.
[[nodiscard]] int save_variant(VipsImage* image, VipsTarget* target, fs::Format format) {
    if (format == fs::Format::Avif) {
        return vips_image_write_to_target(image, ".avif", target, "Q", kAvifQuality,
#if ANVIL_VIPS_HAS_HEIF_SUBSAMPLE
                                          "subsample_mode", VIPS_FOREIGN_SUBSAMPLE_OFF,
#endif
                                          ANVIL_VIPS_KEEP_NOTHING, nullptr);
    }
    return vips_image_write_to_target(image, ".webp", target, "Q", kWebpQuality,
                                      "smart_subsample", TRUE, "effort", kWebpEffort,
                                      ANVIL_VIPS_KEEP_NOTHING, nullptr);
}

[[nodiscard]] bool encoder_available(fs::Format format) noexcept {
    return format == fs::Format::Avif ? can_encode_avif() : true;
}

// One variant, written into a temp slot and published by rename. The slot is
// discarded on every failure path by its own destructor, so a failed encode
// never leaves a zero-byte file where a reader could find it.
[[nodiscard]] Result<VariantRecord> render_one(const fs::Storage& storage, fs::Ns ns,
                                               const Uuid& id, int master_fd,
                                               fs::VariantKey key) {
    Result<fs::TempSlot> slot = fs::TempSlot::create(storage, id, key);
    if (!slot) { return slot.error(); }
    fs::TempSlot output = std::move(slot).value();

    detail::clear_errors();
    const detail::SourceRef source = detail::source_from_fd(master_fd);
    const detail::TargetRef target = detail::target_to_fd(output.fd());
    if (!source.valid() || !target.valid()) {
        return fail(ErrorCode::Internal, kRejectUnreadable);
    }

    // VIPS_SIZE_DOWN is the no-upscale rule expressed to the resizer rather than
    // enforced afterwards: a master narrower than the target comes back at its
    // own width, and the caller has already declined to ask for it.
    detail::ImageRef thumbnail;
    if (vips_thumbnail_source(source.get(), thumbnail.out(), static_cast<int>(key.width), "size",
                              VIPS_SIZE_DOWN, nullptr) != 0) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia, kRejectEncode);
    }

    const detail::Deadline deadline{thumbnail.get(), kOperationTimeoutSeconds};

    if (save_variant(thumbnail.get(), target.get(), key.format) != 0) {
        detail::clear_errors();
        return fail(ErrorCode::UnsupportedMedia,
                    deadline.expired() ? kRejectTimeout : kRejectEncode);
    }

    const std::uint64_t bytes = output.size();
    const int written_height = vips_image_get_height(thumbnail.get());

    const Status published = output.publish(ns);
    if (!published) { return published.error(); }

    // The recorded width is the KEY's width, not the encoder's output: the
    // filename is built from the key, and a client that builds a URL from a
    // width that disagrees with the filename gets a 404. The caller has already
    // excluded every width the master cannot fill, so the two agree — recording
    // the key is what keeps them agreeing if that ever changes.
    //
    // The byte size is stored so clients can choose informedly and so a
    // regression in encoder settings shows up in metrics rather than only in a
    // bandwidth bill.
    return VariantRecord{static_cast<std::uint32_t>(bytes), key.width,
                         static_cast<std::uint16_t>(written_height), key.format};
}

}  // namespace

Result<std::vector<VariantRecord>> generate_variants(const fs::Storage& storage, fs::Ns ns,
                                                     const Uuid& id, int master_fd,
                                                     ImageInfo master) {
    if (!available()) { return fail(ErrorCode::Internal, kRejectUnreadable); }

    std::vector<VariantRecord> produced;
    produced.reserve(fs::kVariantWidths.size() * fs::kVariantFormats.size());

    for (const std::uint16_t width : fs::kVariantWidths) {
        // No upscaling: a master narrower than this width has nothing to give.
        if (width > master.width) { continue; }
        for (const fs::Format format : fs::kVariantFormats) {
            if (!encoder_available(format)) { continue; }

            const Result<VariantRecord> rendered =
                render_one(storage, ns, id, master_fd, fs::VariantKey{width, format});
            if (!rendered) {
                // Everything already written is removed: a partial set recorded
                // against a media row would serve some widths and 404 others.
                unlink_variants(storage, ns, id, produced);
                return rendered.error();
            }
            produced.push_back(rendered.value());
        }
    }

    return produced;
}

#else   // ANVIL_HAS_VIPS

Result<std::vector<VariantRecord>> generate_variants(const fs::Storage&, fs::Ns, const Uuid&, int,
                                                     ImageInfo) {
    return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
}

#endif  // ANVIL_HAS_VIPS

void unlink_variants(const fs::Storage& storage, fs::Ns ns, const Uuid& id,
                     const std::vector<VariantRecord>& variants) noexcept {
    for (const VariantRecord& variant : variants) {
        const fs::VariantKey key{variant.width, variant.format};
        // Opened through the same descriptor walk every read uses, then unlinked
        // relative to the shard: no path string is ever handed to a syscall.
        const Result<fs::Fd> shard = storage.open_shard_for_write(ns, id);
        if (!shard) { continue; }
        const fs::RelPath path = fs::media_relative_path(ns, id, key);
        const std::string_view view = path.view();
        const std::string_view leaf = view.substr(view.rfind('/') + 1);
        std::array<char, 48> name{};
        if (leaf.size() >= name.size()) { continue; }
        for (std::size_t i = 0; i < leaf.size(); ++i) { name[i] = leaf[i]; }
        (void)::unlinkat(shard.value().get(), name.data(), 0);
    }
}

}  // namespace anvil::images
