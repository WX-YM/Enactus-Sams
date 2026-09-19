#pragma once

// Header probing and the decode limits (docs/08-images.md §1).
//
// A 40 KB PNG can decode to 50 000 x 50 000 pixels. At 4 bytes per pixel that is
// 10 GB of RAM, allocated inside a C decoder before any of this code runs, and
// it is the single most likely way a service built on this library dies in
// production. Every upload is therefore measured from its HEADER — dimensions, page
// count, and the ratio between encoded and decoded size — and rejected before a
// decoder is ever handed the pixels.
//
// The caps are deliberately generous for real photography: a 6000x4000 master
// is 24 Mpx and passes comfortably.
//
// Nothing in this file, or in strip/variants/crop, may run on a Trantor
// event-loop thread. Every entry point here blocks (docs/08-images.md §5).

#include <cstdint>
#include <string_view>

#include "anvil/core/result.h"
#include "anvil/core/types.h"

namespace anvil::images {

// --- limits ---------------------------------------------------------------

inline constexpr std::uint32_t kMaxDimension = 12000;
inline constexpr std::uint64_t kMaxPixels = 50'000'000;
inline constexpr std::uint64_t kMaxBytes = 25ULL * 1024 * 1024;
// Multi-frame images are rejected: a frame count multiplies every cost, and a
// 1000-frame WebP passes a per-frame dimension check untouched (docs/08-images.md §1).
inline constexpr std::uint32_t kMaxPages = 1;
// A file whose decoded size exceeds its encoded size by more than this is a
// bomb, not a photograph. Real JPEGs land between 5x and 50x.
inline constexpr std::uint64_t kMaxCompressionRatio = 1000;
// Per-operation wall clock. A decoder that has not finished by now is either
// pathological input or a stuck delegate; either way the task is killed and the
// upload answered with UNSUPPORTED_MEDIA rather than holding a cpu_pool thread.
inline constexpr std::uint32_t kOperationTimeoutSeconds = 20;

// --- rejection reasons -----------------------------------------------------
//
// Carried in Failure::field so the audit row records WHY, while the client sees
// only UNSUPPORTED_MEDIA. Compile-time names, never submitted bytes.

inline constexpr std::string_view kRejectUnreadable = "image.unreadable";
inline constexpr std::string_view kRejectDimension = "image.dimension";
inline constexpr std::string_view kRejectPixels = "image.pixels";
inline constexpr std::string_view kRejectBytes = "image.bytes";
inline constexpr std::string_view kRejectFrames = "image.frames";
inline constexpr std::string_view kRejectRatio = "image.ratio";
inline constexpr std::string_view kRejectEncode = "image.encode";
inline constexpr std::string_view kRejectTimeout = "image.timeout";
inline constexpr std::string_view kRejectUnsupportedOutput = "image.no_encoder";

// --- values ----------------------------------------------------------------

// 12 bytes, trivially copyable, crosses a pool boundary by value.
struct ImageInfo final {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pages;
};

// --- runtime ---------------------------------------------------------------

// libvips must be initialised ONCE, from the main thread, before any pool
// starts — the same static-initialisation constraint as mongocxx::instance
// (docs/08-images.md §5). Throws on failure, because an image subsystem that did not
// initialise is a boot failure rather than a degraded mode.
void init(std::string_view program_name);
void shutdown() noexcept;
[[nodiscard]] bool available() noexcept;

// True when this build can encode `format`. AVIF needs libheif with an AV1
// encoder, which is a delegate rather than part of libvips itself; asking first
// is what stops a missing delegate from producing a zero-byte variant.
[[nodiscard]] bool can_encode_avif() noexcept;

// --- probing ---------------------------------------------------------------

// Reads the header only — no pixels are decoded. `file_bytes` is the encoded
// size, needed for the compression-ratio check; `fd` is borrowed and its file
// offset is not preserved.
[[nodiscard]] Result<ImageInfo> probe(int fd, std::uint64_t file_bytes);

}  // namespace anvil::images
