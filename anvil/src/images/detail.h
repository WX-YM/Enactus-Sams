#pragma once

// PRIVATE to src/images. Deliberately not under include/anvil: this is the only
// place <vips/vips.h> is included, which keeps glib, gobject and the decoder
// headers out of every consumer of the library.

// libvips plumbing shared by probe/strip/variants/crop.
//
// PRIVATE to lib/images: it is the only place <vips/vips.h> is included, so
// glib, gobject and the decoder headers stay out of every controller and
// repository that merely wants a variant list. Nothing outside this directory
// may include it.
//
// Two rules run through everything here:
//
//   * Every libvips call's return code is checked. libvips reports errors
//     through its own error stack rather than by throwing, and an unchecked
//     failure yields a zero-byte output that gets served as a broken image
//     (docs/08-images.md §5).
//   * Every object is owned by an RAII type. The operations below have many
//     early returns, and a g_object_unref missed on one of them is a leak that
//     accumulates for exactly as long as the process runs.

#include <cstdint>

#if ANVIL_HAS_VIPS

#include <vips/vips.h>

#include <chrono>

#include "anvil/fs/sniff.h"

namespace anvil::images::detail {

// One resource, one owner, released on every path.
template <typename T>
class GObjectRef final {
public:
    GObjectRef() noexcept : ptr_{nullptr} {}
    explicit GObjectRef(T* ptr) noexcept : ptr_{ptr} {}
    ~GObjectRef() noexcept { reset(); }

    GObjectRef(GObjectRef&& other) noexcept : ptr_{other.ptr_} { other.ptr_ = nullptr; }
    GObjectRef& operator=(GObjectRef&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }
    GObjectRef(const GObjectRef&) = delete;
    GObjectRef& operator=(const GObjectRef&) = delete;

    [[nodiscard]] T* get() const noexcept { return ptr_; }
    [[nodiscard]] bool valid() const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] T** out() noexcept { return &ptr_; }
    void reset() noexcept {
        if (ptr_ != nullptr) {
            g_object_unref(ptr_);
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_;
};

using ImageRef = GObjectRef<VipsImage>;
using SourceRef = GObjectRef<VipsSource>;
using TargetRef = GObjectRef<VipsTarget>;

// A source over a DUPLICATED descriptor: libvips takes ownership of the
// descriptor's offset, and the caller's fd is reused for the next variant.
[[nodiscard]] SourceRef source_from_fd(int fd) noexcept;
[[nodiscard]] TargetRef target_to_fd(int fd) noexcept;

// Clears the libvips error buffer. It is global and append-only, so an
// unrelated later failure otherwise reports the first error this process ever
// saw (docs/08-images.md §5).
void clear_errors() noexcept;

// Arms a wall-clock kill on `image`. libvips checks the kill flag between
// scanlines, so a pathological decode stops at the next block boundary instead
// of holding a cpu_pool thread indefinitely. The returned object must outlive
// every operation on that image.
class Deadline final {
public:
    Deadline(VipsImage* image, std::uint32_t seconds) noexcept;
    ~Deadline() noexcept;

    [[nodiscard]] bool expired() const noexcept { return expired_; }

    Deadline(const Deadline&) = delete;
    Deadline& operator=(const Deadline&) = delete;

private:
    static void on_eval(VipsImage* image, VipsProgress* progress, void* user_data) noexcept;

    VipsImage* image_;
    int        elapsed_limit_;
    bool       expired_;
};

// Metadata policy in one place (docs/08-images.md §2): drop EXIF, XMP, IPTC, Photoshop
// resources, MakerNote and the embedded thumbnail; the ICC profile is handled
// by converting to sRGB first and then dropping it too, so output is
// unambiguous and every variant is consistent.
//
// libvips renamed the save option from "strip" to "keep" in 8.15; both spellings
// exist in 8.15+, only one of them before it.
#if VIPS_MAJOR_VERSION > 8 || (VIPS_MAJOR_VERSION == 8 && VIPS_MINOR_VERSION >= 15)
#define ANVIL_VIPS_KEEP_NOTHING "keep", VIPS_FOREIGN_KEEP_NONE
#else
#define ANVIL_VIPS_KEEP_NOTHING "strip", TRUE
#endif

// Whether heifsave can be told not to subsample chroma. 4:2:0 quarters the
// resolution of the red and orange edges this photography is full of, and it is
// the half of the "blocky" artefact that raising Q does not fix.
//
// The option arrived in 8.15, the same release that renamed `strip` to `keep`.
// Below that version AVIF chroma is whatever the encoder picked and there is no
// way to say otherwise; jpegsave and webpsave have had their own spellings of
// the same control for far longer, so only this one needs the guard.
#if VIPS_MAJOR_VERSION > 8 || (VIPS_MAJOR_VERSION == 8 && VIPS_MINOR_VERSION >= 15)
#define ANVIL_VIPS_HAS_HEIF_SUBSAMPLE 1
#else
#define ANVIL_VIPS_HAS_HEIF_SUBSAMPLE 0
#endif

// Encodes a master in its container. Shared by the upload's normalisation and an
// edit's render, because the derived master of an edit is a master in every
// sense the variant code cares about, and two sets of quality settings for one
// kind of file would drift (docs/21-image-edits.md §4). Defined in strip.cc.
[[nodiscard]] int save_master(VipsImage* image, VipsTarget* target, fs::Mime mime);

}  // namespace anvil::images::detail

#endif  // ANVIL_HAS_VIPS
