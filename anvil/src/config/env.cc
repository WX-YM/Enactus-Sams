#include "anvil/config/env.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace anvil::config {

EnvLookup system_environment() noexcept {
    return [](const char* key) { return std::getenv(key); };
}

std::optional<std::string_view> lookup(const EnvLookup& env, const char* key) {
    const char* value = env(key);
    if (value == nullptr) { return std::nullopt; }
    const std::string_view text{value};
    if (text.empty()) { return std::nullopt; }
    return text;
}

std::string required_string(const EnvLookup& env, const char* key) {
    const std::optional<std::string_view> value = lookup(env, key);
    if (!value.has_value()) {
        throw ConfigError{std::string{key} + " is required and is unset or empty"};
    }
    return std::string{*value};
}

std::string optional_string(const EnvLookup& env, const char* key, std::string_view fallback) {
    const std::optional<std::string_view> value = lookup(env, key);
    return std::string{value.value_or(fallback)};
}

namespace {

[[nodiscard]] std::uint64_t parse_number(const char* key, std::string_view text) {
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        throw ConfigError{std::string{key} + " must be a non-negative integer"};
    }
    return value;
}


// Host only: the scheme and any port removed. Cookies are not port-scoped, so
// two origins that differ only by port are one host to every browser that holds
// one — which is exactly the deployment the split has to refuse.
//
// The origin has already passed require_origin_shape, so it has a scheme, a host
// and no path; the only thing left to strip is a port. An IPv6 literal is
// bracketed and full of colons, so the search for one starts after the bracket
// rather than at the end of the string.
[[nodiscard]] std::string_view host_of(std::string_view origin) noexcept {
    const std::size_t scheme_end = origin.find("://");
    const std::string_view host =
        scheme_end == std::string_view::npos ? origin : origin.substr(scheme_end + 3);
    const std::size_t search_from = host.starts_with('[') ? host.find(']') : 0;
    if (search_from == std::string_view::npos) { return host; }
    const std::size_t port_start = host.find(':', search_from);
    return port_start == std::string_view::npos ? host : host.substr(0, port_start);
}

}  // namespace

std::uint64_t optional_number(const EnvLookup& env, const char* key, std::uint64_t fallback) {
    const std::optional<std::string_view> value = lookup(env, key);
    if (!value.has_value()) { return fallback; }
    return parse_number(key, *value);
}

std::uint64_t bounded_number(const EnvLookup& env, const char* key, std::uint64_t fallback,
                             std::uint64_t min, std::uint64_t max) {
    const std::uint64_t value = optional_number(env, key, fallback);
    if (value < min || value > max) {
        throw ConfigError{std::string{key} + " is outside its permitted range (" +
                          std::to_string(min) + ".." + std::to_string(max) + ")"};
    }
    return value;
}

bool optional_bool(const EnvLookup& env, const char* key, bool fallback) {
    const std::optional<std::string_view> value = lookup(env, key);
    if (!value.has_value()) { return fallback; }

    std::string lowered{*value};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    // Not a silent false. A typo in a variable that disables a security control
    // must not read as "disabled".
    throw ConfigError{std::string{key} + " must be one of 1/0, true/false, yes/no, on/off"};
}

void require_origin_shape(const char* key, std::string_view origin) {
    const bool https = origin.rfind("https://", 0) == 0;
    const bool local_http = origin.rfind("http://localhost", 0) == 0 ||
                            origin.rfind("http://127.0.0.1", 0) == 0;
    if (!https && !local_http) {
        throw ConfigError{std::string{key} +
                          " must be an https:// origin (http:// only for localhost)"};
    }
    const std::size_t scheme_end = origin.find("://") + 3;
    if (scheme_end >= origin.size()) {
        throw ConfigError{std::string{key} + " has no host"};
    }
    if (origin.find('/', scheme_end) != std::string_view::npos) {
        throw ConfigError{std::string{key} + " must carry no path and no trailing slash"};
    }
}

void require_distinct(const char* key_a, std::string_view a, const char* key_b,
                      std::string_view b) {
    if (a == b) {
        throw ConfigError{std::string{key_a} + " and " + key_b + " must differ"};
    }
}

void require_origin_split(const char* site_key, std::string_view site_origin,
                          const char* content_key, std::string_view content_origin) {
    require_origin_shape(site_key, site_origin);
    require_origin_shape(content_key, content_origin);
    if (host_of(site_origin) == host_of(content_origin)) {
        throw ConfigError{std::string{content_key} + " must be a different host from " +
                          site_key + ": a __Host- cookie set on one is sent to the other "
                          "when they share a host, whatever their ports"};
    }
}

void require_database_name(const char* key, std::string_view name) {
    if (name.empty()) {
        throw ConfigError{std::string{key} + " must not be empty"};
    }
    // 63 bytes is the server's limit.
    if (name.size() > 63) {
        throw ConfigError{std::string{key} + " must be at most 63 bytes"};
    }
    constexpr std::string_view forbidden{"/\\. \"$*<>:|?"};
    if (name.find_first_of(forbidden) != std::string_view::npos) {
        throw ConfigError{std::string{key} +
                          " must not contain any of /\\. \"$*<>:|? or a space"};
    }
}

std::string require_directory(const char* key, std::string_view path) {
    if (path.empty() || path.front() != '/') {
        throw ConfigError{std::string{key} + " must be an absolute path"};
    }

    std::error_code ec;
    const std::filesystem::path candidate{path};

    // Checked BEFORE canonical(), which follows symlinks and would report the
    // target's nature rather than the link's. A symlinked root is a root whose
    // destination can be changed by anyone who can write the link.
    if (std::filesystem::is_symlink(candidate, ec)) {
        throw ConfigError{std::string{key} + " must not be a symlink"};
    }
    if (!std::filesystem::exists(candidate, ec) || ec) {
        throw ConfigError{std::string{key} + " does not exist"};
    }
    if (!std::filesystem::is_directory(candidate, ec) || ec) {
        throw ConfigError{std::string{key} + " is not a directory"};
    }

    const std::filesystem::path resolved = std::filesystem::canonical(candidate, ec);
    if (ec) {
        throw ConfigError{std::string{key} + " could not be resolved"};
    }
    return resolved.string();
}

RedactedSummary& RedactedSummary::line(std::string_view key, std::string_view value) {
    text_.append(key).append(1, '=').append(value).append(1, '\n');
    return *this;
}

RedactedSummary& RedactedSummary::line(std::string_view key, std::uint64_t value) {
    text_.append(key).append(1, '=').append(std::to_string(value)).append(1, '\n');
    return *this;
}

RedactedSummary& RedactedSummary::secret(std::string_view key, bool present) {
    // Never the value, and never its length: a length is a meaningful hint about
    // a short secret.
    text_.append(key).append(1, '=').append(present ? "set" : "UNSET").append(1, '\n');
    return *this;
}

}  // namespace anvil::config
