#pragma once

// The boot assertion that the server we connected to is actually one we
// support (docs/09-mongodb.md §7).
//
// Two separate facts, and checking only the first is the common mistake:
//
//   1. The BINARY version, from buildInfo. Below the floor, features this
//      service relies on either do not exist or behave differently.
//   2. featureCompatibilityVersion. Upgrading the binaries does NOT move FCV —
//      an in-place upgrade that nobody finished leaves a server reporting 7.0
//      that still refuses 7.0 behaviour. That is not a startup failure, because
//      the deployment is recoverable and refusing to boot on it turns a
//      half-finished maintenance window into an outage; it is logged
//      prominently instead.

#include <cstdint>
#include <string>

#include <mongocxx/client.hpp>

namespace anvil::db {

// The floor is a compile-time constant so the check and the documentation
// cannot drift.
inline constexpr std::int32_t kMinServerMajor = 7;
inline constexpr std::int32_t kMinServerMinor = 0;

struct ServerCheck final {
    std::string   version;                  // as reported by buildInfo
    std::string   feature_compatibility;    // empty when the server would not say
    std::int32_t  major;
    std::int32_t  minor;
    bool          is_replica_set;
    bool          feature_compatibility_lags;
};

// Throws std::runtime_error naming the version found and the version required
// when the server is below the floor or is not a replica set. Returns the
// details otherwise, with feature_compatibility_lags set for the caller to log.
[[nodiscard]] ServerCheck assert_server_supported(mongocxx::client& client);

}  // namespace anvil::db
