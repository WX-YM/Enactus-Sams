#pragma once

// The narrow interface the access filter needs from perm_epoch resolution.
//
// It exists so the filter — which lives in lib/ — does not include a service.
// Dependencies point downward only (docs/00-architecture.md §2), and a lib
// header reaching up into services/ would make that rule advisory rather than a
// link error. services::AuthzService implements this; the filter only ever sees
// the two calls below.
//
// The split into a synchronous check and an asynchronous resolve is the whole
// design of the hot path:
//
//   check_cached   pure CPU, no allocation, safe on a Trantor event-loop
//                  thread. Answers the overwhelming majority of requests.
//   resolve_async  a Redis GET and, rarely, a MongoDB read. BLOCKING work that
//                  must be handed to a worker pool; the callback may run on any
//                  thread.

#include <cstdint>
#include <functional>

#include "anvil/core/result.h"
#include "anvil/core/types.h"

namespace anvil::accesscontrol {

enum class EpochVerdict : std::uint8_t {
    Match,      // the authority is cached locally and agrees with the token
    Mismatch,   // the authority is cached locally and disagrees: deny
    Unknown,    // not cached: resolve_async must answer
};

class EpochResolver {
public:
    virtual ~EpochResolver() = default;

    [[nodiscard]] virtual EpochVerdict check_cached(const Uuid& user_id,
                                                    std::uint64_t token_epoch) const noexcept = 0;

    // Invokes `done` exactly once, with the authoritative epoch or with a
    // Failure. A Failure is a DENIAL, never an allow: an unreachable revocation
    // channel must not read as "no revocations".
    virtual void resolve_async(const Uuid& user_id,
                               std::function<void(Result<std::uint64_t>)> done) = 0;

protected:
    EpochResolver() = default;
    EpochResolver(const EpochResolver&) = default;
    EpochResolver& operator=(const EpochResolver&) = default;
    EpochResolver(EpochResolver&&) = default;
    EpochResolver& operator=(EpochResolver&&) = default;
};

}  // namespace anvil::accesscontrol
