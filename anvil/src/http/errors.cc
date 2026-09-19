#include "anvil/http/errors.h"

#include "anvil/http/json_writer.h"
#include "anvil/http/request_id.h"

namespace anvil::http {

void append_error_body(std::string& out, ErrorCode code, const RequestId& request_id,
                       std::span<const input::FieldError> fields) {
    out.append(R"({"error":)");
    out.push_back('{');
    append_json_key(out, "code");
    append_json_string(out, wire_name(code));
    out.push_back(',');
    append_json_key(out, "request_id");
    out.push_back('"');
    append_request_id(out, request_id);
    out.push_back('"');

    if (carries_field_detail(code)) {
        out.push_back(',');
        append_json_key(out, "fields");
        out.push_back('{');
        bool first = true;
        for (const input::FieldError& field : fields) {
            if (!first) { out.push_back(','); }
            first = false;
            append_json_key(out, field.field);
            append_json_string(out, wire_name(field.reason));
        }
        out.push_back('}');
    }
    out.append("}}");
}

// Everything in this translation unit is constexpr and lives in the header, by
// design: the mapping must be usable in static_asserts so a new ErrorCode
// without a mapping fails the build rather than silently returning 500.
//
// This file exists to give the tests a translation unit to link against and to
// hold the exhaustiveness assertions below.

namespace {

// If a new enumerator is added without a case in either switch, -Werror on
// -Wswitch fires at compile time. These assertions cover the values, so a case
// that exists but returns something wrong is also caught.
static_assert(http_status(ErrorCode::Unauthenticated) == 401);
static_assert(http_status(ErrorCode::Forbidden) == 403);
static_assert(http_status(ErrorCode::NotFound) == 404);
static_assert(http_status(ErrorCode::ValidationFailed) == 400);
static_assert(http_status(ErrorCode::VersionMismatch) == 409);
static_assert(http_status(ErrorCode::RateLimited) == 429);
static_assert(http_status(ErrorCode::ServiceUnavailable) == 503);
static_assert(http_status(ErrorCode::Internal) == 500);

static_assert(is_stealth_hidden(ErrorCode::Unauthenticated));
static_assert(is_stealth_hidden(ErrorCode::Forbidden));
static_assert(!is_stealth_hidden(ErrorCode::ValidationFailed));
static_assert(!is_stealth_hidden(ErrorCode::NotFound));
static_assert(!is_stealth_hidden(ErrorCode::ServiceUnavailable));

static_assert(carries_field_detail(ErrorCode::ValidationFailed));
static_assert(!carries_field_detail(ErrorCode::Internal));

static_assert(wire_name(ErrorCode::VersionMismatch) == "VERSION_MISMATCH");

}  // namespace

}  // namespace anvil::http
