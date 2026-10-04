#pragma once

// The account schema: what an application's accounts are made of, declared as a
// table and checked at compile time (docs/01-seams.md §16).
//
// anvil ships the account FLOWS — registration, contact verification, sign-in,
// password reset and change (anvil/accounts/service.h) — because every
// application needs them and every application gets part of them wrong the
// first time. What differs between applications is only this:
//
//   * which identifiers an account has — an email, a username, a phone — which
//     of them registration requires, and which may be used to sign in;
//   * which of them receives a code (the CONTACT), and whether an account must
//     prove it before it can sign in;
//   * what else a person is asked for — a given name, a family name — as text
//     fields with code-point bounds.
//
// The same table is published in the client descriptor, so a generated client
// renders exactly these fields and sends exactly these keys, and nothing about
// an account has to be written twice.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/identity/login_identity.h"
#include "anvil/input/fields.h"

namespace anvil::accounts {

using identity::LoginIdentity;

struct IdentifierSpec final {
    LoginIdentity kind;
    // Must registration carry it.
    bool          required;
    // May a person sign in, and ask for a reset, with it.
    bool          sign_in;
};

// A profile field: text, bounded in code points, stored under
// identity::fields::kProfile. Its key is the application's and appears in the
// descriptor, in the request body and in the stored document; the words for its
// label are the client application's, per locale, and are not here.
struct ProfileFieldSpec final {
    std::string_view key;
    input::TextRules rules;
    bool             required;
};

enum class Activation : std::uint8_t {
    // Registration writes PendingVerification and sends a code to the contact;
    // the account cannot sign in until the code is presented.
    AfterVerification = 0,
    // Registration writes Active. For an application that verifies out of band,
    // or not at all — a decision with consequences it owns.
    Immediate = 1,
};

// Where the password is hashed (docs/05-auth-sessions.md §8 and §12).
//
// Client is first and is the default: the password never leaves the device,
// and a sign-in costs the server one HMAC instead of ~100 ms and 64 MiB.
// Server is the opt-out, for an application whose clients cannot run Argon2.
enum class Hashing : std::uint8_t {
    Client = 0,
    Server = 1,
};

struct AccountSchema final {
    std::span<const IdentifierSpec>   identifiers;
    std::span<const ProfileFieldSpec> profile;
    // Receives verification and reset codes, so it must be an email or a phone,
    // declared and required.
    LoginIdentity                     contact = LoginIdentity::Email;
    Activation                        activation = Activation::AfterVerification;
};

// What a route does in the account flows. The application declares the routes
// themselves — paths are always the application's (docs/01-seams.md §3) — and
// says which of its route ids plays which role.
enum class AccountRole : std::uint8_t {
    Salt,
    Register,
    Verify,
    Resend,
    SignIn,
    ResetRequest,
    ResetConfirm,
    Change,
    Refresh,
    SignOut,
};

inline constexpr std::size_t kAccountRoleCount = 10;

// How many digits a verification or reset code has, published so a client can
// shape the box it is typed into. The code itself is minted by
// identity/verification.h, which cannot be included here without the database
// driver; src/identity/verification.cc asserts the two are one number.
inline constexpr std::size_t kAccountCodeDigits = 6;

[[nodiscard]] constexpr std::string_view role_name(AccountRole role) noexcept {
    switch (role) {
        case AccountRole::Salt:         return "salt";
        case AccountRole::Register:     return "register";
        case AccountRole::Verify:       return "verify";
        case AccountRole::Resend:       return "resend";
        case AccountRole::SignIn:       return "sign_in";
        case AccountRole::ResetRequest: return "reset_request";
        case AccountRole::ResetConfirm: return "reset_confirm";
        case AccountRole::Change:       return "change";
        case AccountRole::Refresh:      return "refresh";
        case AccountRole::SignOut:      return "sign_out";
    }
    return "salt";
}

struct AccountRoute final {
    AccountRole      role;
    std::string_view route_id;  // a RouteDescription::id
};

// Everything the descriptor publishes about accounts, and everything the
// service is constructed against — one value, so the two cannot disagree.
struct AccountDescription final {
    const AccountSchema*           schema;
    Hashing                        hashing = Hashing::Client;
    std::span<const AccountRoute>  routes;
};

namespace detail {

[[nodiscard]] constexpr bool is_key_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// The keys a register body already uses. A profile field spelled like one would
// be ambiguous in the body and in a field reason.
inline constexpr std::array<std::string_view, 9> kReservedKeys{
    "email", "username", "phone", "password", "credential", "locale", "code", "identifier",
    "profile"};

}  // namespace detail

// Refused, each for the reason given:
//   no identifier may sign in           — nobody could ever use an account;
//   a kind declared twice               — which one's `required` applies?
//   a contact that is not an email or a phone, or not declared and required
//                                       — a code would have nowhere to go;
//   a profile key empty, over 32 characters, outside [a-z0-9_], reserved, or
//   declared twice                      — it is a stored key and a wire key;
//   profile bounds with min > max or max == 0.
[[nodiscard]] constexpr bool account_schema_is_well_formed(const AccountSchema& schema) noexcept {
    bool any_sign_in = false;
    bool contact_ok = false;
    for (std::size_t i = 0; i < schema.identifiers.size(); ++i) {
        const IdentifierSpec& spec = schema.identifiers[i];
        for (std::size_t j = 0; j < i; ++j) {
            if (schema.identifiers[j].kind == spec.kind) { return false; }
        }
        any_sign_in = any_sign_in || spec.sign_in;
        if (spec.kind == schema.contact && spec.required) { contact_ok = true; }
    }
    if (!any_sign_in || !contact_ok) { return false; }
    if (schema.contact != LoginIdentity::Email && schema.contact != LoginIdentity::Phone) {
        return false;
    }

    for (std::size_t i = 0; i < schema.profile.size(); ++i) {
        const ProfileFieldSpec& field = schema.profile[i];
        if (field.key.empty() || field.key.size() > 32) { return false; }
        for (const char c : field.key) {
            if (!detail::is_key_char(c)) { return false; }
        }
        for (const std::string_view reserved : detail::kReservedKeys) {
            if (field.key == reserved) { return false; }
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (schema.profile[j].key == field.key) { return false; }
        }
        if (field.rules.max_code_points == 0 ||
            field.rules.min_code_points > field.rules.max_code_points) {
            return false;
        }
    }
    return true;
}

// The description as a whole: a well-formed schema, each role at most once, a
// sign-in route, and a salt route exactly when the client hashes — without one
// a client cannot derive anything, and with server hashing it would answer a
// question nobody asks.
//
// Declare the schema `constexpr` at namespace scope and NOT `inline constexpr`.
// GCC 16 under -fsanitize=null does not fold the null check below on the address
// of an external-linkage inline variable, so a `static_assert` on this function
// stops compiling in exactly the sanitiser build that is supposed to run it.
[[nodiscard]] constexpr bool account_description_is_well_formed(
    const AccountDescription& description) noexcept {
    if (description.schema == nullptr || !account_schema_is_well_formed(*description.schema)) {
        return false;
    }
    bool sign_in = false;
    bool salt = false;
    for (std::size_t i = 0; i < description.routes.size(); ++i) {
        const AccountRoute& route = description.routes[i];
        if (route.route_id.empty()) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (description.routes[j].role == route.role) { return false; }
        }
        sign_in = sign_in || route.role == AccountRole::SignIn;
        salt = salt || route.role == AccountRole::Salt;
    }
    return sign_in && (salt == (description.hashing == Hashing::Client));
}

[[nodiscard]] constexpr const IdentifierSpec* find_identifier(const AccountSchema& schema,
                                                              LoginIdentity kind) noexcept {
    for (const IdentifierSpec& spec : schema.identifiers) {
        if (spec.kind == kind) { return &spec; }
    }
    return nullptr;
}

}  // namespace anvil::accounts
