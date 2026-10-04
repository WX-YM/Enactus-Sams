#pragma once

// The reference application's accounts (docs/01-seams.md §16): what an account
// is made of, and which of this application's routes play which role in the
// built-in account flows.
//
// An account has an email — required, the contact codes are sent to — and a
// username, both usable to sign in, plus an optional phone. Registration asks
// for a given name and, optionally, a family name. The table is all an
// application writes; the flows are anvil's, and the client is generated from
// the descriptor this table is published in.

#include <array>

#include "anvil/accounts/schema.h"
#include "anvil/input/fields.h"

namespace testapp {

namespace acc = anvil::accounts;
using anvil::identity::LoginIdentity;

inline constexpr std::array<acc::IdentifierSpec, 3> kAccountIdentifiers{{
    {LoginIdentity::Email, true, true},
    // Required because this catalogue's username index is unique and NOT
    // partial (indexes.h). An application that makes it optional declares the
    // index partial, as the phone's already is.
    {LoginIdentity::Username, true, true},
    {LoginIdentity::Phone, false, true},
}};

inline constexpr std::array<acc::ProfileFieldSpec, 2> kAccountProfile{{
    {"given_name", anvil::input::kPersonNameRules, true},
    {"family_name", anvil::input::kPersonNameRules, false},
}};

// `constexpr` and not `inline constexpr`, here and on kAccounts below. GCC 16
// under -fsanitize=null does not fold a null check on the address of an
// external-linkage inline variable, so `account_description_is_well_formed`
// stops being a constant expression. Internal linkage costs one copy of a
// small table per translation unit and nothing compares the addresses, and
// kAccounts has to follow it: an inline variable holding the address of an
// internal one has a different value in every unit, which is an ODR violation.
constexpr acc::AccountSchema kAccountSchema{
    kAccountIdentifiers, kAccountProfile, LoginIdentity::Email,
    acc::Activation::AfterVerification};

static_assert(acc::account_schema_is_well_formed(kAccountSchema));

// Role → route id, every id declared in route_descriptions.h.
inline constexpr std::array<acc::AccountRoute, 10> kAccountRoutes{{
    {acc::AccountRole::Salt, "auth.prehash"},
    {acc::AccountRole::Register, "auth.signup"},
    {acc::AccountRole::Verify, "auth.verify"},
    {acc::AccountRole::Resend, "auth.resend"},
    {acc::AccountRole::SignIn, "auth.login"},
    {acc::AccountRole::ResetRequest, "auth.reset"},
    {acc::AccountRole::ResetConfirm, "auth.reset_confirm"},
    {acc::AccountRole::Change, "auth.password"},
    {acc::AccountRole::Refresh, "auth.refresh"},
    {acc::AccountRole::SignOut, "auth.logout"},
}};

// Client hashing: the default, and the reason no route here takes a password.
constexpr acc::AccountDescription kAccounts{&kAccountSchema, acc::Hashing::Client,
                                                   kAccountRoutes};

static_assert(acc::account_description_is_well_formed(kAccounts));

}  // namespace testapp
