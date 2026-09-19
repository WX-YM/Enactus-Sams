#pragma once

// Derived sizes (docs/08-images.md §3).
//
// Generated ONCE, at upload, from the master. Never per request:
// re-encoding on demand puts image work on the read path, makes latency
// unpredictable, and lets an attacker force arbitrary CPU by asking for many
// size and format combinations.
//
// This is what replaces the brief's "staff should upload different sizes for
// different screens": one upload, derived breakpoints, and a variant list the
// dashboard and the site turn into a `srcset`.
//
// Widths only — height follows the aspect ratio. Never upscaled: an 800 px
// master produces 320 and 640 and nothing else, because upscaling wastes bytes
// and looks worse than letting the browser do it.

#include <cstdint>
#include <vector>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/paths.h"
#include "anvil/images/probe.h"

namespace anvil::images {

// One row of the `variants` array in the media document. 8 bytes, ordered
// largest-first so the array packs with no padding.
struct VariantRecord final {
    std::uint32_t bytes;
    std::uint16_t width;
    std::uint16_t height;
    fs::Format    format;
};

static_assert(sizeof(VariantRecord) == 12, "packed variant row");

// Derives every variant that does not upscale, publishes each by rename, and
// returns what was actually written. Each variant is generated FROM THE MASTER;
// chaining lossy re-encodes through a smaller variant compounds artefacts.
//
// A format this build cannot encode is skipped rather than failed: a missing
// AVIF delegate should cost the AVIF variants, not the upload. A format that
// CAN be encoded but fails mid-encode aborts the whole set, so a half-populated
// variant list is never recorded.
//
// Blocking. cpu_pool only.
[[nodiscard]] Result<std::vector<VariantRecord>> generate_variants(const fs::Storage& storage,
                                                                   fs::Ns ns, const Uuid& id,
                                                                   int master_fd,
                                                                   ImageInfo master);

// Removes every variant file for `id`, enumerated from `variants` rather than
// from a readdir glob — a glob is racy, slow, and would delete a file belonging
// to a different id that happens to share a prefix (docs/07-filesystem.md §7).
void unlink_variants(const fs::Storage& storage, fs::Ns ns, const Uuid& id,
                     const std::vector<VariantRecord>& variants) noexcept;

}  // namespace anvil::images
