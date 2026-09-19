#pragma once

// The half of a denial that is not the response.
//
// Private to `src/`, because it exists for exactly one reason: TWO places in
// this library answer a denied request. The access filter answers most of them,
// and `accesscontrol/upgrade_gate.h` answers one class of them earlier — before
// the framework has built the object whose destructor leaks four bytes past the
// response. The RESPONSE differs between the two (one hands it to a filter
// callback, the other returns it to the framework); the counter and the audit
// row must not, and a second spelling of "what a denial records" is how one of
// the two quietly stops recording something.
//
// Not in `include/anvil/`: nothing outside this library calls it, and a helper
// whose only two callers are in one directory does not belong in the ABI.

#include <memory>
#include <optional>

#include "anvil/core/types.h"

namespace drogon {
class HttpRequest;
}  // namespace drogon

namespace anvil::accesscontrol::detail {

// The denial counter and the audit row, in that order.
//
// It never builds a response and it must never be called before one has been
// handed to the client. `DenialSink::record` says so in its own contract, and
// the reason is the stealth claim: a denied route and a nonexistent one are
// timed the same only while the work that distinguishes them happens after the
// bytes are gone (accesscontrol/stealth.h).
void record_denial(const std::shared_ptr<drogon::HttpRequest>& req, bool stealth, ErrorCode code,
                   const std::optional<Uuid>& actor) noexcept;

}  // namespace anvil::accesscontrol::detail
