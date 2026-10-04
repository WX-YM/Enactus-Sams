#pragma once

// The account flows (anvil docs/05-auth-sessions.md §13, docs/01-seams.md §16).
//
// Staff accounts only. Nobody registers themselves, resets by email, or
// verifies an address: an account is created by someone holding the `users`
// permission, so the only roles declared are salt, sign-in, refresh, sign-out
// and password change. A role that is not declared has no route at all.
//
// Client hashing (the anvil default): the browser derives the credential with
// Argon2id and the server stores a keyed stage over it, so no password ever
// reaches this process or any proxy in front of it.

#include <array>

#include "anvil/accounts/schema.h"

namespace enactus {

namespace acc = anvil::accounts;
using anvil::identity::LoginIdentity;

inline constexpr std::array<acc::IdentifierSpec, 1> kAccountIdentifiers{{
    {LoginIdentity::Email, true, true},
}};

inline constexpr std::array<acc::ProfileFieldSpec, 0> kAccountProfile{};

// Declared `constexpr` at namespace scope and NOT `inline constexpr`; see
// account_description_is_well_formed.
constexpr acc::AccountSchema kAccountSchema{
    kAccountIdentifiers, kAccountProfile, LoginIdentity::Email, acc::Activation::Immediate};

static_assert(acc::account_schema_is_well_formed(kAccountSchema));

inline constexpr std::array<acc::AccountRoute, 5> kAccountRoutes{{
    {acc::AccountRole::Salt, "auth.salt"},
    {acc::AccountRole::SignIn, "auth.login"},
    {acc::AccountRole::Change, "auth.password"},
    {acc::AccountRole::Refresh, "auth.refresh"},
    {acc::AccountRole::SignOut, "auth.logout"},
}};

constexpr acc::AccountDescription kAccounts{&kAccountSchema, acc::Hashing::Client,
                                            kAccountRoutes};

static_assert(acc::account_description_is_well_formed(kAccounts));

}  // namespace enactus
