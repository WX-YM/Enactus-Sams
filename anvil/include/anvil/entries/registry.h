#pragma once

// The entry registry: sections that repeat.
//
// --- what this adds to the sections CMS ---------------------------------------
//
// A section is one document whose SHAPE a developer owns and whose CONTENT staff
// own (anvil/sections/registry.h). That split is right for a page's hero and
// wrong for a portfolio, a news feed, a forum or a comment thread, where the
// NUMBER of documents is content too: a project is added by a staff member, a
// post by a member, and neither is a deploy.
//
// An entry kind keeps the half of the split that matters and moves the other.
// The shape is still a `constexpr` table in `.rodata` — the kind's
// `SectionSpec` is bound, validated, canonicalised, hashed, stored and
// serialised by exactly the code a section is, so a field type means one thing
// everywhere. What becomes data is only the INSTANCE: its id, which the server
// mints, its slug, which is checked here, and where it sits among its siblings.
//
// The allow-list property survives intact, and it is the reason this is a kind
// table rather than a "section with a wildcard key". A request names a kind that
// is in the table or is refused; the content it carries is bound against that
// kind's fields and nothing else; an entry id is never a key, a collection name
// or a path; and a slug is a URL segment that is compared, never interpolated.
//
// --- what the application configures, and why each knob exists ----------------
//
// One mechanism serves a portfolio and a forum because the differences between
// them are a handful of independent choices, each of which the table states:
//
//   workflow  Editorial — staff write a draft and publish it (a project, an
//             article). Immediate — a write is what readers see (a post, a
//             reply, a comment).
//   ordering  Manual — a position staff set (a portfolio's running order).
//             Newest / Oldest — creation order, which a UUIDv7 id already is.
//   slug      Unique — the entry has a URL segment, unique within the kind.
//             None — it is addressed by id alone.
//   flags     Up to sixteen application-named booleans ("pinned", "featured",
//             "locked", "hidden"), stored as one bitset and filterable in a
//             listing. Placement, not content: they apply at once, in every
//             workflow.
//   parent    The kind this one hangs under (a reply under a thread). A child
//             entry names its parent; the parent counts its children.
//   capacity  The most entries one scope may hold — per parent for a child
//             kind, per kind otherwise.
//
// anvil ships the vocabulary, the checks and the lookups. The table is yours
// (docs/01-seams.md §6, docs/20-entries.md).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/core/locale.h"
#include "anvil/sections/defaults.h"
#include "anvil/sections/registry.h"

namespace anvil::entries {

// --- the knobs ----------------------------------------------------------------
//
// None of these is stored. They are compile-time classifications of a kind, so
// the enumerators may be reordered freely.

enum class Workflow : std::uint8_t {
    // Writes land on the draft. Readers see the published copy, which only
    // publish() replaces and unpublish() removes. An entry created under this
    // workflow is invisible until it is first published.
    Editorial,
    // Writes land on the published copy. There is no draft.
    Immediate,
};

enum class Ordering : std::uint8_t {
    // A position staff set with reorder(). New entries go last.
    Manual,
    // Creation order, newest first. The id is a UUIDv7, whose leading 48 bits
    // are the creation time in milliseconds, so the primary key already is the
    // order and no second field has to agree with it.
    Newest,
    // Creation order, oldest first — a comment thread read top to bottom.
    Oldest,
};

enum class SlugRule : std::uint8_t {
    None,
    // Every entry carries a slug, unique within the kind, enforced by a unique
    // index rather than by a read before the write.
    Unique,
};

// --- flags ---------------------------------------------------------------------

// Sixteen, because the bitset is stored as one int32 and the listing filters on
// it with $bitsAllSet — and because a kind needing more than sixteen named
// switches is describing content, which belongs in its fields.
inline constexpr std::size_t kMaxFlagsPerKind = 16;

using FlagSet = std::uint16_t;

// One named switch. The label is for the staff editor, one per locale, for the
// same reason a FieldSpec carries one.
struct FlagSpec final {
    std::string_view key;
    Localized<>      label;
};

// --- the kind ------------------------------------------------------------------

// The most entries a manually ordered scope may hold. reorder() rewrites every
// position in the scope in one transaction, so the scope has to be small enough
// for that to be a reasonable transaction — and a list a person drags into
// order is small anyway.
inline constexpr std::uint32_t kMaxManualCapacity = 1000;

struct KindSpec final {
    // The shape. `shape.key` is the kind's key, spelled and checked the way a
    // section key is; `shape.site_path` is the page entries of this kind appear
    // on, which a preview lands the reader on.
    sections::SectionSpec     shape;
    std::span<const FlagSpec> flags;
    // The key of the parent kind, or empty for a root kind.
    std::string_view          parent;
    std::uint32_t             capacity;
    Workflow                  workflow;
    Ordering                  ordering;
    SlugRule                  slug;
};

// --- slugs ---------------------------------------------------------------------

inline constexpr std::size_t kMaxSlugBytes = 64;

// Lowercase ASCII letters and digits in runs joined by single hyphens: `garden-cafe`,
// `2026-review`. No leading, trailing or doubled hyphen, and nothing that needs
// escaping in a URL, an HTML attribute or a log line — a slug is printed in all
// three, and a rule this narrow means none of them has to think about it.
//
// ASCII on purpose. A slug is an identifier, not copy: the title is localised
// and the slug is the one spelling both editions of a URL share.
[[nodiscard]] constexpr bool is_wellformed_slug(std::string_view slug) noexcept {
    if (slug.empty() || slug.size() > kMaxSlugBytes) { return false; }
    bool previous_was_hyphen = true;   // a leading hyphen is invalid
    for (const char c : slug) {
        if (c == '-') {
            if (previous_was_hyphen) { return false; }
            previous_was_hyphen = true;
            continue;
        }
        const bool is_lower = c >= 'a' && c <= 'z';
        const bool is_digit = c >= '0' && c <= '9';
        if (!is_lower && !is_digit) { return false; }
        previous_was_hyphen = false;
    }
    return !previous_was_hyphen;
}

// --- lookup --------------------------------------------------------------------

// The position of `key` in the table, or `kinds.size()` when it is absent.
//
// The compile-time checks below search with this rather than with find_kind:
// GCC refuses to constant-evaluate a comparison of a pointer into a
// namespace-scope table against nullptr under -fsanitize=undefined, and the
// sanitiser build is the one CI gates on (the same constraint
// sections::defaults_match_registry is written around).
[[nodiscard]] constexpr std::size_t kind_index(std::span<const KindSpec> kinds,
                                               std::string_view key) noexcept {
    std::size_t low = 0;
    std::size_t high = kinds.size();
    while (low < high) {
        const std::size_t mid = low + ((high - low) / 2);
        if (kinds[mid].shape.key < key) {
            low = mid + 1;
        } else if (key < kinds[mid].shape.key) {
            high = mid;
        } else {
            return mid;
        }
    }
    return kinds.size();
}

// nullptr for an unknown key, which a caller answers with the bytes a
// nonexistent route produces. Binary search: the table is sorted by key, which
// kinds_are_well_formed() asserts.
[[nodiscard]] constexpr const KindSpec* find_kind(std::span<const KindSpec> kinds,
                                                  std::string_view key) noexcept {
    std::size_t low = 0;
    std::size_t high = kinds.size();
    while (low < high) {
        const std::size_t mid = low + ((high - low) / 2);
        if (kinds[mid].shape.key < key) {
            low = mid + 1;
        } else if (key < kinds[mid].shape.key) {
            high = mid;
        } else {
            return &kinds[mid];
        }
    }
    return nullptr;
}

// The bit for a named flag, or zero when the kind declares no such flag. Zero is
// never a declared bit, so a caller testing `mask != 0` cannot mistake an
// unknown name for a flag.
[[nodiscard]] constexpr FlagSet flag_bit(const KindSpec& kind, std::string_view key) noexcept {
    for (std::size_t i = 0; i < kind.flags.size(); ++i) {
        if (kind.flags[i].key == key) { return static_cast<FlagSet>(1U << i); }
    }
    return 0;
}

// Every bit this kind declares. A stored or requested bit outside it is refused.
[[nodiscard]] constexpr FlagSet declared_flags(const KindSpec& kind) noexcept {
    return kind.flags.size() >= kMaxFlagsPerKind
               ? static_cast<FlagSet>(0xFFFFU)
               : static_cast<FlagSet>((1U << kind.flags.size()) - 1U);
}

// --- table conformance ---------------------------------------------------------
//
// One check for the whole table, because every clause is a mistake that would
// otherwise ship and none of them is worth a separate assert at the call site.
// static_assert it over your own table.

namespace detail {

[[nodiscard]] constexpr bool flags_are_well_formed(const KindSpec& kind) noexcept {
    if (kind.flags.size() > kMaxFlagsPerKind) { return false; }
    for (std::size_t i = 0; i < kind.flags.size(); ++i) {
        if (!sections::is_wellformed_key(kind.flags[i].key)) { return false; }
        // A dotted flag name would read as a path in every consumer that prints
        // one; the key rule allows dots, so this is narrower.
        for (const char c : kind.flags[i].key) {
            if (c == '.') { return false; }
        }
        if (!kind.flags[i].label.complete()) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (kind.flags[j].key == kind.flags[i].key) { return false; }
        }
    }
    return true;
}

// The parent chain is finite. Walked at most kinds.size() steps; a chain that
// is still going after that has visited some kind twice.
[[nodiscard]] constexpr bool parent_chain_ends(std::span<const KindSpec> kinds,
                                               const KindSpec& kind) noexcept {
    std::string_view parent = kind.parent;
    for (std::size_t step = 0; step <= kinds.size(); ++step) {
        if (parent.empty()) { return true; }
        const std::size_t at = kind_index(kinds, parent);
        if (at == kinds.size()) { return false; }
        parent = kinds[at].parent;
    }
    return false;
}

}  // namespace detail

// Sorted strictly by key with every key well formed; every shape passes the same
// three checks a section registry does; flags are named, unique, labelled and
// at most sixteen; a parent is a declared kind and no chain loops; capacity is
// set, and a manually ordered kind stays small enough to reorder in one
// transaction.
[[nodiscard]] constexpr bool kinds_are_well_formed(std::span<const KindSpec> kinds) noexcept {
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const KindSpec& kind = kinds[i];
        if (!sections::is_wellformed_key(kind.shape.key)) { return false; }
        if (i > 0 && !(kinds[i - 1].shape.key < kind.shape.key)) { return false; }

        const std::span<const sections::SectionSpec> shape{&kind.shape, 1};
        if (!sections::fields_fit_buffers(shape)) { return false; }
        if (!sections::choices_are_well_formed(shape)) { return false; }
        for (std::size_t f = 0; f < kind.shape.fields.size(); ++f) {
            if (!kind.shape.fields[f].label.complete()) { return false; }
            for (std::size_t g = 0; g < f; ++g) {
                if (kind.shape.fields[g].key == kind.shape.fields[f].key) { return false; }
            }
        }

        if (!detail::flags_are_well_formed(kind)) { return false; }
        if (kind.parent == kind.shape.key) { return false; }
        if (!detail::parent_chain_ends(kinds, kind)) { return false; }

        if (kind.capacity == 0) { return false; }
        if (kind.ordering == Ordering::Manual && kind.capacity > kMaxManualCapacity) {
            return false;
        }
    }
    return true;
}

// --- seeds ---------------------------------------------------------------------
//
// What a fresh database holds. Sections need a default for every key, because a
// page reads every key; a kind needs none, because an empty portfolio is a
// legitimate state. Where an application does want entries on day one, it seeds
// them — ONCE, ever, per kind (bootstrap.h says why "once" and not "if absent").

struct EntrySeed final {
    // Empty for a kind whose slug rule is None.
    std::string_view          slug;
    // The content, checked against the kind's shape exactly as a section's
    // defaults are: every field, both locales, within bounds, images as files.
    // `content.key` is the kind's key.
    sections::SectionDefaults content;
    FlagSet                   flags;
};

struct KindSeeds final {
    std::string_view           kind;
    // In the order they are to appear. For a manually ordered kind this is the
    // initial running order; for a time-ordered one it is the insertion order.
    std::span<const EntrySeed> entries;
};

// Every seeded kind exists and is a ROOT kind (a child names a parent id, which
// no compile-time table can know); no kind is seeded twice; the entries fit the
// capacity; each one's slug obeys the kind's rule and is unique among the seeds;
// its flags are declared; and its content passes defaults_match_registry against
// the kind's shape.
[[nodiscard]] constexpr bool seeds_match_kinds(std::span<const KindSpec> kinds,
                                               std::span<const KindSeeds> seeds) noexcept {
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        const KindSeeds& seeded = seeds[i];
        const std::size_t at = kind_index(kinds, seeded.kind);
        if (at == kinds.size()) { return false; }
        const KindSpec* kind = &kinds[at];
        if (!kind->parent.empty()) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (seeds[j].kind == seeded.kind) { return false; }
        }
        if (seeded.entries.size() > kind->capacity) { return false; }

        for (std::size_t e = 0; e < seeded.entries.size(); ++e) {
            const EntrySeed& entry = seeded.entries[e];
            if (kind->slug == SlugRule::Unique) {
                if (!is_wellformed_slug(entry.slug)) { return false; }
                for (std::size_t d = 0; d < e; ++d) {
                    if (seeded.entries[d].slug == entry.slug) { return false; }
                }
            } else if (!entry.slug.empty()) {
                return false;
            }
            if ((entry.flags & static_cast<FlagSet>(~declared_flags(*kind))) != 0) {
                return false;
            }
            if (!sections::defaults_match_registry(
                    std::span<const sections::SectionSpec>{&kind->shape, 1},
                    std::span<const sections::SectionDefaults>{&entry.content, 1})) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace anvil::entries
