#pragma once

// The field names anvil writes into the users collection.
//
// Published rather than kept private to the repository because the INDEX
// CATALOGUE is the application's (docs/01-seams.md §4, anvil/db/migrations.h).
// An application has to be able to declare `{em: 1}` unique without guessing
// what anvil spelled the column, and a guess that is wrong produces an index
// the planner never uses and a COLLSCAN on the login path — which CI catches,
// late, instead of the compiler catching it here.
//
// --- what anvil owns, and what it does not ---------------------------------
//
// anvil owns exactly the fields below: the ones that decide whether a
// credential may in, and what it may do once it is. It does NOT own the
// application's profile — no display name, no job title, no address, no
// preferences beyond the locale the access token has to carry anyway.
//
// That is a namespace agreement rather than a partition of the document. Every
// write here is a targeted `$set` of anvil's own keys and never a whole-document
// replace, so an application is free to keep its own fields in the same row and
// read them with its own projection. Two collections joined on a user id would
// cost a second round trip on every screen that renders a person; one document
// with two owners costs nothing, provided neither writer replaces it wholesale.
// Nothing here ever does.
//
// The names are short for the reason every stored key is: a twelve-character
// key is twelve bytes on every document and in every index entry that carries
// it, to spell something the code already knows (ENGINEERING_RULES.md §2.3).

#include <string_view>

namespace anvil::identity::fields {

inline constexpr std::string_view kId = "_id";

// The LOOKUP keys and the DISPLAY spellings, kept apart on purpose. A login
// matches the normalised form — case-folded, NFC, and with whatever else the
// application's normaliser does — and the display form is what the person
// typed. Storing one and re-deriving the other means either logins that fail on
// a capital letter or a screen that renders an address in a spelling nobody
// wrote.
inline constexpr std::string_view kEmailNormalised = "em";
inline constexpr std::string_view kEmailDisplay = "emd";
inline constexpr std::string_view kUsernameNormalised = "un";
inline constexpr std::string_view kUsernameDisplay = "und";

// E.164, and ABSENT rather than empty when the account has none. A unique index
// treats a missing field as null, so without a partial filter on
// `{ph: {$exists: true}}` the first phoneless account would lock out every
// other one — and an empty string is a value every phoneless account shares,
// which defeats the partial filter just as thoroughly.
inline constexpr std::string_view kPhone = "ph";

// The argon2id encoded hash, parameters and all. Read by exactly one projection.
inline constexpr std::string_view kPasswordHash = "pw";

inline constexpr std::string_view kUserType = "ut";
inline constexpr std::string_view kStatus = "st";

// The DIRECT grants, and the denormalised union of those with whatever role
// masks the application resolved at write time.
//
// Two fields because they answer different questions and only one of them is on
// the hot path. `eff` is what the token is minted from and what every
// authorisation check reads, so it is one 16-byte load; `perms` is what an
// administrator edited and what the grid renders back. Deriving `eff` per
// request would be a second query and a set union on every protected call.
inline constexpr std::string_view kDirectPerms = "perms";
inline constexpr std::string_view kEffectivePerms = "eff";

// The revocation channel. Monotonic, starts at 1 so "never bumped" and "no
// epoch recorded" stay distinguishable in a token.
inline constexpr std::string_view kPermEpoch = "pe";

// Consecutive failed authentications, and the instant the account stops
// refusing them. The BACKOFF POLICY is the application's — anvil records the
// count and stores whatever lock instant the caller computed from it.
inline constexpr std::string_view kFailureCount = "fc";
inline constexpr std::string_view kLockUntil = "lu";

// The locale index, as declared in the application's table. It is here and not
// only in the token because it is a PREFERENCE rather than an authority: a
// token minted before the person changed it would otherwise keep rendering
// their account in the language they just stopped using.
inline constexpr std::string_view kLocale = "lc";

inline constexpr std::string_view kCreatedAt = "created_at";
inline constexpr std::string_view kUpdatedAt = "updated_at";

// Optimistic concurrency. Spelled by anvil/db/versioned.h, repeated here so an
// index catalogue can name it without including that header.
inline constexpr std::string_view kVersion = "v";

}  // namespace anvil::identity::fields
