// The recorder for anvil's own failure bytes.
//
// --- why this program exists ------------------------------------------------
//
// `testapp_emit_descriptor` writes an APPLICATION's tables. This writes the
// LIBRARY's bytes, and the distinction is worth keeping straight: one of those
// files changes when an application changes, and the other changes only when
// anvil does.
//
// A client generator built against this repository already read `wire_name`,
// `http_status`, `is_stealth_hidden` and `kNotFoundBody` out of these headers,
// which is real provenance. What it could not read was the two lines that
// ASSEMBLE a body, because until `http::append_error_body` landed there was no
// function here that returned one — so the generator copied the assembly, and a
// copy in a client is a copy that goes stale silently, in the direction where
// nobody notices: the client keeps parsing a shape the server stopped sending.
//
// So the bytes are recorded here, by the writer itself. **No string in this file
// is a body.** Every `body` below comes out of `append_error_body` or is
// `kNotFoundBody` verbatim; a literal here would be the second copy this program
// exists to delete.
//
// --- determinism ------------------------------------------------------------
//
// A file that changes on every run is a file nobody diffs, and a diff is the
// entire value of publishing this. The one value that would otherwise be fresh
// per run is the request id, so the recorded id is a documented PLACEHOLDER —
// see `kPlaceholderId` — and is published under its own key so a reader can see
// that it is one. Everything else is a pure function of the headers.
//
// --- what is NOT here -------------------------------------------------------
//
// Headers, apart from the name the id goes out under. The stealth response's
// `Cache-Control`, its `X-Content-Type-Options` and — more importantly — the
// three it deliberately omits are built in `src/accesscontrol/stealth.cc`, which
// is compiled into `anvil::platform` and needs Drogon. This program links
// `anvil::foundation` and nothing else, which is the same property that makes
// generating a client need no driver, no event loop and no database. Recording a
// header set this binary cannot read would mean typing it in, and a typed-in
// header set is exactly the copy this file exists to remove.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "anvil/core/types.h"
#include "anvil/core/version.h"
#include "anvil/crypto/digest.h"
#include "anvil/http/errors.h"
#include "anvil/http/json_writer.h"
#include "anvil/http/request_id.h"
#include "anvil/http/request_scope.h"
#include "anvil/input/fields.h"

namespace {

using anvil::ErrorCode;
using anvil::input::FieldError;
using anvil::input::Reason;

// The format version, bumped by anvil when the SHAPE of this document changes.
// Separate from the descriptor's number for the reason those two files are
// separate at all: they version independently because they change for different
// reasons.
constexpr int kEnvelopesFormat = 1;

// The id every recorded body carries.
//
// All-ones, which renders `7ZZZZZZZZZZZZZZZZZZZZZZZZZ` — the extreme vector
// `errors_test.cc` already pins by hand. Three things recommend it over a fresh
// mint or a zero:
//
//   * It cannot be mistaken for a real id. The leading `7` is the two bits of
//     padding showing through, and the timestamp half being all-ones puts it in
//     the year 10889.
//   * It is not all-zero, which `http::request_id_of()` returns for a request
//     with no scope and therefore already means "no id was minted".
//   * It exercises the encoder's hard case, so a recorder that produced 26 `Z`s
//     would be visibly wrong in the artefact rather than quietly wrong in a
//     client built from it.
constexpr anvil::http::RequestId kPlaceholderId{
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
     0xFF, 0xFF}};

// Every ErrorCode, listed once, in numeric order.
//
// The same list `src/descriptor/emit.cc` keeps and for the same reason: a
// range-for over an enum is not a thing C++ has, and casting 0..kMax emits a
// name for a value that is not an enumerator the moment the enum grows a gap.
// The assertion below turns an appended code with no entry here into a build
// failure rather than an artefact with a hole in it.
constexpr std::array<ErrorCode, 15> kAllErrorCodes{
    ErrorCode::Ok,
    ErrorCode::Unauthenticated,
    ErrorCode::Forbidden,
    ErrorCode::NotFound,
    ErrorCode::CapabilityRequired,
    ErrorCode::CapabilityInvalid,
    ErrorCode::ValidationFailed,
    ErrorCode::Conflict,
    ErrorCode::VersionMismatch,
    ErrorCode::RateLimited,
    ErrorCode::PayloadTooLarge,
    ErrorCode::UnsupportedMedia,
    ErrorCode::ServiceUnavailable,
    ErrorCode::Internal,
    ErrorCode::InsufficientStorage,
};

static_assert(kAllErrorCodes.back() == anvil::kMaxErrorCode,
              "an ErrorCode was appended without being listed here, so its body would "
              "be missing from the recorded envelopes");

// The worked `fields` example, and the reason it has TWO entries rather than
// one: a client that has only ever seen a single-field map has never seen the
// separator, and the separator is the half a hand-written parser gets wrong.
//
// The names are deliberately anvil's own vocabulary and not an application's —
// a field name here is a schema constant, and `input/fields.h` is explicit that
// it is never a key taken from a request.
constexpr std::array<FieldError, 2> kWorkedFields{{
    {"email", Reason::BadFormat},
    {"password", Reason::Weak},
}};

void append_hex(std::string& out, std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kHex = "0123456789abcdef";
    out += '"';
    for (const std::uint8_t byte : bytes) {
        out += kHex[byte >> 4U];
        out += kHex[byte & 0x0FU];
    }
    out += '"';
}

// One recorded failure: everything a client needs to recognise it, plus the
// exact bytes the server sends.
void append_error(std::string& out, ErrorCode code) {
    out += '{';
    anvil::http::append_json_key(out, "code");
    anvil::http::append_json_string(out, anvil::http::wire_name(code));
    out += ',';
    anvil::http::append_json_key(out, "value");
    anvil::http::append_json_int(out, static_cast<std::int64_t>(code));
    out += ',';
    anvil::http::append_json_key(out, "http");
    anvil::http::append_json_int(out, anvil::http::http_status(code));
    out += ',';
    // Which codes the filter rewrites to the byte-identical 404. A client may
    // not report a denial on such a route as a denial, or it rebuilds the
    // existence oracle the server removed.
    anvil::http::append_json_key(out, "stealth_hidden");
    out += anvil::http::is_stealth_hidden(code) ? "true" : "false";
    out += ',';
    // Whether `fields` is present in the body below. Emitted as a fact rather
    // than left for a client to infer from the bytes, because the inference — "I
    // did not see the key, so it is optional" — is how a form comes to have no
    // branch for the one code that carries it.
    anvil::http::append_json_key(out, "carries_fields");
    out += anvil::http::carries_field_detail(code) ? "true" : "false";
    out += ',';

    anvil::http::append_json_key(out, "body");
    std::string body;
    body.reserve(128);
    // The writer, not a literal. For ValidationFailed this is the EMPTY map,
    // which is a deliberate shape rather than an omission: `append_error_body`
    // writes `fields` whenever `carries_field_detail()` says so, including when
    // the span is empty, so a client parses one thing rather than two. The
    // populated map is recorded separately below.
    anvil::http::append_error_body(body, code, kPlaceholderId);
    anvil::http::append_json_string(out, body);
    out += '}';
}

void append_bodies(std::string& out) {
    out += '{';

    // The shape of every failure, once per code.
    anvil::http::append_json_key(out, "errors");
    out += '[';
    bool first = true;
    for (const ErrorCode code : kAllErrorCodes) {
        if (!first) { out += ','; }
        first = false;
        append_error(out, code);
    }
    out += ']';
    out += ',';

    // The populated `fields` map, which no entry above can show: the loop
    // records each code's body with an empty span, because a body carrying a
    // field map is a property of one code and a client needs the separator.
    anvil::http::append_json_key(out, "validation_example");
    out += '{';
    anvil::http::append_json_key(out, "fields");
    out += '[';
    for (std::size_t i = 0; i < kWorkedFields.size(); ++i) {
        if (i != 0) { out += ','; }
        out += '{';
        anvil::http::append_json_key(out, "field");
        anvil::http::append_json_string(out, kWorkedFields[i].field);
        out += ',';
        anvil::http::append_json_key(out, "reason");
        anvil::http::append_json_string(out, anvil::http::wire_name(kWorkedFields[i].reason));
        out += '}';
    }
    out += ']';
    out += ',';
    anvil::http::append_json_key(out, "body");
    {
        std::string body;
        body.reserve(160);
        anvil::http::append_error_body(body, ErrorCode::ValidationFailed, kPlaceholderId,
                                       kWorkedFields);
        anvil::http::append_json_string(out, body);
    }
    out += '}';
    out += ',';

    // The stealth body, verbatim, and the three facts about it a client cannot
    // infer from any entry above.
    anvil::http::append_json_key(out, "stealth");
    out += '{';
    anvil::http::append_json_key(out, "http");
    anvil::http::append_json_int(out, anvil::http::http_status(ErrorCode::NotFound));
    out += ',';
    anvil::http::append_json_key(out, "content_type");
    anvil::http::append_json_string(out, anvil::http::kNotFoundContentType);
    out += ',';
    // It carries NEITHER a request id nor a field map, and that is not an
    // oversight to be tidied later: a correlatable value a genuine 404 does not
    // have is one of the tells `accesscontrol/stealth.h` enumerates beside
    // `WWW-Authenticate` and `Set-Cookie`. Published as an explicit `false` so a
    // client generator records the absence rather than inheriting it.
    anvil::http::append_json_key(out, "carries_request_id");
    out += "false";
    out += ',';
    anvil::http::append_json_key(out, "body");
    anvil::http::append_json_string(out, anvil::http::kNotFoundBody);
    out += '}';
    out += ',';

    // The content type every body above is sent with, EXCEPT the stealth one,
    // whose own type is recorded beside it because it has to match an edge's
    // `error_page 404` byte for byte and therefore carries no charset parameter.
    anvil::http::append_json_key(out, "content_type");
    anvil::http::append_json_string(out, anvil::http::kJsonContentType);
    out += ',';

    // The header the same id goes out under, so a client that cannot parse a
    // body still knows where to read the value a user will quote.
    anvil::http::append_json_key(out, "request_id_header");
    anvil::http::append_json_string(out, anvil::http::kRequestIdHeader);

    out += '}';
}

}  // namespace

int main() {
    // Built first because the hash is over its bytes, and emitted after the
    // hash for the reason the descriptor does the same: a reader streaming the
    // document learns whether it already has these bytes before parsing them.
    std::string bodies;
    bodies.reserve(4096);
    append_bodies(bodies);

    const anvil::crypto::Digest256 digest = anvil::crypto::sha256(std::string_view{bodies});

    std::string doc;
    doc.reserve(bodies.size() + 256);
    doc += '{';
    anvil::http::append_json_key(doc, "envelopes");
    anvil::http::append_json_int(doc, kEnvelopesFormat);
    doc += ',';
    anvil::http::append_json_key(doc, "emitted_by");
    anvil::http::append_json_string(doc, anvil::version_string());
    doc += ',';
    // Outside the hash, so that publishing the placeholder does not make the
    // hash a function of a value that means nothing.
    anvil::http::append_json_key(doc, "request_id_placeholder");
    {
        const std::array<char, anvil::http::kRequestIdChars> rendered =
            anvil::http::format_request_id(kPlaceholderId);
        anvil::http::append_json_string(doc, std::string_view{rendered.data(), rendered.size()});
    }
    doc += ',';
    anvil::http::append_json_key(doc, "hash");
    append_hex(doc, digest);
    doc += ',';
    anvil::http::append_json_key(doc, "bodies");
    doc += bodies;
    doc += '}';

    std::fwrite(doc.data(), 1, doc.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
