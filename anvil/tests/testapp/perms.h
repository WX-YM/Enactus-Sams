#pragma once

// The reference application's permission vocabulary.
//
// This is what every application built on anvil writes, and it is compiled by
// every build of the test suite — so the worked example in docs/01-seams.md §1 is
// a file that must keep compiling rather than a snippet that can rot.
//
// The gaps between blocks are deliberate and are exercised on purpose: a real
// application leaves room to grow a block without renumbering, and anvil has to
// answer "what is bit 5?" with nothing rather than with a guess.

#include <array>
#include <cstdint>

#include "anvil/core/perm_catalogue.h"

namespace testapp {

// Bit indices are stored in access tokens AND in the user row.
// NEVER renumber. Retired bits are reserved, never reused — renumbering silently
// regrants access for every token currently in flight.
enum class Perm : std::uint8_t {
    ContentRead   = 0,
    ContentWrite  = 1,
    ContentDelete = 2,

    MediaUpload   = 8,
    MediaDelete   = 9,

    FormCreate    = 16,
    FormRead      = 17,
    // Reading who submitted a form and reading the identity they submitted are
    // separate authorities: the desk needs the list, and only a supervisor should
    // be able to unseal an identity number.
    FormPii       = 18,

    StaffManage   = 24,
    // Reading the security log, deliberately NOT folded into StaffManage: the
    // person who investigates an incident should not need the authority to cause
    // one.
    AuditRead     = 25,

    // Conversations (docs/22-chat.md). Taking part in one needs no bit beyond
    // being signed in: membership is what decides. Creating a group or a
    // channel is an account-level authority, held apart so a product can let
    // everybody message and only some people broadcast.
    ChatCreateGroup   = 32,
    ChatCreateChannel = 33,
    // Reading a conversation one is not in, and the reports members file
    // (docs/22-chat.md §9.2): staff, and audited on every read.
    ChatReview        = 34,

    // The highest bit any test declares, and the last word of the 128-bit set.
    // Present so the wire format's high word is exercised rather than assumed.
    SystemAnnounce = 120,
};

inline constexpr std::array<anvil::PermName, 14> kPermNameTable{{
    {"ContentRead",    static_cast<std::uint8_t>(Perm::ContentRead)},
    {"ContentWrite",   static_cast<std::uint8_t>(Perm::ContentWrite)},
    {"ContentDelete",  static_cast<std::uint8_t>(Perm::ContentDelete)},
    {"MediaUpload",    static_cast<std::uint8_t>(Perm::MediaUpload)},
    {"MediaDelete",    static_cast<std::uint8_t>(Perm::MediaDelete)},
    {"FormCreate",     static_cast<std::uint8_t>(Perm::FormCreate)},
    {"FormRead",       static_cast<std::uint8_t>(Perm::FormRead)},
    {"FormPii",        static_cast<std::uint8_t>(Perm::FormPii)},
    {"StaffManage",    static_cast<std::uint8_t>(Perm::StaffManage)},
    {"AuditRead",      static_cast<std::uint8_t>(Perm::AuditRead)},
    {"ChatCreateGroup",   static_cast<std::uint8_t>(Perm::ChatCreateGroup)},
    {"ChatCreateChannel", static_cast<std::uint8_t>(Perm::ChatCreateChannel)},
    {"ChatReview",        static_cast<std::uint8_t>(Perm::ChatReview)},
    {"SystemAnnounce", static_cast<std::uint8_t>(Perm::SystemAnnounce)},
}};

inline constexpr anvil::PermCatalogue kPerms{kPermNameTable};

static_assert(kPerms.well_formed(),
              "duplicate bit, duplicate name, empty name, or a bit outside PermSet");
static_assert(kPerms.size() == kPermNameTable.size(),
              "adding a permission without naming it is a build failure, not a bit the "
              "dashboard receives and cannot render");

// The compile-time route masks an application builds. Here to prove they ARE
// compile-time: if perm_mask ever stopped being usable in a constant expression,
// this is where it would fail.
inline constexpr anvil::PermSet kContentAuthor =
    anvil::perm_mask(Perm::ContentRead, Perm::ContentWrite);
inline constexpr anvil::PermSet kFormSupervisor =
    anvil::perm_mask(Perm::FormRead, Perm::FormPii);

static_assert(kContentAuthor.count() == 2);
static_assert(kFormSupervisor.test(static_cast<std::size_t>(Perm::FormPii)));
static_assert(!kContentAuthor.test(static_cast<std::size_t>(Perm::ContentDelete)),
              "authoring must not imply deleting");

}  // namespace testapp
