#pragma once

// The reference application's configuration header.
//
// Every application built on anvil supplies one of these and points
// ANVIL_CONFIG_INCLUDE_DIR at the directory holding it. anvil's own headers
// include it by that name, which is why it is <anvil_app_config.h> and not a
// path (docs/01-seams.md §2).
//
// This copy belongs to tests/testapp — the reference consumer. It is compiled by
// every build of the test suite, so the worked example in
// docs/02-getting-started.md is a file that must keep compiling rather than a
// snippet that can rot.
//
// NOTHING HERE MAY NAME A DRIVER TYPE. anvil::foundation includes this header for
// the locale table and links no database driver at all, so a mongocxx type
// reaching this file surfaces as a missing include in a text validator. The job
// table hands every handler a client, so it lives in anvil_app_jobs.h instead,
// included only by anvil/timer/registry.h — which is platform-only. That split is
// the same boundary the link targets already draw.

#include <array>
#include <cstddef>
#include <cstdint>

#include "anvil/core/locale_spec.h"
#include "anvil/db/collection_spec.h"
#include "anvil/fs/namespace_spec.h"

namespace anvil::config {

// --- locales ---------------------------------------------------------------
//
// THE ORDER IS PERSISTED. The index is byte 3 of every access token and the value
// stored in users.lang. APPEND ONLY: never reorder, never remove. This is exactly
// the rule permission bit indices live under, and it fails the same way —
// silently, by reinterpreting rows that are already written.
//
// Two locales here because the second one earns its keep in tests: an RTL locale
// with a real ICU collation exercises the bidi, normalisation and collation paths
// that a single Latin locale would leave unmeasured.
inline constexpr std::array<LocaleSpec, 2> kLocales{{
    {"en", "en", false},
    {"ar", "ar", true},
}};

// The locale a request that names none is answered in.
inline constexpr std::size_t kDefaultLocale = 0;

static_assert(!kLocales.empty(), "at least one locale must be declared");
static_assert(kDefaultLocale < kLocales.size(), "the default locale must exist");

// --- storage namespaces ----------------------------------------------------
//
// Which API owns a stored object. THE ORDER IS PERSISTED: the index is stored as
// int32 in the media row, so this table is append-only — never reorder, never
// reuse a retired slot (anvil/fs/namespace.h).
//
// The directory name is also the `{ns}` segment of a media URL, so there is one
// string for both rather than two that can disagree.
inline constexpr std::array<fs::NamespaceSpec, 3> kNamespaces{{
    // No `accepts`, so both take everything the pipeline decodes. A namespace
    // with no opinion states none rather than restating the default.
    {"content"},
    {"media"},
    // Deliberately a third: a namespace whose uploader has NO account exercises
    // the path that separates "who may write here" from "who may read this",
    // which a single-namespace table would leave untested.
    //
    // And NARROWED, which is the other thing it exercises. This is the only
    // namespace fed by unauthenticated input, and JPEG and PNG are what a camera
    // and a canvas export produce — so the AVIF and WebP decoders, which are the
    // newer and larger attack surface of the four, are not reachable from a form
    // anyone on the internet can post to. A global accept list could not express
    // that, which is the whole reason the mask is per namespace.
    {"guest", fs::mime_bit(fs::Mime::Jpeg) | fs::mime_bit(fs::Mime::Png)},
}};

static_assert(!fs::mime_accepted(kNamespaces[2].accepts, fs::Mime::Avif),
              "the guest namespace is narrowed on purpose; widening it puts the AVIF decoder "
              "back behind an unauthenticated form");
static_assert(fs::mime_accepted(kNamespaces[0].accepts, fs::Mime::Avif),
              "a namespace that declares no mask takes everything the pipeline decodes");

// --- the variant ladder ----------------------------------------------------
//
// The widths the image pipeline writes. Every role below must name one of these,
// and anvil/fs/namespace.h static_asserts exactly that: a role pointing at a
// width nothing writes resolves to the next rung down for every object in the
// system, and the table looks entirely correct while doing it.
inline constexpr std::array<std::uint16_t, 5> kVariantWidths{{320, 640, 1024, 1600, 2560}};

// Which rung each role resolves to, per namespace, in kNamespaces order. It MAY
// differ per namespace — a thumbnail in a list and a hero on a landing page want
// different ladders — and changing one needs no client release, because the
// client names a role and never a width.
inline constexpr std::array<std::array<std::uint16_t, fs::kRoleCount>, 3> kRoleWidths{{
    {{320, 1024, 1600, 2560}},  // content: the public furniture, goes full-bleed
    {{320, 640, 1024, 1600}},   // media:   illustrations inside a body of text
    {{320, 640, 1024, 1600}},   // guest:   looked at beside the row it belongs to
}};

static_assert(kRoleWidths.size() == kNamespaces.size(),
              "every declared namespace needs a row in the role-width table");

// --- databases -------------------------------------------------------------
//
// A stable KEY, not the physical name: the physical name is a deployment decision
// read from the environment at boot, and the code refers to the key.
//
// Two of them, because one would leave the multi-database path untested. The
// second exists for the reason a real deployment would want one — a high-churn
// collection kept beside hot application data spends WiredTiger cache on rows
// that are about to expire.
inline constexpr std::array<db::DatabaseSpec, 2> kDatabases{{
    {"application"},
    {"scratch"},
}};

// --- collections -----------------------------------------------------------
//
// Name, the field a LIFETIME TTL expires on (empty when there is none), and which
// database the rows live in.
//
// The expiry field is the load-bearing column. A TTL index is a garbage
// collector, not an access control: the monitor runs roughly every 60 seconds, so
// an expired session is still READABLE and would still authenticate. Naming the
// field here is what lets append_not_expired filter on it, and what
// tools/check-db-discipline.sh fails the build over when a query forgets.
//
// audit_log deliberately names NO field despite having a TTL index: 400 days of
// history is a retention policy, not a lifetime, and filtering its reads on
// `at > now` would return nothing at all.
inline constexpr std::array<db::CollectionSpec, 17> kCollections{{
    {"users",               "",           0},
    {"user_sessions",       "expires_at", 0},
    {"capability_tokens",   "expires_at", 0},
    {"email_verifications", "expires_at", 0},
    {"media",               "",           0},
    {"audit_log",           "",           0},
    // One document, holding a counter nobody reads. It is a MUTEX rather than
    // data: every transaction that could reduce the privileged-account
    // population $incs it first, so two of them collide there and one is retried
    // against the other's committed state (anvil/identity/staff.h).
    {"staff_guard",         "",           0},
    {"form_definitions",    "",           0},
    {"form_submissions",    "",           0},
    {"drafts",              "expires_at", 1},
    // Two documents per key — published and draft — under a compound `_id`, so
    // the collection carries no secondary index at all. No expiry field: a
    // section is the live content of a page and has no lifetime.
    {"sections",            "",           0},
    // Both notification collections name an expiry field, and both mean a
    // LIFETIME: a notification past its retention must be absent from the inbox
    // and from the badge count, not merely eligible for reaping. The TTL monitor
    // lags by up to a minute, so the index alone would leave an expired row
    // readable — and rendering one is the leak the read-time recheck exists to
    // prevent, arriving by another route.
    {"notifications",       "expires_at", 0},
    {"notification_inbox",  "expires_at", 0},
    // An endpoint has no lifetime. It is disabled, or it is deleted.
    {"notification_clients", "",          0},
    // The two high-churn analytics collections go in the SECOND database, which
    // is what that database was declared for: a collection turning over its
    // whole contents every few days, kept beside hot application data, spends
    // WiredTiger cache on rows that are about to expire
    // (docs/17-analytics.md §15).
    //
    // Both name an expiry field and both mean a LIFETIME. The TTL monitor lags
    // by up to a minute, so the index alone would leave an expired row readable,
    // and a retention window that a query can see past is not a retention
    // window.
    {"analytics_events",    "expires_at", 1},
    {"analytics_sessions",  "expires_at", 1},
    // Rollups are the opposite case and name NO field. They carry no subject,
    // are not erased, and a count of signups per day has no lifetime — it is the
    // answer the raw rows were collected to produce, and it outlives them
    // deliberately.
    {"analytics_rollups",   "",           0},
}};

}  // namespace anvil::config
