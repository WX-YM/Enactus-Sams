#pragma once

// What a stored media object is on disk and in the database.
//
// Three rules govern every query over it, and all three are in the FILTER rather
// than in a caller:
//
//   * The NAMESPACE is part of every lookup. A media id belongs to exactly one
//     API, and a handler holding another namespace's id must get nothing back.
//     The check cannot be forgotten because it is not a check — it is part of
//     the query (anvil/fs/namespace.h).
//   * Reference counts move by $inc INSIDE the owning document's transaction. A
//     read-then-write on the count loses one of two concurrent attaches, and a
//     count adjusted outside the transaction that caused it is wrong the moment
//     that transaction aborts.
//   * The row is deleted BEFORE the file is unlinked. A crash between them
//     leaves an orphan file the sweeper reaps; the reverse order leaves a row
//     pointing at nothing, which is a 500 on the read path.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "anvil/core/types.h"
#include "anvil/crypto/secret.h"
#include "anvil/db/codec.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/paths.h"
#include "anvil/fs/sniff.h"
#include "anvil/images/variants.h"

namespace anvil::media {

namespace media_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kNamespace = "ns";
inline constexpr std::string_view kSha256 = "sha";
inline constexpr std::string_view kOwner = "own";
// The address the bytes arrived from, and it is here because the owner cannot
// carry it: an upload with no account behind it has an owner that means nothing,
// and every such upload's means the same nothing. This is the only handle abuse
// tooling has on who sent a file.
//
// OMITTED on disk when absent, never written null: the {ns, ip} index is partial
// on the field existing, and a null on every account-backed upload would put one
// entry per row into an index that exists to serve the minority of rows with an
// address at all.
inline constexpr std::string_view kUploaderIp = "uip";
inline constexpr std::string_view kBytes = "b";
inline constexpr std::string_view kWidth = "w";
inline constexpr std::string_view kHeight = "h";
inline constexpr std::string_view kMime = "m";
inline constexpr std::string_view kRefs = "refs";
inline constexpr std::string_view kVariants = "var";
inline constexpr std::string_view kCreatedAt = "created_at";
// An EDIT's three fields (docs/21-image-edits.md §3), omitted on a row that is
// not one — the same rule as `uip`, and for the same reason: the unique
// {ns, src, esha} index is partial on `src` existing.
//
// `src` is the source's id, so the editor reopens the source rather than the
// result. `edit` is the canonical recipe, so it reopens with the recipe loaded.
// `esha` is the SHA-256 of those bytes, the key that makes the same edit of the
// same source one object without any client-supplied key at all.
inline constexpr std::string_view kSource = "src";
inline constexpr std::string_view kEdit = "edit";
inline constexpr std::string_view kEditSha = "esha";
// Sub-keys of one variant row.
inline constexpr std::string_view kVariantWidth = "w";
inline constexpr std::string_view kVariantHeight = "h";
inline constexpr std::string_view kVariantFormat = "f";
inline constexpr std::string_view kVariantBytes = "b";
}  // namespace media_fields

// The PIN. A row carrying it is part of the deployment's own default content and
// must never be reclaimed by the garbage collector, however its ordinary
// references move.
//
// Large enough that ordinary attach and release traffic can never walk it down
// to zero, and small enough to stay obviously deliberate in a document dump.
//
// It is ADDED to the count rather than replacing it, so the ordinary count is
// still in there and comes back out by subtraction.
inline constexpr std::int32_t kPinnedRefs = 1'000'000;

// What the serving path needs, and nothing more. Projected rather than read
// whole: returning a large document to serve one file costs network, BSON decode
// CPU and heap on every media request (CLAUDE.md §7).
struct MediaRecord final {
    std::vector<images::VariantRecord>          variants;
    crypto::Digest256                           sha256;
    db::TimeMs                                  created_at;
    std::uint64_t                               bytes;
    Uuid                                        id;
    Uuid                                        owner;
    // ABSENT rather than all-zero for a row written without one. The unspecified
    // address is a real address, and a decoder that turned "we did not record
    // this" into it would make every such row look like one sender.
    std::optional<std::array<std::uint8_t, 16>> uploader_ip;
    std::uint32_t                               width;
    std::uint32_t                               height;
    // Attaches minus releases, PLUS kPinnedRefs when the row is pinned. Read it
    // through is_pinned() and attached_refs() rather than directly: raw, it is a
    // number that means two things at once, and rendering it as one puts "used
    // in 1000000 places" on a screen.
    std::int32_t                                refs;
    fs::Ns                                      ns;
    fs::Mime                                    mime;
    // An edit's canonical recipe, EMPTY on a row that is not an edit.
    std::vector<std::uint8_t>                   edit{};
    // The object this one was rendered from, ABSENT on a row that is not an
    // edit and on a detached one. A row carrying it holds one reference on
    // that source (docs/21-image-edits.md §3.2).
    std::optional<Uuid>                         source{};
};

// NEAR the pin, not at or above it. A pinned row can read BELOW the pin — a
// release with no matching attach walks it down — so an `>=` test calls those
// unpinned and then reports 999999 as their ordinary count, which is the same
// bug wearing a different number.
//
// Half the pin is the threshold because there is nothing to be careful about:
// the pin is deliberately larger than any reachable count, so everything real
// sits far below half of it and everything pinned sits far above.
[[nodiscard]] constexpr bool is_pinned(const MediaRecord& row) noexcept {
    return row.refs > kPinnedRefs / 2;
}

// The count a person is asking about: how many places actually use this object.
//
// Clamped at zero. A pinned row can read below the pin, and while that is an
// accounting fault worth finding, "used in -1 places" is not a thing to put on a
// screen — the caller that renders it logs the discrepancy instead.
[[nodiscard]] constexpr std::int32_t attached_refs(const MediaRecord& row) noexcept {
    const std::int32_t attached = is_pinned(row) ? row.refs - kPinnedRefs : row.refs;
    return attached < 0 ? 0 : attached;
}

// Where a listing resumed from. A PAIR, because two uploads share a millisecond
// often enough — a variant set is derived from one request — and an instant
// alone cannot express a position between two rows carrying the same one.
struct MediaCursor final {
    db::TimeMs created_at;
    Uuid       id;
};

struct NewMedia final {
    std::vector<images::VariantRecord>          variants;
    crypto::Digest256                           sha256;
    std::uint64_t                               bytes;
    Uuid                                        id;
    Uuid                                        owner;
    std::optional<std::array<std::uint8_t, 16>> uploader_ip;
    std::uint32_t                               width;
    std::uint32_t                               height;
    fs::Ns                                      ns;
    fs::Mime                                    mime;
    // An edit's three fields. All present or all absent: MediaRepository::
    // insert_edit is the only writer that sets them.
    std::vector<std::uint8_t>                   edit{};
    std::optional<Uuid>                         source{};
    std::optional<crypto::Digest256>            edit_sha{};
};

// One address, and what it has sent into one namespace. The unit an abuse screen
// is built out of and the unit a purge takes.
struct UploaderTotal final {
    std::array<std::uint8_t, 16> ip;
    std::int64_t                 bytes;
    std::int64_t                 count;
};

// A namespace's weight, and the addresses that account for most of it. `top` is
// LIMITED and ordered by bytes: the question is "who is filling the disk", and
// the answer is the first few rows or it is not an answer.
struct NamespaceUsage final {
    std::vector<UploaderTotal> top;
    std::int64_t               bytes;
    std::int64_t               count;
};

}  // namespace anvil::media
