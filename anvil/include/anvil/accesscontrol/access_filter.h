#pragma once

// The Drogon adapter around decision.h.
//
// It does transport work only: pull the Cookie header, look the route up in the
// registry, call evaluate(), and turn the answer into a response or a call down
// the chain. Every security decision is in decision.h, where it can be measured
// without an HTTP stack.
//
// --- Why dependencies come from a process-wide accessor ---
//
// Drogon resolves filters by CLASS NAME through DrObject's object map, and only
// auto-created (default-constructible) filters are in that map. A filter with
// constructor arguments has no name to reference in registerHandler, so the
// signing keys, the epoch resolver and the audit sink cannot be injected
// through a constructor. They are installed once at boot instead — the same
// shape MongoPool, RedisClient and Pools already use in this codebase.
//
// --- The two orderings that are security properties ---
//
//   1. On a stealth drop the RESPONSE is sent before the audit row is enqueued.
//      An attacker's stopwatch stops when the response arrives, so audit work
//      cannot appear in the timing distribution that must overlap with a
//      nonexistent route's. The forensic record still happens; it
//      just happens after the client has been answered.
//   2. UserContext is attached as exactly ONE attribute. Drogon's attribute map
//      costs a string hash, a map node and a control-block allocation per
//      entry; five attributes cost five of each.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include <drogon/HttpFilter.h>

#include <span>

#include "anvil/accesscontrol/epoch_resolver.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/auth/token.h"
#include "anvil/core/types.h"
#include "anvil/core/user_context.h"

namespace anvil::accesscontrol {

// What the audit log needs from a denial. Deliberately NOT
// repo::AuditEntry: repositories sit above lib in the layering, and a lib
// header including one would make "dependencies point downward" advisory
// (docs/00-architecture.md §2). A service adapts between the two.
struct DenialRecord final {
    std::optional<Uuid>          actor;   // set when the token verified
    std::array<std::uint8_t, 16> ip;
    // The TRUE code, even when the client received a 404. Losing the 401/403
    // signal from client-visible responses is only an acceptable trade because
    // this field preserves it server-side.
    ErrorCode                    code;
    bool                         stealthed;
};

class DenialSink {
public:
    virtual ~DenialSink() = default;
    // Must not block and must not throw: it is called from an event-loop
    // thread, immediately after the response has been handed to the client.
    virtual void record(const DenialRecord& denial) noexcept = 0;

protected:
    DenialSink() = default;
    DenialSink(const DenialSink&) = default;
    DenialSink& operator=(const DenialSink&) = default;
    DenialSink(DenialSink&&) = default;
    DenialSink& operator=(DenialSink&&) = default;
};

struct AccessControlDeps final {
    // shared_ptr<const>: rotation swaps the whole key set atomically, so a
    // request that started with the old set finishes with it rather than seeing
    // a half-updated one (CLAUDE.md §4).
    std::shared_ptr<const auth::TokenKeys> keys;
    EpochResolver*                         epochs;
    DenialSink*                            denials;
    // The application's route table. A span over a constexpr array, so the table
    // stays in .rodata and this costs one 16-byte member on a struct the filter
    // already reads once per request (docs/01-seams.md §3).
    //
    // EMPTY is not a permissive default: policy_for returns nullptr for every
    // pattern, and the filter treats an unknown pattern as a denial. A deployment
    // that forgot to install its table serves 404s, which is loud.
    std::span<const RoutePolicy>           routes;
};

// Installed once from main(), before the first listener starts.
class AccessControl final {
public:
    static void init(AccessControlDeps deps);
    [[nodiscard]] static const AccessControlDeps& deps();
    // Whether `init` has run, asked WITHOUT the throw `deps()` answers with.
    //
    // `upgrade_gate.h` runs inside a Drogon sync advice, where an escaping
    // exception is a request the framework has no handler for. It has to be able
    // to ask the question and fail closed on its own terms rather than by
    // throwing out of the event loop, so the question is a predicate.
    [[nodiscard]] static bool initialised() noexcept;
    // For tests, which build a fresh set of dependencies per case.
    static void reset() noexcept;
};

class AccessFilter final : public drogon::HttpFilter<AccessFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                  drogon::FilterChainCallback&& fccb) override;
};

// Reads the context attached by the filter. Returns nullptr on a public route
// that no one was signed in for.
[[nodiscard]] std::shared_ptr<const UserContext> user_context(
    const drogon::HttpRequestPtr& req);

}  // namespace anvil::accesscontrol
