#pragma once

// The permission bits of this application.
//
// Bit indices are stored in access tokens AND in every account row
// (anvil docs/04-access-control.md §1). They are APPEND-ONLY: never renumber,
// never reuse a retired bit — renumbering silently regrants access for every
// token in flight.
//
// The first seven are the panel permissions staff see in Access Control. The
// group from 16 up are IMPLIED bits: never granted by hand, derived at write
// time from the panel bits (`with_implied`), because an anvil route mask is
// "holds ALL of these" and several routes are legitimately reachable from more
// than one panel permission.

#include <array>
#include <cstdint>

#include "anvil/core/perm_catalogue.h"
#include "anvil/core/perm_set.h"

namespace enactus {

enum class Perm : std::uint8_t {
    Dashboard    = 0,
    Applications = 1,
    FormMaker    = 2,
    Teams        = 3,
    Content      = 4,
    Gallery      = 5,
    Users        = 6,

    // Implied — see `with_implied`.
    MediaUpload = 16,  // Content | Gallery
    FormRead    = 17,  // Applications | FormMaker
};

inline constexpr std::array<anvil::PermName, 9> kPermNameTable{{
    {"dashboard",          static_cast<std::uint8_t>(Perm::Dashboard)},
    {"applications",       static_cast<std::uint8_t>(Perm::Applications)},
    {"form_maker",         static_cast<std::uint8_t>(Perm::FormMaker)},
    {"teams",              static_cast<std::uint8_t>(Perm::Teams)},
    {"content",            static_cast<std::uint8_t>(Perm::Content)},
    {"gallery",            static_cast<std::uint8_t>(Perm::Gallery)},
    {"users",              static_cast<std::uint8_t>(Perm::Users)},
    {"media_upload",       static_cast<std::uint8_t>(Perm::MediaUpload)},
    {"form_read",          static_cast<std::uint8_t>(Perm::FormRead)},
}};

inline constexpr anvil::PermCatalogue kPerms{kPermNameTable};

static_assert(kPerms.well_formed(),
              "duplicate bit, duplicate name, empty name, or a bit outside PermSet");
static_assert(kPerms.size() == kPermNameTable.size());

// The bits a person may grant in Access Control. Everything else is derived.
inline constexpr anvil::PermSet kGrantable =
    anvil::perm_mask(Perm::Dashboard, Perm::Applications, Perm::FormMaker, Perm::Teams,
                     Perm::Content, Perm::Gallery, Perm::Users);

[[nodiscard]] constexpr bool holds(const anvil::PermSet& set, Perm bit) noexcept {
    return set.test(static_cast<std::size_t>(bit));
}

// Granted bits plus everything they imply. Applied on every write of a
// permission set, so the stored effective mask is always closed under it.
[[nodiscard]] constexpr anvil::PermSet with_implied(anvil::PermSet granted) noexcept {
    anvil::PermSet out = granted;
    if (holds(granted, Perm::Content) || holds(granted, Perm::Gallery)) {
        out = out | anvil::perm_mask(Perm::MediaUpload);
    }
    if (holds(granted, Perm::Applications) || holds(granted, Perm::FormMaker)) {
        out = out | anvil::perm_mask(Perm::FormRead);
    }
    return out;
}

static_assert(holds(with_implied(anvil::perm_mask(Perm::Gallery)), Perm::MediaUpload));
static_assert(!holds(with_implied(anvil::perm_mask(Perm::Teams)), Perm::MediaUpload));

}  // namespace enactus
