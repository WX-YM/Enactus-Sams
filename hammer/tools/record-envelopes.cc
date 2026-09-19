// Records the envelopes anvil writes, out of anvil's own tables.
//
// THIS FILE IS NOT BUILT BY THIS REPOSITORY. It is C++ in a TypeScript library
// and that is deliberate: it is a recorder, not a component, and it is here
// rather than nowhere because the fixture it produces is worth exactly as much
// as its provenance. `tests/fixtures/envelopes/anvil.json` is committed;
// `tools/record-envelopes.sh` is what refreshes it, against a sibling anvil
// checkout, by hand.
//
// --- what is read and what is copied ----------------------------------------
//
// Everything below but two lines is READ from a header in anvil/include:
// `wire_name(ErrorCode)`, `http_status`, `is_stealth_hidden`,
// `carries_field_detail`, `kMaxErrorCode`, `kNotFoundBody`,
// `kNotFoundContentType` and `wire_name(input::Reason)`. Those are the tables
// that can drift away from this client's generated unions, and reading them is
// the whole point.
//
// The two copied lines are the body ASSEMBLY, because anvil has no function that
// returns one: `plain_error()` builds the string inline in
// `src/accesscontrol/access_filter.cc` and `build()` does the same in
// `src/accesscontrol/stealth.cc`. They are cited at the line that copies them so
// that a reader can check them, and the row asking anvil to own this emitter is
// in `docs/15-tasks.md` §Cross-repo — a copy in a client is a copy that goes
// stale silently, which is exactly the thing this fixture exists to catch
// elsewhere.
//
//   g++ -std=c++20 -I <anvil>/include -o record-envelopes tools/record-envelopes.cc

#include <cstdio>
#include <string>
#include <string_view>

#include "anvil/core/types.h"
#include "anvil/http/errors.h"
#include "anvil/input/fields.h"

namespace {

std::string quoted(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
}

}  // namespace

int main() {
    using anvil::ErrorCode;
    namespace http = anvil::http;
    namespace input = anvil::input;

    std::string out;
    out += "{\n";
    out += "  \"recordedFrom\": \"anvil/include/anvil/http/errors.h\",\n";
    out += "  \"codes\": [\n";

    for (int raw = 0; raw <= static_cast<int>(anvil::kMaxErrorCode); ++raw) {
        const ErrorCode code = static_cast<ErrorCode>(raw);
        const std::string name{http::wire_name(code)};

        // src/accesscontrol/access_filter.cc, plain_error(): the code and
        // nothing else. No request id — a 401 from that filter carries no
        // server-side detail worth correlating — and no submitted value, ever.
        const std::string body =
            raw == 0 ? std::string{} : std::string{R"({"error":{"code":")"} + name + R"("}})";

        out += "    {";
        out += "\"name\": " + quoted(name);
        out += ", \"value\": " + std::to_string(raw);
        out += ", \"status\": " + std::to_string(http::http_status(code));
        out += ", \"stealthHidden\": ";
        out += http::is_stealth_hidden(code) ? "true" : "false";
        out += ", \"carriesFieldDetail\": ";
        out += http::carries_field_detail(code) ? "true" : "false";
        // `OK` is the success member. It is not a failure any error surface can
        // reach and anvil never writes an error body for it, so it carries no
        // body here rather than a fabricated one.
        out += ", \"body\": ";
        out += raw == 0 ? "null" : quoted(body);
        out += ", \"contentType\": " + quoted(http::kNotFoundContentType);
        out += ", \"cacheControl\": \"no-store\"}";
        out += raw == static_cast<int>(anvil::kMaxErrorCode) ? "\n" : ",\n";
    }
    out += "  ],\n";

    out += "  \"reasons\": [\n";
    for (int raw = 0; raw <= static_cast<int>(http::kMaxReason); ++raw) {
        const input::Reason reason = static_cast<input::Reason>(raw);
        out += "    {\"name\": " + quoted(http::wire_name(reason)) +
               ", \"value\": " + std::to_string(raw) + "}";
        out += raw == static_cast<int>(http::kMaxReason) ? "\n" : ",\n";
    }
    out += "  ],\n";

    // src/accesscontrol/stealth.cc, build(): the constexpr body, and the headers
    // that have to be byte-identical to an unmatched route's — including
    // Nginx's `error_page 404`, which serves exactly these bytes with no request
    // behind it to have an id.
    out += "  \"stealth\": {\"status\": 404, \"body\": " + quoted(http::kNotFoundBody) +
           ", \"contentType\": " + quoted(http::kNotFoundContentType) +
           ", \"cacheControl\": \"no-store\", \"contentTypeOptions\": \"nosniff\"}\n";
    out += "}\n";

    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
