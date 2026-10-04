#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <chrono>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <json/json.h>
#include "anvil/crypto/digest.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/constant_time.h"

namespace enactus::security {

struct JwtClaims {
    std::string email;
    std::string role;
    std::string team;
    std::vector<std::string> permissions;
    int64_t exp = 0;
    int64_t iat = 0;
};

// The signing key. There is deliberately no built-in fallback: a default key
// published in the repository lets anyone mint a token for any account, and
// the auth layer trusts the token's subject. `initJwtSecret()` is called once
// at boot and refuses to start the server without a strong key.
inline constexpr std::size_t kMinJwtSecretBytes = 32;

inline std::string& jwtSecretStorage() {
    static std::string secret;
    return secret;
}

inline void initJwtSecret() {
    const char* env_secret = std::getenv("JWT_SECRET");
    if (!env_secret || std::strlen(env_secret) < kMinJwtSecretBytes) {
        throw std::runtime_error(
            "JWT_SECRET must be set to a random value of at least 32 bytes "
            "(e.g. `openssl rand -base64 48`)");
    }
    jwtSecretStorage() = env_secret;
}

inline const std::string& getJwtSecret() {
    const std::string& secret = jwtSecretStorage();
    if (secret.empty()) {
        throw std::logic_error("JWT secret used before initJwtSecret()");
    }
    return secret;
}

// Base64URL for {"alg":"HS256","typ":"JWT"}. The only header this server
// issues, and the only one it accepts.
inline constexpr std::string_view kJwtHeaderB64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
inline constexpr std::size_t kMaxTokenBytes = 4096;

inline std::string signToken(const JwtClaims& claims) {
    static constexpr std::string_view header_b64 = kJwtHeaderB64;

    Json::Value payload;
    payload["sub"] = claims.email;
    payload["role"] = claims.role;
    payload["team"] = claims.team;
    Json::Value perms(Json::arrayValue);
    for (const auto& p : claims.permissions) {
        perms.append(p);
    }
    payload["permissions"] = perms;
    payload["exp"] = static_cast<Json::Value::Int64>(claims.exp);
    payload["iat"] = static_cast<Json::Value::Int64>(claims.iat);

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string payload_str = Json::writeString(writer, payload);
    std::string payload_b64 = anvil::crypto::base64url_encode(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload_str.data()), payload_str.size())
    );

    std::string to_sign = std::string(header_b64) + "." + payload_b64;
    const std::string& secret = getJwtSecret();
    auto hmac = anvil::crypto::hmac_sha256(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(secret.data()), secret.size()),
        to_sign
    );

    std::string sig_b64 = anvil::crypto::base64url_encode(hmac);
    return to_sign + "." + sig_b64;
}

inline std::optional<JwtClaims> verifyToken(std::string_view token) {
    if (token.empty() || token.size() > kMaxTokenBytes) return std::nullopt;
    size_t first_dot = token.find('.');
    if (first_dot == std::string_view::npos) return std::nullopt;
    size_t second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos) return std::nullopt;

    std::string_view header_b64 = token.substr(0, first_dot);
    std::string_view payload_b64 = token.substr(first_dot + 1, second_dot - (first_dot + 1));
    std::string_view sig_b64 = token.substr(second_dot + 1);
    if (header_b64 != kJwtHeaderB64 || payload_b64.empty() ||
        sig_b64.find('.') != std::string_view::npos) {
        return std::nullopt;
    }

    std::string to_sign = std::string(token.substr(0, second_dot));
    const std::string& secret = getJwtSecret();
    auto expected_hmac = anvil::crypto::hmac_sha256(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(secret.data()), secret.size()),
        to_sign
    );
    std::string expected_sig_b64 = anvil::crypto::base64url_encode(expected_hmac);

    if (sig_b64.size() != expected_sig_b64.size() ||
        !anvil::crypto::secure_equal(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(sig_b64.data()), sig_b64.size()),
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(expected_sig_b64.data()), expected_sig_b64.size())
        )) {
        return std::nullopt;
    }

    auto decoded_bytes = anvil::crypto::base64url_decode(payload_b64);
    if (!decoded_bytes) return std::nullopt;

    std::string payload_str(decoded_bytes->begin(), decoded_bytes->end());
    Json::CharReaderBuilder reader_builder;
    std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
    Json::Value payload;
    std::string errs;
    if (!reader->parse(payload_str.data(), payload_str.data() + payload_str.size(), &payload, &errs)) {
        return std::nullopt;
    }

    int64_t now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    if (!payload.isObject() ||
        !payload["exp"].isIntegral() || payload["exp"].asInt64() <= now_sec ||
        !payload["iat"].isIntegral() || payload["iat"].asInt64() > now_sec + 60 ||
        !payload["sub"].isString() || payload["sub"].asString().empty()) {
        return std::nullopt; // expired, malformed, or issued in the future
    }

    JwtClaims claims;
    claims.email = payload["sub"].asString();
    claims.role = payload["role"].isString() ? payload["role"].asString() : "";
    claims.team = payload["team"].isString() ? payload["team"].asString() : "";
    claims.exp = payload["exp"].asInt64();
    claims.iat = payload["iat"].asInt64();
    if (payload["permissions"].isArray()) {
        for (const auto& p : payload["permissions"]) {
            if (p.isString()) claims.permissions.push_back(p.asString());
        }
    }
    return claims;
}

} // namespace enactus::security
