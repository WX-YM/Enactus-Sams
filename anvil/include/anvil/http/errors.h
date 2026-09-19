#pragma once

// The single ErrorCode -> HTTP status table, and the one error body shape.
//
// Exactly one mapping exists in the codebase. A second one drifts, and drift in
// this table is how a 403 leaks from an admin route that was supposed to
// stealth-404 (docs/00-architecture.md §8).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "anvil/core/types.h"
#include "anvil/http/request_id.h"
#include "anvil/input/fields.h"

namespace anvil::http {

// Wire name for an ErrorCode. Stable: clients map these to their own bilingual
// messages, so renaming one is a breaking API change.
[[nodiscard]] constexpr std::string_view wire_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:                 return "OK";
        case ErrorCode::Unauthenticated:    return "UNAUTHENTICATED";
        case ErrorCode::Forbidden:          return "FORBIDDEN";
        case ErrorCode::NotFound:           return "NOT_FOUND";
        case ErrorCode::CapabilityRequired: return "CAPABILITY_REQUIRED";
        case ErrorCode::CapabilityInvalid:  return "CAPABILITY_INVALID";
        case ErrorCode::ValidationFailed:   return "VALIDATION_FAILED";
        case ErrorCode::Conflict:           return "CONFLICT";
        case ErrorCode::VersionMismatch:    return "VERSION_MISMATCH";
        case ErrorCode::RateLimited:        return "RATE_LIMITED";
        case ErrorCode::PayloadTooLarge:    return "PAYLOAD_TOO_LARGE";
        case ErrorCode::UnsupportedMedia:   return "UNSUPPORTED_MEDIA";
        case ErrorCode::ServiceUnavailable: return "SERVICE_UNAVAILABLE";
        case ErrorCode::Internal:           return "INTERNAL";
        case ErrorCode::InsufficientStorage: return "INSUFFICIENT_STORAGE";
    }
    // Unreachable for a valid enumerator. Returning INTERNAL rather than an
    // empty string means a future code added without a case still fails closed.
    return "INTERNAL";
}

[[nodiscard]] constexpr int http_status(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:                 return 200;
        case ErrorCode::Unauthenticated:    return 401;
        case ErrorCode::Forbidden:          return 403;
        case ErrorCode::NotFound:           return 404;
        case ErrorCode::CapabilityRequired: return 428;  // Precondition Required
        case ErrorCode::CapabilityInvalid:  return 403;
        case ErrorCode::ValidationFailed:   return 400;
        case ErrorCode::Conflict:           return 409;
        case ErrorCode::VersionMismatch:    return 409;
        case ErrorCode::RateLimited:        return 429;
        case ErrorCode::PayloadTooLarge:    return 413;
        case ErrorCode::UnsupportedMedia:   return 415;
        case ErrorCode::ServiceUnavailable: return 503;
        case ErrorCode::Internal:           return 500;
        case ErrorCode::InsufficientStorage: return 507;  // Insufficient Storage
    }
    return 500;
}

// Whether this code is one the stealth filter must rewrite to a 404 on an admin
// route. Kept here rather than in the filter so the decision is visible next to
// the status table it overrides.
[[nodiscard]] constexpr bool is_stealth_hidden(ErrorCode code) noexcept {
    return code == ErrorCode::Unauthenticated || code == ErrorCode::Forbidden ||
           code == ErrorCode::CapabilityRequired || code == ErrorCode::CapabilityInvalid;
}

// Only ValidationFailed carries a per-field breakdown. Every other code returns
// the code and a request id, nothing more — a 409 that explains which document
// version it saw is a disclosure.
[[nodiscard]] constexpr bool carries_field_detail(ErrorCode code) noexcept {
    return code == ErrorCode::ValidationFailed;
}

// Wire name for a validation Reason — the values of the `fields` map that
// carries_field_detail admits.
//
// It lives beside wire_name(ErrorCode) because it is the other half of the one
// body shape this header defines, and because the two are read together: a
// client maps a code to a sentence and a reason to a sentence, from one
// vocabulary.
//
// It exists at all because until now there was none. docs/00-architecture.md §8
// has shown `{"email":"INVALID_FORMAT"}` since phase 0 and nothing in the
// library produced that string, so every application invented its own spelling
// of anvil's own enum — which means two applications answer differently for the
// same failure, and a generated client cannot be generated at all. The name is
// the enumerator in SCREAMING_SNAKE and nothing cleverer, so the mapping is
// mechanical in both directions.
//
// Stable, for the same reason the codes are: renaming one is a breaking API
// change for every client that has a word for it.
[[nodiscard]] constexpr std::string_view wire_name(input::Reason reason) noexcept {
    switch (reason) {
        case input::Reason::Ok:          return "OK";
        case input::Reason::Required:    return "REQUIRED";
        case input::Reason::TooShort:    return "TOO_SHORT";
        case input::Reason::TooLong:     return "TOO_LONG";
        case input::Reason::BadFormat:   return "BAD_FORMAT";
        case input::Reason::BadCharset:  return "BAD_CHARSET";
        case input::Reason::OutOfRange:  return "OUT_OF_RANGE";
        case input::Reason::NotAllowed:  return "NOT_ALLOWED";
        case input::Reason::BadChecksum: return "BAD_CHECKSUM";
        case input::Reason::Weak:        return "WEAK";
        case input::Reason::Breached:    return "BREACHED";
    }
    return "BAD_FORMAT";
}

// The bound a decoder can state, for the same reason kMaxErrorCode exists: a
// client enumerating the reasons needs to know where the list ends without
// holding a belief about how long it is.
inline constexpr input::Reason kMaxReason = input::Reason::Breached;

// THE writer for a failure body, and the only one.
//
// docs/00-architecture.md §8 has published this shape since phase 0 and nothing
// in this library wrote it: the access filter assembled `{"error":{"code":"X"}}`
// with a string concatenation, the stealth path was a constexpr with neither of
// the other two fields, and the string `request_id` appeared in no writer
// anywhere in the source. A contract a client is generated against and nobody
// keeps is how two applications come to spell one failure two ways — which is
// exactly how `wire_name(input::Reason)` came to be missing, and why docs/00's
// own example named a reason this library has never been able to produce.
//
// It lives here, beside the status table, because the two are read together and
// a body whose `code` disagrees with its status is the drift this header exists
// to prevent.
//
// `fields` is `std::span<const input::FieldError>` — the type the validators
// already return — so a key in that map is a schema constant and never a key
// taken from the request. It is written exactly when `carries_field_detail()`
// says so, which keeps "only ValidationFailed explains itself" a property of one
// function rather than of every call site: a 409 that explained which version it
// saw would be a disclosure. It is written even when the span is EMPTY, so the
// body's shape is decided by the code alone and a client parses one thing rather
// than two — a form that cannot place a server's reason on a field is a form that
// refuses to submit with nothing marked on it.
//
// **The stealth body does not go through this.** See kNotFoundBody below.
void append_error_body(std::string& out, ErrorCode code, const RequestId& request_id,
                       std::span<const input::FieldError> fields = {});

// The stealth 404 body. One constexpr string used for stealth drops, unmatched
// routes, and genuinely-missing resources on admin routes, so the three are
// byte-identical. Nginx's error_page 404 must serve exactly these bytes too.
//
// It carries NEITHER a code detail nor an id, and that is the reason it is not
// produced by append_error_body: a correlatable value a genuine 404 does not
// have is one of the tells `accesscontrol/stealth.h` enumerates beside
// `WWW-Authenticate` and `Set-Cookie`. An id here would make a stealth drop
// separable from an unmatched route by reading the body, which is the whole
// property gone.
inline constexpr std::string_view kNotFoundBody =
    R"({"error":{"code":"NOT_FOUND"}})";
inline constexpr std::string_view kNotFoundContentType = "application/json";

}  // namespace anvil::http
