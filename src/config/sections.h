#pragma once

// The public website's editable copy (anvil docs/12-sections-cms.md).
//
// The SHAPE is here, compiled in; the CONTENT is in MongoDB and edited from the
// Content CMS. A key that is not in this table cannot be written — there is no
// passthrough, which is the whole of the CMS's mass-assignment defence.
//
// Every field has a default, and the defaults are the site's real copy: they are
// what a fresh database publishes. The legacy migration overwrites them with the
// content the old backend held.
//
// Images that repeat (the about photos, the life gallery, the Tafrah shots) are
// entries, not section slots — see entries.h. The one image here is the Tafrah
// platform screenshot, which is a single fixed slot.

#include <array>
#include <string_view>

#include "anvil/sections/defaults.h"
#include "anvil/sections/registry.h"

namespace enactus {

namespace sec = anvil::sections;
using sec::FieldType;

inline constexpr std::array<sec::ImageSpec, 0> kNoImages{};
inline constexpr std::array<sec::DefaultImage, 0> kNoDefaultImages{};

// --- home.about --------------------------------------------------------------
inline constexpr std::array<sec::FieldSpec, 5> kAboutFields{{
    {"heading", {{"Heading"}}, {}, 160, FieldType::Text, false, true},
    {"kicker", {{"Kicker"}}, {}, 80, FieldType::Text, false, true},
    {"p1", {{"First paragraph"}}, {}, 1200, FieldType::Text, false, true},
    {"p2", {{"Second paragraph"}}, {}, 1200, FieldType::Text, false, false},
    {"tags", {{"Tags (comma separated)"}}, {}, 300, FieldType::Text, false, false},
}};
inline constexpr std::array<sec::DefaultField, 5> kAboutDefaults{{
    {"heading", {{"A club that builds businesses to solve problems."}}},
    {"kicker", {{"01 — Who we are"}}},
    {"p1", {{"Enactus is a global network of students, academics and business leaders. Our "
             "chapter at Sadat Academy — Maadi runs the full cycle ourselves: we find a problem "
             "in our community, design a venture that answers it, build it, measure it, and "
             "defend it in front of national judges."}}},
    {"p2", {{"Nobody here is only a member. You write, you pitch, you film, you negotiate, you "
             "manage a budget — usually in the same week."}}},
    {"tags", {{"Social entrepreneurship, Field research, Pitching, Production"}}},
}};

// --- home.footer -------------------------------------------------------------
inline constexpr std::array<sec::FieldSpec, 7> kFooterFields{{
    {"address_line1", {{"Address, first line"}}, {}, 120, FieldType::Text, false, true},
    {"address_line2", {{"Address, second line"}}, {}, 120, FieldType::Text, false, false},
    {"facebook", {{"Facebook link"}}, {}, 256, FieldType::Url, false, false},
    {"instagram", {{"Instagram link"}}, {}, 256, FieldType::Url, false, false},
    {"linkedin", {{"LinkedIn link"}}, {}, 256, FieldType::Url, false, false},
    {"note", {{"Footer note"}}, {}, 240, FieldType::Text, false, true},
    {"tiktok", {{"TikTok link"}}, {}, 256, FieldType::Url, false, false},
}};
inline constexpr std::array<sec::DefaultField, 7> kFooterDefaults{{
    {"address_line1", {{"Sadat Academy for Management Sciences — Maadi"}}},
    {"address_line2", {{"Cairo, Egypt"}}},
    {"facebook", {{"https://www.facebook.com/EnactusSAMS"}}},
    {"instagram", {{"https://www.instagram.com/enactus.sams"}}},
    {"linkedin", {{"https://www.linkedin.com/company/enactus-sams"}}},
    {"note", {{"Enactus SAMS Maadi is a student chapter of the global Enactus network."}}},
    {"tiktok", {{"https://www.tiktok.com/"}}},
}};

// --- home.hero ---------------------------------------------------------------
inline constexpr std::array<sec::FieldSpec, 15> kHeroFields{{
    {"campus", {{"Campus"}}, {}, 80, FieldType::Text, false, true},
    {"chapter", {{"Chapter"}}, {}, 80, FieldType::Text, false, true},
    {"cta1", {{"Primary button"}}, {}, 40, FieldType::Text, false, true},
    {"cta2", {{"Secondary button"}}, {}, 40, FieldType::Text, false, true},
    {"headline1", {{"Headline, line 1"}}, {}, 24, FieldType::Text, false, true},
    {"headline2", {{"Headline, line 2"}}, {}, 24, FieldType::Text, false, true},
    {"headline3", {{"Headline, line 3"}}, {}, 24, FieldType::Text, false, true},
    {"stat1_label", {{"Statistic 1 label"}}, {}, 40, FieldType::Text, false, true},
    {"stat1_num", {{"Statistic 1 number"}}, {}, 8, FieldType::Text, false, true},
    {"stat2_label", {{"Statistic 2 label"}}, {}, 40, FieldType::Text, false, true},
    {"stat2_num", {{"Statistic 2 number"}}, {}, 8, FieldType::Text, false, true},
    {"stat3_label", {{"Statistic 3 label"}}, {}, 40, FieldType::Text, false, true},
    {"stat3_num", {{"Statistic 3 number"}}, {}, 8, FieldType::Text, false, true},
    {"subtitle", {{"Subtitle"}}, {}, 400, FieldType::Text, false, true},
    {"tagline", {{"Tagline"}}, {}, 80, FieldType::Text, false, true},
}};
inline constexpr std::array<sec::DefaultField, 15> kHeroDefaults{{
    {"campus", {{"Sadat Academy — Maadi"}}},
    {"chapter", {{"Student chapter · Enactus Egypt"}}},
    {"cta1", {{"Join the next season"}}},
    {"cta2", {{"See Tafrah"}}},
    {"headline1", {{"We"}}},
    {"headline2", {{"Change"}}},
    {"headline3", {{"The World"}}},
    {"stat1_label", {{"Teams"}}},
    {"stat1_num", {{"06"}}},
    {"stat2_label", {{"Project at nationals"}}},
    {"stat2_num", {{"01"}}},
    {"stat3_label", {{"Reasons to start"}}},
    {"stat3_num", {{"∞"}}},
    {"subtitle", {{"A student team building real ventures for real problems — six teams, one "
                   "project a year, and a campus that hears about all of it."}}},
    {"tagline", {{"Entrepreneurial action"}}},
}};

// --- home.inside -------------------------------------------------------------
inline constexpr std::array<sec::FieldSpec, 3> kInsideFields{{
    {"desc", {{"Description"}}, {}, 400, FieldType::Text, false, true},
    {"kicker", {{"Kicker"}}, {}, 80, FieldType::Text, false, true},
    {"title", {{"Title"}}, {}, 120, FieldType::Text, false, true},
}};
inline constexpr std::array<sec::DefaultField, 3> kInsideDefaults{{
    {"desc", {{"Every team owns a real part of the outcome. You pick where you start — not "
               "where you stay."}}},
    {"kicker", {{"03 — Inside the club"}}},
    {"title", {{"Six teams. One project."}}},
}};

// --- home.join ---------------------------------------------------------------
inline constexpr std::array<sec::FieldSpec, 11> kJoinFields{{
    {"closed_desc", {{"Closed banner text"}}, {}, 400, FieldType::Text, false, true},
    {"closed_title", {{"Closed banner title"}}, {}, 120, FieldType::Text, false, true},
    {"cta_closed", {{"Button while closed"}}, {}, 40, FieldType::Text, false, true},
    {"cta_open", {{"Button while open"}}, {}, 40, FieldType::Text, false, true},
    {"desc", {{"Description"}}, {}, 600, FieldType::Text, false, true},
    {"kicker_closed", {{"Kicker while closed"}}, {}, 80, FieldType::Text, false, true},
    {"kicker_open", {{"Kicker while open"}}, {}, 80, FieldType::Text, false, true},
    {"note_closed", {{"Note while closed"}}, {}, 200, FieldType::Text, false, true},
    {"note_open", {{"Note while open"}}, {}, 200, FieldType::Text, false, true},
    {"open", {{"Recruitment is open"}}, {}, 0, FieldType::Bool, false, false},
    {"title", {{"Title"}}, {}, 120, FieldType::Text, false, true},
}};
inline constexpr std::array<sec::DefaultField, 11> kJoinDefaults{{
    {"closed_desc", {{"Recruitment for this semester has ended. Follow our socials to know when "
                      "the next season begins!"}}},
    {"closed_title", {{"Applications Closed."}}},
    {"cta_closed", {{"Join the waitlist"}}},
    {"cta_open", {{"Apply now"}}},
    {"desc", {{"Pick the team you want to start in, tell us why, and come to the interview. No "
               "experience required — only the willingness to do the work."}}},
    {"kicker_closed", {{"Recruitment"}}},
    {"kicker_open", {{"Recruitment is open"}}},
    {"note_closed", {{"We open applications at the start of each semester"}}},
    {"note_open", {{"Applications close at the end of the month"}}},
    {"open", {{"true"}}},
    {"title", {{"Join the next season."}}},
}};

// --- home.life ---------------------------------------------------------------
inline constexpr std::array<sec::FieldSpec, 3> kLifeFields{{
    {"kicker", {{"Kicker"}}, {}, 80, FieldType::Text, false, true},
    {"subtitle", {{"Subtitle"}}, {}, 120, FieldType::Text, false, true},
    {"title", {{"Title"}}, {}, 120, FieldType::Text, false, true},
}};
inline constexpr std::array<sec::DefaultField, 3> kLifeDefaults{{
    {"kicker", {{"04 — Life at Enactus"}}},
    {"subtitle", {{"Tap any photo to enlarge"}}},
    {"title", {{"Long days, yellow everywhere."}}},
}};

// --- home.tafrah -------------------------------------------------------------
inline constexpr std::array<sec::FieldSpec, 14> kTafrahFields{{
    {"desc", {{"Description"}}, {}, 600, FieldType::Text, false, true},
    {"footer", {{"Footer line"}}, {}, 300, FieldType::Text, false, true},
    {"h1_desc", {{"Highlight 1 text"}}, {}, 300, FieldType::Text, false, true},
    {"h1_title", {{"Highlight 1 title"}}, {}, 40, FieldType::Text, false, true},
    {"h2_desc", {{"Highlight 2 text"}}, {}, 300, FieldType::Text, false, true},
    {"h2_title", {{"Highlight 2 title"}}, {}, 40, FieldType::Text, false, true},
    {"h3_desc", {{"Highlight 3 text"}}, {}, 300, FieldType::Text, false, true},
    {"h3_title", {{"Highlight 3 title"}}, {}, 40, FieldType::Text, false, true},
    {"h4_desc", {{"Highlight 4 text"}}, {}, 300, FieldType::Text, false, true},
    {"h4_title", {{"Highlight 4 title"}}, {}, 40, FieldType::Text, false, true},
    {"kicker", {{"Kicker"}}, {}, 80, FieldType::Text, false, true},
    {"tagline", {{"Tagline"}}, {}, 80, FieldType::Text, false, true},
    {"title", {{"Title"}}, {}, 80, FieldType::Text, false, true},
    {"visible", {{"Show the Tafrah section"}}, {}, 0, FieldType::Bool, false, false},
}};
inline constexpr std::array<sec::ImageSpec, 1> kTafrahImages{{
    {"site", {{"Platform screenshot"}}, 320, 200, 0, 0},
}};
inline constexpr std::array<sec::DefaultField, 14> kTafrahDefaults{{
    {"desc", {{"A training and employment platform built for autistic people in Egypt: a calm "
               "working environment, direct instruction, and real job opportunities that "
               "protect their rights."}}},
    {"footer", {{"Tafrah was the venture Enactus SAMS Maadi carried to the Enactus Egypt "
                 "national competition this year."}}},
    {"h1_desc", {{"Skill tracks written in plain, literal language with predictable structure "
                  "and no sensory noise."}}},
    {"h1_title", {{"Courses"}}},
    {"h2_desc", {{"Step-by-step guidance through every task, so nobody is left guessing what "
                  "happens next."}}},
    {"h2_title", {{"Assistant"}}},
    {"h3_desc", {{"Vetted employers, clear expectations, and roles matched to how each person "
                  "actually works best."}}},
    {"h3_title", {{"Jobs"}}},
    {"h4_desc", {{"Progress, certificates and readiness in one view — for the trainee and for "
                  "the employer."}}},
    {"h4_title", {{"Dashboard"}}},
    {"kicker", {{"02 — This year's project"}}},
    {"tagline", {{"طفـــرة — the leap"}}},
    {"title", {{"Tafrah"}}},
    {"visible", {{"true"}}},
}};
inline constexpr std::array<sec::DefaultImage, 1> kTafrahDefaultImages{{
    {"site", "tafrah-site.jpg"},
}};

// --- the registry -------------------------------------------------------------
inline constexpr std::array<sec::SectionSpec, 7> kSections{{
    {"home.about", "/", kAboutFields, kNoImages},
    {"home.footer", "/", kFooterFields, kNoImages},
    {"home.hero", "/", kHeroFields, kNoImages},
    {"home.inside", "/", kInsideFields, kNoImages},
    {"home.join", "/", kJoinFields, kNoImages},
    {"home.life", "/", kLifeFields, kNoImages},
    {"home.tafrah", "/", kTafrahFields, kTafrahImages},
}};

static_assert(sec::registry_is_sorted(kSections),
              "kSections must be sorted by key with no duplicates; the lookup is a binary "
              "search");
static_assert(sec::fields_fit_buffers(kSections));
static_assert(sec::choices_are_well_formed(kSections));

inline constexpr std::array<sec::SectionDefaults, kSections.size()> kSectionDefaults{{
    {"home.about", kAboutDefaults, kNoDefaultImages},
    {"home.footer", kFooterDefaults, kNoDefaultImages},
    {"home.hero", kHeroDefaults, kNoDefaultImages},
    {"home.inside", kInsideDefaults, kNoDefaultImages},
    {"home.join", kJoinDefaults, kNoDefaultImages},
    {"home.life", kLifeDefaults, kNoDefaultImages},
    {"home.tafrah", kTafrahDefaults, kTafrahDefaultImages},
}};

static_assert(sec::defaults_match_registry(kSections, kSectionDefaults),
              "a default violates its FieldSpec, is missing a locale, is not valid UTF-8, or a "
              "section has no defaults at all");

inline constexpr std::string_view kSectionsCollection = "sections";

}  // namespace enactus
