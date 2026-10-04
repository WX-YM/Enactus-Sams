#pragma once

// An entry as it is stored and as a caller receives it.
//
// --- one document per entry, not two -------------------------------------------
//
// A section is two documents — published and draft under a compound `_id` — so
// that a public read cannot leak an unpublished string through a forgotten
// projection (docs/12-sections-cms.md §5). An entry is ONE document holding both
// stages, and the reason is what an entry has that a section does not: PLACEMENT.
// Its slug, its flags, its position and its child count belong to the entry
// rather than to either stage, and with two documents every one of them would
// have to be written twice, in a transaction, to stay in agreement — the
// multi-document invariant CLAUDE.md §6 asks to avoid where a single document
// will do.
//
// The leak the section design closes is closed here a different way: the
// published projection is built in exactly ONE place (repository.cc,
// `published_projection`) and excludes the draft subtree by name, and a test
// asserts that a draft-only string never comes back from a published read.
//
//   { _id:   <UUIDv7>,              the entry id; its time prefix is creation order
//     k:     "blog.post",           the kind key, from the registry
//     sc:    "blog.post" | "forum.reply/<parent id>",   the listing scope
//     p:     <UUID>,                the parent entry, child kinds only
//     slug:  "hello-world",         SlugRule::Unique only
//     o:     <int64>,               position; 0 for a time-ordered kind
//     f:     <int32>,               the flag bitset
//     n:     <int64>,               live child count
//     live:  <bool>,                a published copy exists
//     pub:   { data, media, etag, at, by } | null,
//     drf:   { data, media, etag, at, by } | null,   Editorial only
//     created_at, created_by, updated_at, v }
//
// `sc` exists because an index holds four keys (db/migrations.h) and a listing
// needs a scope, a stage, an order and a tiebreak. Folding kind and parent into
// one string is what lets `{sc, live, o, _id}` answer every listing this module
// issues, forward and backward, with no in-memory sort.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/types.h"
#include "anvil/crypto/fast_hash.h"
#include "anvil/db/codec.h"
#include "anvil/entries/registry.h"
#include "anvil/sections/content.h"

namespace anvil::entries {

// The stored field names, published so an application's index and query
// catalogue name the columns anvil writes rather than a literal that drifts
// (the same reason every other subsystem publishes its own).
namespace entry_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kKind = "k";
inline constexpr std::string_view kScope = "sc";
inline constexpr std::string_view kParent = "p";
inline constexpr std::string_view kSlug = "slug";
inline constexpr std::string_view kPosition = "o";
inline constexpr std::string_view kFlags = "f";
inline constexpr std::string_view kChildren = "n";
inline constexpr std::string_view kLive = "live";
inline constexpr std::string_view kPublished = "pub";
inline constexpr std::string_view kDraft = "drf";
inline constexpr std::string_view kCreatedAt = "created_at";
inline constexpr std::string_view kCreatedBy = "created_by";
inline constexpr std::string_view kUpdatedAt = "updated_at";

// Inside `pub` and `drf`, beside the section codec's `data` and `media`.
inline constexpr std::string_view kStageEtag = "etag";
inline constexpr std::string_view kStageAt = "at";
inline constexpr std::string_view kStageBy = "by";
}  // namespace entry_fields

// Which copy a read wants.
enum class Stage : std::uint8_t {
    // What readers see: live entries only, and never the draft subtree.
    Published,
    // What an editor works on: every entry in the scope, live or not, with both
    // copies. For an Immediate kind the working copy IS the published one.
    Working,
};

// One stage's content and who last wrote it.
struct EntryContent final {
    sections::SectionContent content;
    crypto::FastDigest       etag;
    db::TimeMs               updated_at;
    Uuid                     updated_by;
};

struct EntryDocument final {
    std::optional<EntryContent> published;
    // Present only on a Working read of an Editorial kind.
    std::optional<EntryContent> draft;
    std::string                 slug;
    // The kind this was decoded against: a row whose `k` the registry no longer
    // declares is never returned at all.
    const KindSpec*             kind;
    std::optional<Uuid>         parent;
    db::TimeMs                  created_at;
    db::TimeMs                  updated_at;
    std::int64_t                position;
    std::int64_t                children;
    std::int64_t                version;
    Uuid                        id;
    Uuid                        created_by;
    FlagSet                     flags;

    [[nodiscard]] bool live() const noexcept { return published.has_value(); }

    // The copy an editor starts from: the draft where the workflow has one, the
    // published copy otherwise. nullptr only for an Editorial entry read at
    // Stage::Published, which carries no draft by construction.
    [[nodiscard]] const EntryContent* working() const noexcept {
        if (draft.has_value()) { return &*draft; }
        return published.has_value() ? &*published : nullptr;
    }

    [[nodiscard]] bool has(FlagSet mask) const noexcept { return (flags & mask) == mask; }
};

// Where the next page starts. Only a time-ordered kind pages: a manually ordered
// scope is bounded by kMaxManualCapacity and is returned whole, which is what a
// person dragging it into order needs anyway.
struct EntryCursor final {
    Uuid after;
};

struct EntryQuery final {
    // Required for a child kind, refused for a root kind.
    std::optional<Uuid>        parent{};
    std::optional<EntryCursor> after{};
    // Every entry returned has ALL of these bits.
    FlagSet                    flags_all{0};
    // Clamped to [1, kMaxPage] for a time-ordered kind; ignored for a manual one.
    std::int32_t               limit{20};
    Stage                      stage{Stage::Published};
};

inline constexpr std::int32_t kMaxPage = 100;

struct EntryPage final {
    std::vector<EntryDocument> entries;
    std::optional<EntryCursor> next;
};

}  // namespace anvil::entries
