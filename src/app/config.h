#pragma once

// The backend's configuration, read once at boot through anvil's typed readers
// (anvil docs/14-config.md). A missing or malformed value is a boot failure,
// never a degraded mode: a server that starts with no signing key is worse than
// one that does not start.

#include <cstdint>
#include <string>

#include "anvil/auth/token.h"
#include "anvil/config/env.h"
#include "anvil/crypto/secret.h"

namespace enactus {

struct Config final {
    // Where and how this process listens.
    std::string   bind_addr;
    std::uint16_t port;
    std::string   doc_root;        // the public website and the built admin panel
    std::string   storage_root;    // uploaded media; never web-served
    std::string   defaults_dir;    // default images seeded into a fresh database

    // Backing services.
    std::string mongodb_uri;
    std::string mongo_database;
    std::string redis_url;

    // The one origin a state-changing request may come from (anvil
    // http/origin_check.h), e.g. https://enactussams.org.
    std::string site_origin;
    // CIDRs of the reverse proxies in front of this process. Empty means this
    // process is the edge (anvil docs/00-architecture.md §4.1).
    std::string trusted_proxies;

    // Every secret is 32 CSPRNG bytes of unpadded base64url, loaded straight
    // into zeroing storage. See scripts/generate-secrets.sh.
    anvil::crypto::SecretBuffer<anvil::auth::TokenKeys::kKeyBytes> token_signing_key;
    anvil::crypto::SecretBuffer<32> session_pepper;
    anvil::crypto::SecretBuffer<32> prehash_pepper;
    std::string                     prehash_pepper_id;
    anvil::crypto::SecretBuffer<32> prehash_salt_key;
    anvil::crypto::SecretBuffer<32> code_pepper;
    anvil::crypto::SecretBuffer<32> address_index_key;
    anvil::crypto::SecretBuffer<32> analytics_visitor_pepper;
    anvil::crypto::SecretBuffer<32> pii_seal_key;
    anvil::crypto::SecretBuffer<32> pii_index_key;

    // Pool sizing (anvil docs/00-architecture.md §3).
    std::uint64_t db_threads;
    std::uint64_t hash_threads;
    std::uint64_t http_threads;
};

[[nodiscard]] Config load_config(const anvil::config::EnvLookup& env);

// The boot summary: every value, every secret as set/UNSET, never a secret's
// value or length.
[[nodiscard]] std::string summarise(const Config& config);

}  // namespace enactus
