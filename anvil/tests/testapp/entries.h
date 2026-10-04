#pragma once

// The reference application's entry kinds and seeds.
//
// Compiled by every build of the suite, for the reason sections.h is: the worked
// example in docs/20-entries.md has to keep compiling, and kinds_are_well_formed
// and seeds_match_kinds have to be evaluated against a real table on every build.
//
// Four kinds, chosen so that every knob is exercised by one of them and no two
// of them are the same product:
//
//   blog.post      Editorial, newest first, unique slugs, two flags — a news
//                  feed with a pinned story
//   forum.reply    Immediate, oldest first, under forum.thread, THREE per thread
//                  so the exact per-parent bound is reachable in a test
//   forum.thread   Immediate, newest first, a "locked" flag
//   gallery.item   Editorial, staff-ordered, unique slugs, an image slot, small
//                  capacity, seeded — a portfolio

#include <array>
#include <string_view>

#include "anvil/entries/registry.h"
#include "anvil/sections/defaults.h"
#include "anvil/sections/registry.h"

namespace testapp {

namespace ent = anvil::entries;
namespace esec = anvil::sections;

inline constexpr std::array<esec::FieldSpec, 2> kPostFields{{
    {"title", {{"Title", "العنوان"}}, {}, 120, esec::FieldType::Text, true, true},
    {"body", {{"Body", "الكلام"}}, {}, 2000, esec::FieldType::RichText, true, false},
}};

inline constexpr std::array<ent::FlagSpec, 2> kPostFlags{{
    {"pinned", {{"Pinned", "متثبت"}}},
    {"featured", {{"Featured", "مميز"}}},
}};

inline constexpr std::array<esec::FieldSpec, 1> kReplyFields{{
    // Not localised: a member writes a reply in the language they write in.
    {"text", {{"Reply", "الرد"}}, {}, 500, esec::FieldType::Text, false, true},
}};

inline constexpr std::array<esec::FieldSpec, 1> kThreadFields{{
    {"subject", {{"Subject", "الموضوع"}}, {}, 120, esec::FieldType::Text, false, true},
}};

inline constexpr std::array<ent::FlagSpec, 1> kThreadFlags{{
    {"locked", {{"Locked", "مقفول"}}},
}};

inline constexpr std::array<esec::FieldSpec, 2> kGalleryFields{{
    {"name", {{"Name", "الاسم"}}, {}, 60, esec::FieldType::Text, true, true},
    {"link", {{"Link", "اللينك"}}, {}, 256, esec::FieldType::Url, false, true},
}};

inline constexpr std::array<esec::ImageSpec, 1> kGalleryImages{{
    {"shot", {{"Screenshot", "صورة"}}, 64, 36, 16, 9},
}};

inline constexpr std::array<ent::FlagSpec, 1> kGalleryFlags{{
    {"pinned", {{"On the home page", "في الصفحة الرئيسية"}}},
}};

// Sorted by key: find_kind binary-searches, and kinds_are_well_formed says so.
inline constexpr std::array<ent::KindSpec, 4> kKinds{{
    {{"blog.post", "/blog", kPostFields, {}}, kPostFlags, "", 10000,
     ent::Workflow::Editorial, ent::Ordering::Newest, ent::SlugRule::Unique},
    {{"forum.reply", "/forum", kReplyFields, {}}, {}, "forum.thread", 3,
     ent::Workflow::Immediate, ent::Ordering::Oldest, ent::SlugRule::None},
    {{"forum.thread", "/forum", kThreadFields, {}}, kThreadFlags, "", 100000,
     ent::Workflow::Immediate, ent::Ordering::Newest, ent::SlugRule::None},
    {{"gallery.item", "/", kGalleryFields, kGalleryImages}, kGalleryFlags, "", 5,
     ent::Workflow::Editorial, ent::Ordering::Manual, ent::SlugRule::Unique},
}};

static_assert(ent::kinds_are_well_formed(kKinds),
              "kinds unsorted, a malformed key or flag, a shape that fails the section "
              "checks, a parent that is not declared or loops, or a capacity that is zero or "
              "too large to reorder");

inline constexpr std::array<esec::DefaultField, 2> kFirstItem{{
    {"name", {{"First", "الأول"}}},
    {"link", {{"https://example.com/first", ""}}},
}};
inline constexpr std::array<esec::DefaultField, 2> kSecondItem{{
    {"name", {{"Second", "التاني"}}},
    {"link", {{"/second", ""}}},
}};
inline constexpr std::array<esec::DefaultImage, 1> kFirstShot{{{"shot", "first.png"}}};
inline constexpr std::array<esec::DefaultImage, 1> kSecondShot{{{"shot", "second.png"}}};

inline constexpr std::array<ent::EntrySeed, 2> kGallerySeeds{{
    {"first", {"gallery.item", kFirstItem, kFirstShot}, 1},
    {"second", {"gallery.item", kSecondItem, kSecondShot}, 0},
}};

inline constexpr std::array<ent::KindSeeds, 1> kSeeds{{
    {"gallery.item", kGallerySeeds},
}};

static_assert(ent::seeds_match_kinds(kKinds, kSeeds),
              "a seed names an undeclared or child kind, breaks its slug rule, sets an "
              "undeclared flag, overfills the kind, or fails the kind's own field checks");

inline constexpr std::string_view kEntriesCollection = "entries";

}  // namespace testapp
