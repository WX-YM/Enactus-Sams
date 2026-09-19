#pragma once

// The per-request state anvil attaches, and the single attribute that carries
// it.
//
// `core/user_context.h` argues at length for exactly ONE attribute: Drogon's
// `req->attributes()` is a `std::map<std::string, std::any>`, so every entry
// costs a string compare per level of the tree, a map node, and — because a
// `shared_ptr` does not fit `std::any`'s small-buffer slot — a second heap
// allocation beside the control block. Five attributes cost five of each on
// every request.
//
// The request id has to be minted before routing, so it exists on a request
// that is refused before any filter runs, and the context is filled during the
// filter. Values with different lifetimes, one attribute: this struct.
//
// --- why the context is first, and why that is not cosmetic -----------------
//
// `UserContext` is 64 bytes and is asserted to be exactly one cache line. It is
// read on every protected request, and it is the only member the request path
// reads at all. So it sits at offset 0, where the line it occupies is the line
// the allocation starts on, and the id, the flag and the trace live past it in
// the second line — allocated on every request, touched on none of the hot ones.
//
// --- and why the whole thing is one allocation ------------------------------
//
// The scope is created by the pre-routing advice and FILLED by the filter, so
// it is mutable for a window. That window is one event-loop thread: Drogon runs
// the pre-routing observers, the filter chain and the handler for a request on
// the loop that owns the connection, in that order, with no thread hop between
// them. Readers outside the filter are handed `const UserContext&` through
// `accesscontrol::user_context()`, which aliases this object's control block
// rather than copying it, so a context that crosses a thread-pool boundary
// keeps the scope alive and costs no second allocation.

#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>

#include "anvil/core/user_context.h"
#include "anvil/http/request_id.h"
#include "anvil/http/trace_context.h"

// Declared, not included. The struct below is a plain aggregate over three types
// the foundation library already owns; only the functions at the bottom need a
// request, and they are defined in src/http/request_scope.cc, which is compiled
// into anvil::platform. Same split as client_address.h, for the same
// reason: anvil::foundation does not link Drogon.
namespace drogon {
class HttpRequest;
}  // namespace drogon

namespace anvil::http {

struct RequestScope final {
    // Offset 0, and asserted below. See the header comment.
    UserContext ctx;          // 64
    // Minted before routing, so a request refused by the framework's own
    // unmatched-route path still has one to log against.
    RequestId   request_id;   // 16
    // Whether `ctx` was ever filled, which is NOT the same question as whether
    // `ctx.user_id` is nil.
    //
    // A zeroed context would answer the second question for every route this
    // library has, and it answers it by believing that no token can name the
    // nil UUID. That is a property of an application's token minting, not of
    // this library, and "is anybody signed in" is the wrong place to start
    // trusting a sentinel. The filter sets this exactly where it used to decide
    // whether to insert the attribute at all, so the semantics are unchanged by
    // the move.
    bool        has_context;  //  1
    // APPENDED, rather than ordered by alignment as ENGINEERING_RULES.md §3.2 asks.
    //
    // Everything from `request_id` down has alignment 1, so there is no packing
    // to win by moving it: the struct is 106 bytes of members either way. What
    // appending buys is that the two asserted offsets below do not move, which
    // is what keeps this an additive change to a struct applications read.
    //
    // All-zero is absent, and it is absent on every request under the default
    // policy. `TraceContext` has no separate empty flag because the standard
    // forbids an all-zero trace-id, so the zero value is already spoken for
    // (http/trace_context.h).
    TraceContext trace;       // 25
};

// 112 rather than 106: `UserContext` has alignment 8, so the members are
// followed by six bytes of tail padding. Stated rather than trimmed — the
// alternative is packing the flag into `UserContext::reserved`, which would
// spend that reserve on a fact that is not about the context.
//
// It was 88 before the trace context. The 24 bytes are in the allocation that
// already happens per request rather than in a SECOND attribute, which is the
// cost `core/user_context.h` measured and the reason this struct exists at all:
// a second entry in Drogon's attribute map is a string hash, a map node and a
// control-block allocation, every request, traced or not.
static_assert(sizeof(RequestScope) == 112);
static_assert(alignof(RequestScope) == 8);
static_assert(std::is_standard_layout_v<RequestScope>);
static_assert(offsetof(RequestScope, ctx) == 0,
              "the filter's read must start on the allocation's first cache line");
static_assert(offsetof(RequestScope, request_id) == 64,
              "the id must sit past the line UserContext occupies, not inside it");
// It crosses thread-pool boundaries by value wherever a caller prefers a copy to
// keeping the scope alive, for the same reason UserContext does.
static_assert(std::is_trivially_copyable_v<RequestScope>);

// ONE key, declared beside the type it carries.
//
// It replaces `kUserContextKey`, which named the same attribute for a value that
// is now a member of this one. There is deliberately no second constant left
// behind: two constants naming one key is the trap `core/user_context.h`
// records, where code written against one of them read an attribute nothing had
// ever set and got a null context with no error anywhere.
//
// Short, because the map is keyed by `std::string` and this one has to fit the
// small-string optimisation or every lookup allocates.
inline constexpr std::string_view kRequestScopeKey = "anvil.req";

// The header the id goes out as, in its canonical spelling.
//
// Drogon lowercases a field name when it stores one, so `x-request-id` is what
// crosses the socket. That is not a problem — RFC 9110 makes field names
// case-insensitive and every client reads it the same — and the constant keeps
// the readable form because it is also what a person looks for in a browser's
// network tab before quoting it into a support ticket.
inline constexpr std::string_view kRequestIdHeader = "X-Request-Id";

// Installs both halves of the wiring, once, before the first listener starts.
//
// Two advices, and they install together on purpose. The first mints the id and
// creates the scope in the earliest pre-routing observer, which is the last
// point before Drogon can answer a request without consulting anything of ours.
// The second sends that id back as `X-Request-Id`. Installing one without the
// other is a half-kept contract in either direction — an id nobody can quote, or
// a header that is always empty — and neither failure produces a symptom the
// deployment that has it would notice.
//
// Idempotent: a second call installs nothing. Drogon cannot unregister an
// advice, so a double install would be a second mint overwriting the first, and
// a library whose boot function is unsafe to call twice is a library that gets
// called twice.
//
// `ingest` is the trace-context policy, and it is `Off` unless an application
// asks otherwise — see `TraceIngest` in `http/trace_context.h` for why a header
// a client can type is not believed by default. A defaulted parameter, so no
// existing call site and no consumer's build changes.
void install_request_scope(TraceIngest ingest = TraceIngest::Off);

// The scope for this request, or nullptr when the advice above was never
// installed — and on one framework path that answers before routing, the
// `OPTIONS *` reply Drogon builds itself.
[[nodiscard]] std::shared_ptr<const RequestScope> request_scope(
    const std::shared_ptr<drogon::HttpRequest>& req);

// The id minted for this request, all-zero when there is no scope.
//
// All-zero renders as 26 `0` characters, which is a value no mint can produce —
// the timestamp half is never zero on a running clock. So an id that reads
// `00000000000000000000000000` in a log or a body says exactly one thing, and it
// says it loudly: `install_request_scope()` was not called. That is better than
// minting one here, which would answer a different id to every caller within one
// request and correlate to nothing.
[[nodiscard]] RequestId request_id_of(
    const std::shared_ptr<drogon::HttpRequest>& req) noexcept;

// The trace this request arrived carrying, absent unless `install_request_scope`
// was given a policy that believes one and this request came from a hop that
// policy trusts.
//
// This is the accessor the design line points at: anvil owns the context and the
// application owns the export. An application that wants its own work correlated
// installs it as the ambient context for the duration of its handler —
//
//     const http::TraceScope scope{http::trace_of(req)};
//
// — and everything anvil posts to a pool from inside that scope carries it,
// because `guarded()` samples the ambient context at post time. That one line is
// the application's because Drogon has no advice that WRAPS a handler: the
// pre-handling advice returns before the handler runs, so a scope installed
// there would be gone by the time there was anything to correlate.
[[nodiscard]] TraceContext trace_of(
    const std::shared_ptr<drogon::HttpRequest>& req) noexcept;

// Fills the context half of the scope. Called by the access filter, on the loop
// thread that owns the request, before any handler runs — see the header
// comment for why that is the whole synchronisation argument.
//
// Not a general-purpose setter. A second call overwrites the authority an
// earlier one established, so it refuses one and says so: returns false if this
// request already carries a context, or if it has no scope at all.
[[nodiscard]] bool attach_user_context(const std::shared_ptr<drogon::HttpRequest>& req,
                                       const UserContext& ctx) noexcept;

}  // namespace anvil::http
