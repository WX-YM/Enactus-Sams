#pragma once

// The reference application's section registry and its shipped default content.
//
// Compiled by every build of the test suite, so the worked example in
// docs/01-seams.md §6 is a file that must keep compiling rather than a snippet
// that can rot — and so every conformance check anvil ships is evaluated against
// a real table on every build.
//
// The table is deliberately SMALL and deliberately awkward. Four sections cover
// every FieldType anvil ships, a localised and a non-localised field of each
// kind that has both, a Choice field with its own allow-list, an image slot with
// an aspect constraint and one without, an optional field and a required one,
// and two pages so the dotted-prefix rule has something to separate.
//
// The Arabic literals are not decoration: they are what proves ct::is_valid_utf8
// runs over real multi-byte text at build time, and what makes the code-point
// bounds mean something. A byte limit would silently halve the Arabic allowance,
// which is the defect the `max_cp` column exists to prevent.

#include <array>
#include <cstddef>
#include <string_view>

#include "anvil/sections/defaults.h"
#include "anvil/sections/registry.h"

namespace testapp {

namespace sec = anvil::sections;

// --- the choice allow-list --------------------------------------------------
//
// SORTED, because sec::is_choice binary-searches it and choices_are_well_formed
// asserts it. A value added out of order would be a lookup that quietly misses,
// and the symptom is a staff member typing a name the server refuses for no
// reason anybody can see.
//
// This list is the APPLICATION'S vocabulary. anvil ships FieldType::Choice and
// the binary search; a table of icon names would be one product's furniture
// compiled into a library (ENGINEERING_RULES.md §1).
inline constexpr std::array<std::string_view, 4> kIcons{{
    "calendar", "coffee", "map-pin", "star",
}};

// --- the registry -----------------------------------------------------------

inline constexpr std::array<sec::FieldSpec, 5> kHomeHeroFields{{
    {"headline", {{"Headline", "العنوان"}}, {}, 80, sec::FieldType::Text, true, true},
    {"subline", {{"Sub-line", "السطر التاني"}}, {}, 160, sec::FieldType::Text, true, false},
    // Not localised: a URL is not translated, and repeating it per locale would
    // make "the two disagree" a representable state nothing checks.
    {"cta_href", {{"Button link", "لينك الزرار"}}, {}, 256, sec::FieldType::Url, false, true},
    {"accent", {{"Accent colour", "اللون المميز"}}, {}, 7, sec::FieldType::Color, false, false},
    // max_cp is 0 because a Bool has no text to bound, and value_matches_spec
    // exempts Bool from the code-point check for exactly that reason.
    {"show", {{"Show the band", "إظهار الشريط"}}, {}, 0, sec::FieldType::Bool, false, false},
}};

inline constexpr std::array<sec::ImageSpec, 1> kHomeHeroImages{{
    {"hero", {{"Hero banner", "بانر رئيسي"}}, 1920, 1080, 16, 9},
}};

inline constexpr std::array<sec::FieldSpec, 3> kHomeAboutFields{{
    {"title", {{"Title", "العنوان"}}, {}, 80, sec::FieldType::Text, true, true},
    {"body", {{"Description", "الوصف"}}, {}, 4000, sec::FieldType::RichText, true, true},
    {"icon", {{"Icon beside it", "الأيقونة جنبه"}}, kIcons, 24, sec::FieldType::Choice, false,
     false},
}};

inline constexpr std::array<sec::ImageSpec, 1> kHomeAboutImages{{
    // 0/0 — no aspect constraint, because this slot is rendered with
    // `object-fit: cover` and declaring a ratio the layout then ignores would
    // reject uploads for a requirement that does not exist. The endpoints
    // serialise it as a null aspect rather than as 0:0.
    {"portrait", {{"About photo", "صورة عن المكان"}}, 600, 600, 0, 0},
}};

inline constexpr std::array<sec::ImageSpec, 0> kNoImages{};

inline constexpr std::array<sec::FieldSpec, 2> kContactInfoFields{{
    {"address", {{"Address", "عنوان المكان"}}, {}, 240, sec::FieldType::Text, true, true},
    {"seats", {{"Seats", "عدد الكراسي"}}, {}, 8, sec::FieldType::Number, false, false},
}};

// SORTED BY KEY. The sort is asserted below and the lookup is a binary search
// that depends on it. Keys are dotted identifiers: the segment before the first
// dot is the page, which is what makes a request for a whole page one scan of a
// contiguous range rather than a walk of the table.
inline constexpr std::array<sec::SectionSpec, 3> kSections{{
    {"contact.info", "/contact", kContactInfoFields, kNoImages},
    {"home.about", "/", kHomeAboutFields, kHomeAboutImages},
    // Sorts after `home.about`, which keeps every `home.*` key one contiguous
    // range.
    {"home.hero", "/", kHomeHeroFields, kHomeHeroImages},
}};

static_assert(sec::registry_is_sorted(kSections),
              "kSections must be sorted by key with no duplicates, and every key must be a "
              "dotted lowercase identifier: the lookup is a binary search");
static_assert(sec::fields_fit_buffers(kSections),
              "raise kMaxFieldsPerSection, and never declare an Image field");
static_assert(sec::choices_are_well_formed(kSections),
              "a Choice field needs a sorted, non-empty allow-list and nothing else may "
              "carry one");

// --- the defaults -----------------------------------------------------------
//
// Every field needs one, not only the required ones: a fresh deployment must
// produce a complete, renderable section, and a section missing an optional
// string renders a gap rather than an error.
//
// A non-localised field leaves every slot but the default locale's EMPTY.

inline constexpr std::array<sec::DefaultField, 5> kHomeHeroDefaults{{
    {"headline", {{"Where community meets exploration.", "المكان اللي بيجمع الناس والطريق."}}},
    {"subline", {{"A café, a workshop and a yard.", "كافيه وورشة ويارد."}}},
    // A fragment reference: no scheme, no host, no path, and nothing to redirect
    // to. is_safe_default_url accepts it, and it accepts nothing an ordinary
    // absolute URL would smuggle past a runtime check.
    {"cta_href", {{"#/events", ""}}},
    {"accent", {{"#134411", ""}}},
    {"show", {{"true", ""}}},
}};

inline constexpr std::array<sec::DefaultImage, 1> kHomeHeroDefaultImages{{
    {"hero", "home-hero.jpg"},
}};

inline constexpr std::array<sec::DefaultField, 3> kHomeAboutDefaults{{
    {"title", {{"One yard, two doors.", "يارد واحد وبابين."}}},
    // Both tags survive the sanitiser's allow-list unchanged, which is what
    // makes this a default the RichText binder would also accept on a write.
    {"body", {{"<p>It started as a place to leave your bike.</p>",
               "<p>بدأ المكان عشان تسيب فيه عجلتك.</p>"}}},
    // A real member of kIcons, never blank: "none" is a choice a staff member
    // makes, not a state a fresh deployment ships in.
    {"icon", {{"coffee", ""}}},
}};

inline constexpr std::array<sec::DefaultImage, 1> kHomeAboutDefaultImages{{
    {"portrait", "home-about.jpg"},
}};

inline constexpr std::array<sec::DefaultImage, 0> kNoDefaultImages{};

inline constexpr std::array<sec::DefaultField, 2> kContactInfoDefaults{{
    {"address", {{"12 Mostafa Kamel", "١٢ شارع مصطفى كامل"}}},
    {"seats", {{"48", ""}}},
}};

// POSITIONALLY identical to kSections, which defaults_match_registry asserts, so
// bootstrap walks both by index and looks nothing up.
inline constexpr std::array<sec::SectionDefaults, kSections.size()> kDefaults{{
    {"contact.info", kContactInfoDefaults, kNoDefaultImages},
    {"home.about", kHomeAboutDefaults, kHomeAboutDefaultImages},
    {"home.hero", kHomeHeroDefaults, kHomeHeroDefaultImages},
}};

static_assert(sec::defaults_match_registry(kSections, kDefaults),
              "a default violates its FieldSpec, is missing a locale, is not valid UTF-8, or "
              "a section has no defaults at all");

// The collection the two documents per key live in. Named here rather than at
// each construction site so the service and the index catalogue cannot disagree.
inline constexpr std::string_view kSectionsCollection = "sections";

}  // namespace testapp
