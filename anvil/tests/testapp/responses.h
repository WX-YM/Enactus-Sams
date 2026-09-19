#pragma once

// The reference application's declared response shapes.
//
// anvil ships `ResponseField`, the grammar and the writer; WHICH routes have a
// described body, and what that body is, is the application's
// (docs/01-seams.md §14). One table per described route, referenced from
// `route_descriptions.h` as a span.
//
// --- why only one route here -----------------------------------------------
//
// Adoption is per route and this file is where that is visible. `identity.me`
// is described because its body is flat and it is the shape every application
// writes first: who am I, in what locale, with what standing.
//
// `session.current` deliberately is NOT, and it is the more instructive entry.
// Its body is `{"routes":{…},"authority":{…}}` — two nested objects, one of them
// a map with a key per route — and the grammar in `anvil/http/response_writer.h`
// covers flat objects and arrays of one declared shape and nothing else. So its
// description leaves the shape empty, the emitter writes `"response":null`, and
// a generated client keeps the declaration it already had. That is the honest
// answer rather than a schema that describes 80% of the body, which is the one
// a client would trust and be wrong about.

#include <array>

#include "anvil/http/response_spec.h"

namespace testapp {

namespace rsp = anvil::http;

// `GET /me`: the signed-in holder, as the filter proved them.
//
// Every field is one the access filter already established, so the handler
// invents nothing and reads no database — which is what makes it the right first
// consumer for a writer whose whole property is that the bytes and the schema
// cannot disagree.
inline constexpr std::array<rsp::ResponseField, 4> kMeResponse{{
    {"id", rsp::FieldKind::Uuid, false},
    {"session_id", rsp::FieldKind::Uuid, false},
    // The tag, not the one-byte index. The index is a storage detail whose
    // meaning is the server's locale table order, and a client that received it
    // would be holding a number it can only interpret by holding that table too.
    {"locale", rsp::FieldKind::String, false},
    // Nullable, and this is the field the flag exists for. A superadmin's
    // permission set is deliberately not all-ones, so "the bits this holder
    // holds" is the wrong question for exactly one account type and the honest
    // answer there is nothing rather than an empty list — an empty list reads as
    // "holds no permissions", which is the opposite of true.
    {"permissions", rsp::FieldKind::Strings, true},
}};

static_assert(rsp::response_shape_is_well_formed(kMeResponse),
              "an empty, non-UTF-8 or duplicated key in a declared response shape");

}  // namespace testapp
