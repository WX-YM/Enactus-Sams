#pragma once

// Content that repeats (anvil docs/20-entries.md): gallery photos, teams and
// their rosters, and recruitment applications.
//
// Each kind's shape is a section shape, so every field is bound, checked and
// stored by the sections code. A key, flag or field not declared here cannot
// be written.

#include <array>
#include <string_view>

#include "anvil/entries/registry.h"
#include "anvil/sections/defaults.h"
#include "anvil/sections/registry.h"

namespace enactus {

namespace ent = anvil::entries;
namespace esec = anvil::sections;

// --- application ------------------------------------------------------------
//
// One per applicant. The slug is a digest of the normalised email
// (`application_slug`), so the unique slug index is what enforces "one
// application per email" — a race between two submissions is two inserts and
// one duplicate-key error, never two rows.
inline constexpr std::array<std::string_view, 5> kApplicationStatuses{{
    "accepted", "interview_scheduled", "pending", "referred", "rejected",
}};

inline constexpr std::array<esec::FieldSpec, 9> kApplicationFields{{
    {"decision", {{"Decision note"}}, {}, 2000, esec::FieldType::Text, false, false},
    {"email", {{"Email"}}, {}, 254, esec::FieldType::Text, false, true},
    {"first_name", {{"First name"}}, {}, 80, esec::FieldType::Text, false, true},
    {"last_name", {{"Last name"}}, {}, 80, esec::FieldType::Text, false, true},
    {"phone", {{"Phone"}}, {}, 32, esec::FieldType::Text, false, true},
    {"reason", {{"Why they want to join"}}, {}, 3000, esec::FieldType::Text, false, false},
    {"referred_to", {{"Referred to team"}}, {}, 80, esec::FieldType::Text, false, false},
    {"status", {{"Status"}}, kApplicationStatuses, 24, esec::FieldType::Choice, false, true},
    {"team", {{"Team"}}, {}, 80, esec::FieldType::Text, false, true},
}};

// --- gallery ----------------------------------------------------------------
inline constexpr std::array<esec::FieldSpec, 1> kPhotoFields{{
    {"caption", {{"Caption (screen readers)"}}, {}, 200, esec::FieldType::Text, false, false},
}};
inline constexpr std::array<esec::ImageSpec, 1> kPhotoImages{{
    {"photo", {{"Photo"}}, 200, 200, 0, 0},
}};

// --- team -------------------------------------------------------------------
//
// `recruiting` puts the team among the choices on the application form;
// `showcase` puts its card in "Inside the club". The two are independent, which
// is what the old backend's separate `recruitmentTeams` and `insideTeams` lists
// were approximating.
inline constexpr std::array<esec::FieldSpec, 2> kTeamFields{{
    {"desc", {{"Description"}}, {}, 600, esec::FieldType::Text, false, false},
    {"name", {{"Name"}}, {}, 80, esec::FieldType::Text, false, true},
}};
inline constexpr std::array<ent::FlagSpec, 2> kTeamFlags{{
    {"recruiting", {{"Open for applications"}}},
    {"showcase", {{"Shown in Inside the club"}}},
}};

inline constexpr std::array<esec::FieldSpec, 2> kMemberFields{{
    {"name", {{"Name"}}, {}, 120, esec::FieldType::Text, false, true},
    {"role", {{"Role"}}, {}, 60, esec::FieldType::Text, false, true},
}};

inline constexpr std::array<ent::KindSpec, 6> kKinds{{
    {{"application", "/", kApplicationFields, {}}, {}, "", 200000, ent::Workflow::Immediate,
     ent::Ordering::Newest, ent::SlugRule::Unique},
    {{"gallery.about", "/", kPhotoFields, kPhotoImages}, {}, "", 24, ent::Workflow::Immediate,
     ent::Ordering::Manual, ent::SlugRule::None},
    {{"gallery.life", "/", kPhotoFields, kPhotoImages}, {}, "", 120, ent::Workflow::Immediate,
     ent::Ordering::Manual, ent::SlugRule::None},
    {{"gallery.tafrah", "/", kPhotoFields, kPhotoImages}, {}, "", 60, ent::Workflow::Immediate,
     ent::Ordering::Manual, ent::SlugRule::None},
    {{"team", "/", kTeamFields, {}}, kTeamFlags, "", 60, ent::Workflow::Immediate,
     ent::Ordering::Manual, ent::SlugRule::Unique},
    {{"team.member", "/", kMemberFields, {}}, {}, "team", 400, ent::Workflow::Immediate,
     ent::Ordering::Oldest, ent::SlugRule::None},
}};

static_assert(ent::kinds_are_well_formed(kKinds),
              "kinds unsorted, a malformed key or flag, a shape that fails the section checks, "
              "a parent that is not declared or loops, or a capacity that is zero or too large "
              "to reorder");

inline constexpr std::string_view kApplicationKind = "application";
inline constexpr std::string_view kTeamKind = "team";
inline constexpr std::string_view kMemberKind = "team.member";
inline constexpr std::array<std::string_view, 3> kGalleryKinds{{
    "gallery.about", "gallery.life", "gallery.tafrah",
}};

// --- seeds: what a fresh database shows -------------------------------------
//
// The photos and teams the website shipped with. Seeded once, ever, per kind
// (anvil docs/20-entries.md §7), so a photo staff delete stays deleted.
inline constexpr std::array<esec::DefaultField, 1> kNoCaption{{{"caption", {{"Life at Enactus"}}}}};

#define ENACTUS_PHOTO(slot_file) \
    std::array<esec::DefaultImage, 1> { { { "photo", slot_file } } }

inline constexpr auto kPhotoBench = ENACTUS_PHOTO("bench.jpg");
inline constexpr auto kPhotoCertificate = ENACTUS_PHOTO("certificate.jpg");
inline constexpr auto kPhotoGlasses = ENACTUS_PHOTO("glasses.jpg");
inline constexpr auto kPhotoBanner = ENACTUS_PHOTO("banner.jpg");
inline constexpr auto kPhotoFlag = ENACTUS_PHOTO("flag.jpg");
inline constexpr auto kPhotoPortraitA = ENACTUS_PHOTO("portrait-a.jpg");
inline constexpr auto kPhotoThinking = ENACTUS_PHOTO("thinking.jpg");
inline constexpr auto kPhotoPortraitB = ENACTUS_PHOTO("portrait-b.jpg");
inline constexpr auto kPhotoSeatedLaptop = ENACTUS_PHOTO("seated-laptop.jpg");
inline constexpr auto kPhotoBazaarTable = ENACTUS_PHOTO("bazaar-table.jpg");
inline constexpr auto kPhotoCaricatureDraw = ENACTUS_PHOTO("caricature-draw.jpg");
inline constexpr auto kPhotoCaricaturePose = ENACTUS_PHOTO("caricature-pose.jpg");
inline constexpr auto kPhotoBoothSmile = ENACTUS_PHOTO("booth-smile.jpg");
inline constexpr auto kPhotoGroupHero = ENACTUS_PHOTO("group-hero.jpg");
inline constexpr auto kPhotoTafrahSet = ENACTUS_PHOTO("tafrah-set.jpg");
inline constexpr auto kPhotoTafrahMonitor = ENACTUS_PHOTO("tafrah-monitor.jpg");
inline constexpr auto kPhotoNotebook = ENACTUS_PHOTO("notebook.jpg");
inline constexpr auto kPhotoLaptopTeam = ENACTUS_PHOTO("laptop-team.jpg");
inline constexpr auto kPhotoMediaTripod = ENACTUS_PHOTO("media-tripod.jpg");

#undef ENACTUS_PHOTO

inline constexpr std::array<ent::EntrySeed, 3> kAboutSeeds{{
    {"", {"gallery.about", kNoCaption, kPhotoBench}, 0},
    {"", {"gallery.about", kNoCaption, kPhotoCertificate}, 0},
    {"", {"gallery.about", kNoCaption, kPhotoGlasses}, 0},
}};

inline constexpr std::array<ent::EntrySeed, 11> kLifeSeeds{{
    {"", {"gallery.life", kNoCaption, kPhotoBanner}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoFlag}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoPortraitA}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoThinking}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoPortraitB}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoSeatedLaptop}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoBazaarTable}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoCaricatureDraw}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoCaricaturePose}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoBoothSmile}, 0},
    {"", {"gallery.life", kNoCaption, kPhotoGroupHero}, 0},
}};

inline constexpr std::array<ent::EntrySeed, 5> kTafrahSeeds{{
    {"", {"gallery.tafrah", kNoCaption, kPhotoTafrahSet}, 0},
    {"", {"gallery.tafrah", kNoCaption, kPhotoTafrahMonitor}, 0},
    {"", {"gallery.tafrah", kNoCaption, kPhotoNotebook}, 0},
    {"", {"gallery.tafrah", kNoCaption, kPhotoLaptopTeam}, 0},
    {"", {"gallery.tafrah", kNoCaption, kPhotoMediaTripod}, 0},
}};

inline constexpr std::array<esec::DefaultImage, 0> kNoSeedImages{};

inline constexpr std::array<esec::DefaultField, 2> kTeamPresentation{{
    {"desc", {{"Turns a year of work into seventeen minutes on stage: script, structure, "
               "delivery, and surviving the judges' questions."}}},
    {"name", {{"Presentation"}}},
}};
inline constexpr std::array<esec::DefaultField, 2> kTeamProjectManagement{{
    {"desc", {{"Owns scope, timelines and field execution — and proves the social and "
               "financial results with real numbers."}}},
    {"name", {{"Project Management"}}},
}};
inline constexpr std::array<esec::DefaultField, 2> kTeamHumanResources{{
    {"desc", {{"Recruitment, onboarding, training calendar and culture. The reason the club "
               "still feels like a club in month nine."}}},
    {"name", {{"Human Resources"}}},
}};
inline constexpr std::array<esec::DefaultField, 2> kTeamExternalAffairs{{
    {"desc", {{"Sponsors, NGOs, companies and the academy itself — the partnerships that make a "
               "student project possible."}}},
    {"name", {{"External Affairs"}}},
}};
inline constexpr std::array<esec::DefaultField, 2> kTeamSocialMedia{{
    {"desc", {{"Calendar, copy, community and campaign launches. If the campus knows about it, "
               "this team made sure of it."}}},
    {"name", {{"Social Media"}}},
}};
inline constexpr std::array<esec::DefaultField, 2> kTeamMediaProduction{{
    {"desc", {{"Photo, video, editing and design — the visual record of every session, "
               "activation and project film."}}},
    {"name", {{"Media Production"}}},
}};

// Both flags set: bit 0 is `recruiting`, bit 1 is `showcase`.
inline constexpr ent::FlagSet kTeamSeedFlags = 0b11;

inline constexpr std::array<ent::EntrySeed, 6> kTeamSeeds{{
    {"presentation", {"team", kTeamPresentation, kNoSeedImages}, kTeamSeedFlags},
    {"project-management", {"team", kTeamProjectManagement, kNoSeedImages}, kTeamSeedFlags},
    {"human-resources", {"team", kTeamHumanResources, kNoSeedImages}, kTeamSeedFlags},
    {"external-affairs", {"team", kTeamExternalAffairs, kNoSeedImages}, kTeamSeedFlags},
    {"social-media", {"team", kTeamSocialMedia, kNoSeedImages}, kTeamSeedFlags},
    {"media-production", {"team", kTeamMediaProduction, kNoSeedImages}, kTeamSeedFlags},
}};

inline constexpr std::array<ent::KindSeeds, 4> kSeeds{{
    {"gallery.about", kAboutSeeds},
    {"gallery.life", kLifeSeeds},
    {"gallery.tafrah", kTafrahSeeds},
    {"team", kTeamSeeds},
}};

static_assert(ent::seeds_match_kinds(kKinds, kSeeds),
              "a seed names an undeclared or child kind, breaks its slug rule, sets an "
              "undeclared flag, overfills the kind, or fails the kind's own field checks");

inline constexpr std::string_view kEntriesCollection = "entries";

}  // namespace enactus
