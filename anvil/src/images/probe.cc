#include "anvil/images/probe.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string>

#include "detail.h"

#if ANVIL_HAS_VIPS
#include <unistd.h>
#endif

namespace anvil::images {
namespace {

std::atomic<bool> g_initialised{false};

}  // namespace

#if ANVIL_HAS_VIPS

namespace detail {

SourceRef source_from_fd(int fd) noexcept {
    // Duplicated: VipsSource seeks and consumes the descriptor's offset, and the
    // caller reuses the same fd for the next variant. dup() gives an independent
    // offset over the same open file description's inode.
    const int copy = ::dup(fd);
    if (copy < 0) { return SourceRef{}; }
    VipsSource* source = vips_source_new_from_descriptor(copy);
    if (source == nullptr) {
        ::close(copy);
        return SourceRef{};
    }
    return SourceRef{source};
}

TargetRef target_to_fd(int fd) noexcept {
    const int copy = ::dup(fd);
    if (copy < 0) { return TargetRef{}; }
    VipsTarget* target = vips_target_new_to_descriptor(copy);
    if (target == nullptr) {
        ::close(copy);
        return TargetRef{};
    }
    return TargetRef{target};
}

void clear_errors() noexcept { vips_error_clear(); }

void Deadline::on_eval(VipsImage* image, VipsProgress* progress, void* user_data) noexcept {
    auto* self = static_cast<Deadline*>(user_data);
    if (self == nullptr || progress == nullptr) { return; }
    if (progress->run >= self->elapsed_limit_) {
        self->expired_ = true;
        // Checked between scanlines by every libvips operation, so the pipeline
        // unwinds at the next block boundary with an ordinary error.
        vips_image_set_kill(image, TRUE);
    }
}

Deadline::Deadline(VipsImage* image, std::uint32_t seconds) noexcept
    : image_{image}, elapsed_limit_{static_cast<int>(seconds)}, expired_{false} {
    if (image_ == nullptr) { return; }
    vips_image_set_progress(image_, TRUE);
    g_signal_connect(image_, "eval", G_CALLBACK(&Deadline::on_eval), this);
}

Deadline::~Deadline() noexcept {
    if (image_ == nullptr) { return; }
    // The image may outlive this object — a caller can keep a reference — so the
    // handler must go before the pointer it captures does.
    g_signal_handlers_disconnect_by_data(image_, this);
}

}  // namespace detail

void init(std::string_view program_name) {
    if (g_initialised.exchange(true)) { return; }

    const std::string name{program_name};
    if (VIPS_INIT(name.c_str()) != 0) {
        g_initialised.store(false);
        throw std::runtime_error{"libvips failed to initialise"};
    }

    // libvips parallelises internally by default. N pool threads each spawning M
    // workers oversubscribes the CPU badly, and the pool size is already the
    // concurrency decision.
    vips_concurrency_set(1);

    // The operation cache holds DECODED images. Left at its default it grows
    // into the RSS budget and undoes the per-task memory reasoning entirely
    // (docs/08-images.md §5), so it is disabled rather than tuned.
    vips_cache_set_max(0);
    vips_cache_set_max_mem(0);
    vips_cache_set_max_files(0);
}

void shutdown() noexcept {
    if (!g_initialised.exchange(false)) { return; }
    vips_shutdown();
}

bool available() noexcept { return g_initialised.load(); }

bool can_encode_avif() noexcept {
    if (!g_initialised.load()) { return false; }
    // AVIF encoding is libheif plus an AV1 encoder, both delegates. Asking
    // libvips rather than assuming is what stops a missing delegate from
    // producing a zero-byte variant on every upload.
    return vips_foreign_find_save_target(".avif") != nullptr;
}

Result<ImageInfo> probe(int fd, std::uint64_t file_bytes) {
    if (!g_initialised.load()) { return fail(ErrorCode::Internal, kRejectUnreadable); }
    if (file_bytes == 0 || file_bytes > kMaxBytes) {
        return fail(ErrorCode::PayloadTooLarge, kRejectBytes);
    }

    detail::clear_errors();
    const detail::SourceRef source = detail::source_from_fd(fd);
    if (!source.valid()) { return fail(ErrorCode::Internal, kRejectUnreadable); }

    // Header only. libvips is demand-driven: opening an image parses its header
    // and nothing else, so a 50 000^2 declaration is visible here at a cost of
    // one read (docs/08-images.md §1 step 1).
    //
    // `n = -1` asks for EVERY page. It decodes nothing extra — the pipeline is
    // still lazy — but it makes the height the full strip, which is how a
    // multi-frame file is detected even when its loader does not publish an
    // `n-pages` field (libvips' WebP loader does not; its GIF loader does).
    // Without it an animation looks like a single frame and its cost
    // multiplication is invisible here.
    detail::ImageRef image{vips_image_new_from_source(source.get(), "", "access",
                                                      VIPS_ACCESS_SEQUENTIAL, "n", -1, nullptr)};
    if (!image.valid()) {
        // `n` exists only on the multi-page loaders; the PNG and JPEG loaders
        // reject it as an unknown argument. A single-page format cannot BE an
        // animation, so falling back loses nothing — but the fallback must be a
        // second open rather than a first one, or the page check silently stops
        // applying to the formats that need it.
        detail::clear_errors();
        const detail::SourceRef retry = detail::source_from_fd(fd);
        if (!retry.valid()) { return fail(ErrorCode::Internal, kRejectUnreadable); }
        image = detail::ImageRef{vips_image_new_from_source(retry.get(), "", "access",
                                                            VIPS_ACCESS_SEQUENTIAL, nullptr)};
        if (!image.valid()) {
            detail::clear_errors();
            return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
        }
    }

    const int width = vips_image_get_width(image.get());
    const int height = vips_image_get_height(image.get());
    const int bands = vips_image_get_bands(image.get());
    if (width <= 0 || height <= 0 || bands <= 0) {
        return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
    }

    // Two independent signals, because neither is universal: the declared page
    // count, and the geometry — with n = -1 the height is pages x page_height,
    // so a loader that publishes no page count still gives itself away. The
    // larger wins; a frame that is invisible to both is one this build cannot
    // decode either.
    const int declared_pages = vips_image_get_n_pages(image.get());
    const int page_height = vips_image_get_page_height(image.get());
    const std::uint32_t geometric_pages =
        page_height > 0 ? static_cast<std::uint32_t>(height / page_height) : 1U;
    const std::uint32_t page_count =
        std::max<std::uint32_t>(declared_pages > 0 ? static_cast<std::uint32_t>(declared_pages) : 1U,
                                geometric_pages == 0 ? 1U : geometric_pages);

    // Rejected before the dimension checks, so an animation is reported as an
    // animation rather than as an image that happens to be very tall.
    if (page_count > kMaxPages) { return fail(ErrorCode::UnsupportedMedia, kRejectFrames); }

    if (static_cast<std::uint32_t>(width) > kMaxDimension ||
        static_cast<std::uint32_t>(height) > kMaxDimension) {
        return fail(ErrorCode::UnsupportedMedia, kRejectDimension);
    }

    const std::uint64_t pixels =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixels > kMaxPixels) { return fail(ErrorCode::UnsupportedMedia, kRejectPixels); }

    // Multiplied out in 64 bits and compared as a ratio: the intermediate
    // pixels * bands * pages cannot overflow because every factor is already
    // bounded above.
    const std::uint64_t decoded = pixels * static_cast<std::uint64_t>(bands) * page_count;
    if (decoded / file_bytes > kMaxCompressionRatio) {
        return fail(ErrorCode::UnsupportedMedia, kRejectRatio);
    }

    return ImageInfo{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                     page_count};
}

#else   // ANVIL_HAS_VIPS

// Built without the image subsystem (-DANVIL_WITH_VIPS=OFF). Every entry
// point refuses rather than silently accepting an unprocessed upload: a build
// that cannot strip metadata must not be a build that stores photographs with
// GPS coordinates in them.

void init(std::string_view) { g_initialised.store(false); }
void shutdown() noexcept {}
bool available() noexcept { return false; }
bool can_encode_avif() noexcept { return false; }

Result<ImageInfo> probe(int, std::uint64_t) {
    return fail(ErrorCode::UnsupportedMedia, kRejectUnreadable);
}

#endif  // ANVIL_HAS_VIPS

}  // namespace anvil::images
