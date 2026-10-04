#pragma once

// Rendering an edit recipe into a NEW object (docs/21-image-edits.md §4).
//
// This replaces images::apply_crop, which wrote its result under the SOURCE's
// id. Uploads are deduplicated per namespace, so two documents can hold one id,
// and cropping one of them cropped both; it also published variants one at a
// time over the ones being served, and a mid-set failure unlinked files the row
// still listed. All three came from writing a new result under an old name, so
// the fix is structural: every file an edit produces is written under the id
// the caller minted for the edit, and nothing under the source's id is opened
// for writing at all.

#include <cstdint>
#include <vector>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/paths.h"
#include "anvil/fs/sniff.h"
#include "anvil/images/recipe.h"
#include "anvil/images/variants.h"

namespace anvil::images {

// The recipe bounds this build publishes as `limits.edit`. The two edges are
// the ladder's widest and narrowest rungs, read from the table the variants are
// written from, so the published numbers and the enforced ones are one table.
inline constexpr EditLimits kEditLimits{kMaxEditStrokes, kMaxEditPoints,
                                        fs::kVariantWidths.back(), fs::kVariantWidths.front()};

struct RenderedEdit final {
    std::vector<VariantRecord> variants;
    std::uint64_t              master_bytes;
    std::uint32_t              width;
    std::uint32_t              height;
};

// Orients, crops, resizes and draws the source master (opened READ-ONLY through
// `source_master_fd`), encodes the result as the derived object's master in the
// source's container, publishes it under `id`, and derives every variant from
// it. `plan` is plan_edit's answer for this recipe against this source.
//
// On failure some files under `id` may exist, published or in tmp/; the caller
// owns that fresh id and removes them. Nothing under any other id is touched.
//
// Blocking. cpu_pool only.
[[nodiscard]] Result<RenderedEdit> render_edit(const fs::Storage& storage, fs::Ns ns,
                                               const Uuid& id, int source_master_fd,
                                               fs::Mime mime, const Recipe& recipe,
                                               const EditPlan& plan);

}  // namespace anvil::images
