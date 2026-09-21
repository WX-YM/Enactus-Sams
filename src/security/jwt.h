#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <chrono>
#include <cstring>
#include <vector>
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

inline std::string getJwtSecret() {
    const char* env_secret = std::getenv("JWT_SECRET");
    if (env_secret && std::strlen(env_secret) > 0) {
        return std::string(env_secret);
    }
    return "enactus_sams_jwt_secret_key_2026_super_secure_vault";
}

inline std::string signToken(const JwtClaims& claims) {
    // Header: {"alg":"HS256","typ":"JWT"}
    // Base64URL for {"alg":"HS256","typ":"JWT"} is eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9
    static constexpr std::string_view header_b64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

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
    std::string secret = getJwtSecret();
    auto hmac = anvil::crypto::hmac_sha256(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(secret.data()), secret.size()),
        to_sign
    );

    std::string sig_b64 = anvil::crypto::base64url_encode(hmac);
    return to_sign + "." + sig_b64;
}

inline std::optional<JwtClaims> verifyToken(std::string_view token) {
    size_t first_dot = token.find('.');
    if (first_dot == std::string_view::npos) return std::nullopt;
    size_t second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos) return std::nullopt;

    std::string_view header_b64 = token.substr(0, first_dot);
    std::string_view payload_b64 = token.substr(first_dot + 1, second_dot - (first_dot + 1));
    std::string_view sig_b64 = token.substr(second_dot + 1);

    std::string to_sign = std::string(token.substr(0, second_dot));
    std::string secret = getJwtSecret();
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

    if (!payload.isMember("exp") || payload["exp"].asInt64() <= now_sec) {
        return std::nullopt; // expired
    }

    JwtClaims claims;
    claims.email = payload["sub"].asString();
    claims.role = payload["role"].asString();
    claims.team = payload.isMember("team") ? payload["team"].asString() : "";
    claims.exp = payload["exp"].asInt64();
    claims.iat = payload.isMember("iat") ? payload["iat"].asInt64() : 0;
    if (payload.isMember("permissions") && payload["permissions"].isArray()) {
        for (const auto& p : payload["permissions"]) {
            claims.permissions.push_back(p.asString());
        }
    }
    return claims;
}

} // namespace enactus::security
