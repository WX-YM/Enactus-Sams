#pragma once

// The identifiers an account can be reached by — email, username, phone — and
// the ONE canonical form each is looked up under.
//
// A login, a registration, a salt answer and a reset all turn what a person
// typed into a lookup key, and they must turn it into the same one. Two
// canonicalisers that disagree by a case fold are an account its owner can
// reach only by typing their address the way they happened to register it — so
// there is exactly one, here, and every flow in anvil/accounts calls it.
//
// The display spelling is kept separately by the caller (identity::NewUser) and
// is never what anything is compared against.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "anvil/identity/login_identity.h"
#include "anvil/input/fields.h"

namespace anvil::accounts {

using identity::LoginIdentity;

// The wire and descriptor spelling of a kind: "email", "username", "phone".
[[nodiscard]] constexpr std::string_view identifier_name(LoginIdentity kind) noexcept {
    switch (kind) {
        case LoginIdentity::Email:    return "email";
        case LoginIdentity::Username: return "username";
        case LoginIdentity::Phone:    return "phone";
    }
    return "email";
}

// Username bounds, in code points (CLAUDE.md §8). Three so a name is a name and
// not an initial; thirty-two so it fits a header and a mention.
inline constexpr std::size_t kUsernameMinCodePoints = 3;
inline constexpr std::size_t kUsernameMaxCodePoints = 32;

// Which kind a single `identifier` field is, decided by its shape so that a
// sign-in form needs one box rather than three: an "@" is an email, a leading
// "+" is a phone, anything else is a username. The shapes cannot overlap
// because a username may contain neither (canonicalise refuses both).
[[nodiscard]] LoginIdentity kind_by_shape(std::string_view raw) noexcept;

// The lookup key for `raw` as a `kind`, or the reason it is not one.
//
//   email     NFKC case-fold, then input::check_email. Case-folding the local
//             part too is a choice, stated: RFC 5321 lets a mailbox be
//             case-sensitive, no mail system anyone registers with is, and two
//             accounts differing only in case are a phishing surface.
//   username  NFKC case-fold, 3..32 code points of the Identifier text class,
//             no "@" and no leading "+" — which is what keeps kind_by_shape
//             unambiguous.
//   phone     E.164: "+" and 8..15 digits, the first not zero, after removing
//             the spaces, hyphens, dots and parentheses people type.
//
// `out` is written only on Reason::Ok.
[[nodiscard]] input::Reason canonicalise(LoginIdentity kind, std::string_view raw,
                                         std::string& out);

}  // namespace anvil::accounts
