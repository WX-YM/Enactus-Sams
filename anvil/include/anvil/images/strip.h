#pragma once

// Metadata stripping and normalisation (docs/08-images.md §2).
//
// Strip by default, and keep exactly two things — for reasons that pull in
// opposite directions:
//
//   Privacy       EXIF carries GPS coordinates, device serial numbers, capture
//                 timestamps and sometimes the photographer's name. A cafe
//                 posting a picture of its terrace must not publish its owner's
//                 home coordinates from an earlier shot on the same camera.
//   Colour        Stripping EVERYTHING removes the ICC profile, and an image
//                 authored in Adobe RGB or Display P3 then renders desaturated
//                 in every viewer that honours one.
//
// The resolution is to convert to sRGB during processing and drop the profile:
// output is unambiguous, every variant is consistent, and a few KB per file is
// saved.
//
// ORIENTATION IS THE CLASSIC BUG. Removing the EXIF orientation
// tag without applying it renders every phone portrait sideways. The rotation
// is baked into the pixels and only then is the tag dropped.
//
// The embedded thumbnail matters too: a cropped image can otherwise retain a
// thumbnail of the UNCROPPED original.

#include <cstdint>

#include "anvil/core/result.h"
#include "anvil/fs/sniff.h"
#include "anvil/images/probe.h"

namespace anvil::images {

// Reads the raw upload from `source_fd`, applies orientation, converts to sRGB,
// drops every metadata block, and writes the result to `output_fd`.
//
// The stored master is this normalised file, not the bytes as uploaded. That
// costs one re-encode at upload time and buys a guarantee: no code path, and no
// future consumer, can ever be handed the original EXIF. Variants are derived
// from this master and never from each other, so the re-encode is not chained
// (docs/08-images.md §3, recorded in).
//
// Blocking. cpu_pool only.
[[nodiscard]] Result<ImageInfo> normalise_master(int source_fd, fs::Mime mime, int output_fd);

}  // namespace anvil::images
