#include "app/config.h"

#include <string>

#include "anvil/config/paths.h"
#include "anvil/http/client_address.h"

namespace enactus {

namespace cfg = anvil::config;

Config load_config(const cfg::EnvLookup& env) {
    Config config{};

    config.bind_addr = cfg::optional_string(env, "BIND_ADDR", "127.0.0.1");
    config.port = static_cast<std::uint16_t>(cfg::bounded_number(env, "PORT", 8085, 1, 65535));
    config.doc_root = cfg::require_directory("DOC_ROOT", cfg::required_string(env, "DOC_ROOT"));
    config.storage_root = cfg::require_directory("STORAGE_ROOT", cfg::required_string(env, "STORAGE_ROOT"));
    config.defaults_dir =
        cfg::require_directory("DEFAULTS_DIR", cfg::required_string(env, "DEFAULTS_DIR"));

    // Uploaded media must never be reachable without passing through the
    // access-checked media route (anvil docs/00-architecture.md §7.4).
    if (cfg::is_web_served_root(config.storage_root) ||
        cfg::path_is_within(config.doc_root, config.storage_root)) {
        throw cfg::ConfigError{"STORAGE_ROOT must not be inside a web-served directory"};
    }

    config.mongodb_uri = cfg::required_string(env, "MONGODB_URI");
    config.mongo_database = cfg::optional_string(env, "MONGO_DATABASE", "application");
    cfg::require_database_name("MONGO_DATABASE", config.mongo_database);
    config.redis_url = cfg::optional_string(env, "REDIS_URL", "tcp://127.0.0.1:6379");

    config.site_origin = cfg::required_string(env, "SITE_ORIGIN");
    cfg::require_origin_shape("SITE_ORIGIN", config.site_origin);

    config.trusted_proxies = cfg::optional_string(env, "TRUSTED_PROXIES", "");
    if (!config.trusted_proxies.empty()) {
        anvil::http::TrustedProxies probe;
        if (!probe.parse(config.trusted_proxies)) {
            throw cfg::ConfigError{"TRUSTED_PROXIES must be a comma-separated list of CIDRs"};
        }
    }

    cfg::load_key(env, "TOKEN_SIGNING_KEY", config.token_signing_key, true);
    cfg::load_key(env, "SESSION_PEPPER", config.session_pepper, true);
    cfg::load_key(env, "PREHASH_PEPPER", config.prehash_pepper, true);
    config.prehash_pepper_id = cfg::optional_string(env, "PREHASH_PEPPER_ID", "p1");
    if (config.prehash_pepper_id.empty() || config.prehash_pepper_id.size() > 16) {
        throw cfg::ConfigError{"PREHASH_PEPPER_ID must be 1-16 characters of [a-z0-9]"};
    }
    for (const char c : config.prehash_pepper_id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            throw cfg::ConfigError{"PREHASH_PEPPER_ID must be 1-16 characters of [a-z0-9]"};
        }
    }
    cfg::load_key(env, "PREHASH_SALT_KEY", config.prehash_salt_key, true);
    cfg::load_key(env, "CODE_PEPPER", config.code_pepper, true);
    cfg::load_key(env, "ADDRESS_INDEX_KEY", config.address_index_key, true);
    cfg::load_key(env, "ANALYTICS_VISITOR_PEPPER", config.analytics_visitor_pepper, true);
    cfg::load_key(env, "PII_SEAL_KEY", config.pii_seal_key, true);
    cfg::load_key(env, "PII_INDEX_KEY", config.pii_index_key, true);

    config.db_threads = cfg::bounded_number(env, "DB_POOL_THREADS", 16, 2, 256);
    config.hash_threads = cfg::bounded_number(env, "HASH_POOL_THREADS", 4, 1, 32);
    config.http_threads = cfg::bounded_number(env, "HTTP_THREADS", 4, 1, 64);
    return config;
}

std::string summarise(const Config& config) {
    return cfg::RedactedSummary{}
        .line("BIND_ADDR", config.bind_addr)
        .line("PORT", config.port)
        .line("DOC_ROOT", config.doc_root)
        .line("STORAGE_ROOT", config.storage_root)
        .line("DEFAULTS_DIR", config.defaults_dir)
        .line("MONGO_DATABASE", config.mongo_database)
        .line("SITE_ORIGIN", config.site_origin)
        .line("TRUSTED_PROXIES", config.trusted_proxies.empty() ? "(none: this process is the edge)"
                                                                 : config.trusted_proxies)
        .secret("TOKEN_SIGNING_KEY", true)
        .secret("SESSION_PEPPER", true)
        .secret("PREHASH_PEPPER", true)
        .line("PREHASH_PEPPER_ID", config.prehash_pepper_id)
        .secret("PREHASH_SALT_KEY", true)
        .secret("CODE_PEPPER", true)
        .secret("ADDRESS_INDEX_KEY", true)
        .secret("ANALYTICS_VISITOR_PEPPER", true)
        .secret("PII_SEAL_KEY", true)
        .secret("PII_INDEX_KEY", true)
        .line("DB_POOL_THREADS", config.db_threads)
        .line("HASH_POOL_THREADS", config.hash_threads)
        .line("HTTP_THREADS", config.http_threads)
        .str();
}

}  // namespace enactus
