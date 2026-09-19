#pragma once

// The client descriptor: every table a generated client needs, as one JSON
// document, emitted by a program the application builds and runs once.
//
// --- why this exists --------------------------------------------------------
//
// A web client holds the same tables this server does — the routes, the
// permission bits, the error vocabulary, the limits. Written twice they drift,
// and the drift is silent in the direction that matters: a renumbered
// permission bit is not a missing feature, it is a WRONG AUTHORITY CHECK
// rendered to a user as an affordance that should not exist.
//
// So they are written once, here, where a static_assert already validates them,
// and the client is generated from the result.
//
// --- what this is not -------------------------------------------------------
//
// It is not served. It carries every route's path, including the ones
// `path_in_bundle` refuses to let a client compile in, and a copy of it in a
// published directory hands over in one request the map the stealth 404 exists
// to withhold. It is a build artefact: emitted into a build tree, read by a
// generator, and not copied anywhere near a document root.
//
// It also carries nothing about storage. No collection name, no index, no
// query, no migration — a client has no business knowing the storage layout,
// and a name in a bundle is a name in an attacker's notes.
//
// --- the hash ---------------------------------------------------------------
//
// `hash` is SHA-256 over the bytes of the `tables` object exactly as emitted,
// and it is the answer to a question a long-lived tab asks: is the client I am
// running built from the server I am talking to? A deploy while a tab is open
// is the ordinary case, not the exotic one. The metadata around it — the
// emitter's version, the application's — is deliberately outside the hash, so
// a version bump that changes no table does not invalidate every client.
//
// Determinism is therefore load-bearing: the same tables must produce the same
// bytes on every run, or the hash reports drift that is not there. Nothing here
// iterates an unordered container.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "anvil/accesscontrol/route_registry.h"
#include "anvil/analytics/event_spec.h"
#include "anvil/core/perm_catalogue.h"
#include "anvil/descriptor/route_description.h"
#include "anvil/forms/field_type.h"
#include "anvil/http/rate_limit.h"
#include "anvil/identity/capability_spec.h"
#include "anvil/notifications/topic_spec.h"
#include "anvil/sections/registry.h"

namespace anvil::descriptor {

// The format version, bumped by anvil when the SHAPE changes. It is not the
// application's version and not the hash: those answer "whose tables" and "which
// tables", and this answers "can the generator read this file at all". One
// number cannot answer three questions.
//
// 2 adds the content tables — `field_types`, `sections`, `topics`, `events` and
// `media`. Purely additive, so a generator written for 1 still parses a 2; what
// it cannot do is the reverse, which is exactly the question this number answers
// for a generator that needs them.
//
// 3 adds `response` to every route: the declared shape of its success body, or
// `null` where the body is hand-written and undescribed. Additive in the same
// way, and `null` is what a generator written for 2 already assumed about every
// route — so the number is what tells it that a non-null answer is now possible
// and is underwritten by the writer rather than by a second table
// (anvil/http/response_writer.h).
inline constexpr int kDescriptorFormat = 3;

// The bounds a client enforces before it spends a round trip finding out. They
// are the application's, because they are deployment decisions — anvil ships
// the mechanism that enforces them and not the numbers.
struct Limits final {
    std::uint64_t upload_max_bytes;   // a byte cap, because an upload is bytes
    std::uint64_t body_max_bytes;
    std::uint32_t page_limit_max;
};

// Everything an application hands the emitter. Spans, so every table stays
// constexpr and stays in .rodata, and so a table an application has not declared
// yet is an empty span rather than a missing symbol.
//
// An empty span emits an empty array, not a missing key. A generator branching
// on "is this member present" branches on a typo, which is the same reason a
// route that is not a list route emits `"page":null` rather than nothing.
struct DescriptorInput final {
    std::string_view app_name;
    std::string_view app_version;

    // --- the transport and authority tables ---------------------------------
    std::span<const PermName>                   permissions;
    std::span<const accesscontrol::RoutePolicy> routes;
    std::span<const RouteDescription>           route_descriptions;
    std::span<const identity::CapabilityScopeSpec> capability_scopes;
    std::span<const http::RateLimitRule>        rate_limits;

    // --- the content tables -------------------------------------------------
    //
    // The four seams whose tables describe what a client RENDERS rather than how
    // it calls: the field types a form may declare, the sections it may edit, the
    // topics it may subscribe to, and the events it may report. Each is already
    // validated by a static_assert beside the application's own table, so the
    // emitter checks none of them again.
    //
    // The media table is not here, because it is not a span seam: namespaces and
    // the role ladder are declared in <anvil_app_config.h> and dimension arrays
    // inside anvil's own translation units, so the emitter reads them where
    // everything else does (docs/01-seams.md §2) — the same reason the locale
    // table is not a parameter either.
    std::span<const forms::FieldTypeSpec>          field_types;
    std::span<const sections::SectionSpec>         sections;
    std::span<const notifications::TopicSpec>      topics;
    std::span<const analytics::EventSpec>          events;

    Limits limits;
};

// Appends the descriptor document to `out`.
//
// Appends rather than returns, for the reason every writer in this codebase
// does: the caller owns one buffer, reserves once, and no fragment is
// materialised on the way.
//
// The locale table is not a parameter. It is a config header seam rather than a
// span seam — it dimensions std::array members inside anvil's own translation
// units — so the emitter reads it where everything else does
// (docs/01-seams.md §2) rather than asking for it and letting a caller pass a
// different one.
void append_descriptor(std::string& out, const DescriptorInput& input);

// The whole document, for a caller that is about to write it to a file and exit.
[[nodiscard]] std::string emit_descriptor(const DescriptorInput& input);

}  // namespace anvil::descriptor
