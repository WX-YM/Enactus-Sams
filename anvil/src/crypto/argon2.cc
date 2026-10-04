#include "anvil/crypto/argon2.h"

#include <argon2.h>

#include <array>
#include <charconv>
#include <limits>

#include "anvil/crypto/errors.h"

namespace anvil::crypto {
namespace {

// Comfortably above the encoded form for any parameters this codebase uses;
// argon2 reports ARGON2_OUTPUT_TOO_SHORT rather than overflowing if this were
// ever too small.
constexpr std::size_t kEncodedBufferBytes = 256;

[[nodiscard]] std::uint8_t* mutable_data(std::span<const std::uint8_t> bytes) noexcept {
    // libargon2's C API takes non-const pointers throughout, including for
    // inputs it only reads. ARGON2_DEFAULT_FLAGS (passed below) never asks it to
    // write through pwd/salt/secret/ad, and the caller owns cleansing its own
    // buffers afterwards (CLAUDE.md §5) — this cast adds no mutation the library
    // does not already promise not to perform.
    return const_cast<std::uint8_t*>(bytes.data());
}

// Reads "name=<digits>" out of an Argon2 PHC string. from_chars neither
// allocates nor consults the locale, and std::regex is banned on any path this
// primitive can end up on (CLAUDE.md §5).
[[nodiscard]] std::optional<std::uint32_t> read_param(std::string_view encoded,
                                                      std::string_view name) noexcept {
    const std::size_t key = encoded.find(name);
    if (key == std::string_view::npos) { return std::nullopt; }

    const std::size_t start = key + name.size();
    std::size_t end = start;
    while (end < encoded.size() && encoded[end] >= '0' && encoded[end] <= '9') { ++end; }
    if (end == start) { return std::nullopt; }

    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(encoded.data() + start, encoded.data() + end, value);
    if (ec != std::errc{}) { return std::nullopt; }
    return value;
}

}  // namespace

void argon2_hash_raw(Argon2Type type, std::uint32_t version, const Argon2Params& params,
                     std::span<const std::uint8_t> password, std::span<const std::uint8_t> salt,
                     std::span<const std::uint8_t> secret,
                     std::span<const std::uint8_t> associated_data, std::span<std::uint8_t> out) {
    // argon2_context carries every length as uint32_t. A span over 4 GiB cast
    // straight down would hash a silently truncated prefix — a different
    // credential than the caller passed, with no error (CLAUDE.md §5).
    constexpr std::size_t kMaxLength = std::numeric_limits<std::uint32_t>::max();
    if (out.size() > kMaxLength || password.size() > kMaxLength || salt.size() > kMaxLength ||
        secret.size() > kMaxLength || associated_data.size() > kMaxLength) {
        throw CryptoError{"argon2 input exceeds the 32-bit length field"};
    }

    argon2_context context{};
    context.out = out.data();
    context.outlen = static_cast<std::uint32_t>(out.size());
    context.pwd = mutable_data(password);
    context.pwdlen = static_cast<std::uint32_t>(password.size());
    context.salt = mutable_data(salt);
    context.saltlen = static_cast<std::uint32_t>(salt.size());
    // A zero-length secret/ad is passed as a null pointer: argon2_context's own
    // validation refuses a non-null pointer paired with a zero length exactly as
    // readily as it refuses the reverse, and an empty span's .data() is
    // unspecified rather than guaranteed null.
    context.secret = secret.empty() ? nullptr : mutable_data(secret);
    context.secretlen = static_cast<std::uint32_t>(secret.size());
    context.ad = associated_data.empty() ? nullptr : mutable_data(associated_data);
    context.adlen = static_cast<std::uint32_t>(associated_data.size());
    context.t_cost = params.iterations;
    context.m_cost = params.memory_kib;
    context.lanes = params.parallelism;
    context.threads = params.parallelism;
    context.version = version;
    context.allocate_cbk = nullptr;
    context.free_cbk = nullptr;
    context.flags = ARGON2_DEFAULT_FLAGS;

    int rc = ARGON2_INCORRECT_TYPE;
    switch (type) {
        case Argon2Type::Argon2d:  rc = argon2d_ctx(&context);  break;
        case Argon2Type::Argon2i:  rc = argon2i_ctx(&context);  break;
        case Argon2Type::Argon2id: rc = argon2id_ctx(&context); break;
    }
    if (rc != ARGON2_OK) {
        throw CryptoError{std::string{"argon2 hashing failed: "} + argon2_error_message(rc)};
    }
}

std::string argon2id_hash_encoded(const Argon2Params& params,
                                  std::span<const std::uint8_t> password,
                                  std::span<const std::uint8_t> salt, std::size_t hash_bytes) {
    std::array<char, kEncodedBufferBytes> encoded{};
    // Global scope: this translation unit's own wrapper shares libargon2's
    // name, and an unqualified call here would recurse into itself.
    const int rc = ::argon2id_hash_encoded(
        params.iterations, params.memory_kib, params.parallelism, password.data(),
        password.size(), salt.data(), salt.size(), hash_bytes, encoded.data(), encoded.size());
    if (rc != ARGON2_OK) {
        throw CryptoError{std::string{"argon2id encoding failed: "} + argon2_error_message(rc)};
    }
    return std::string{encoded.data()};
}

VerifyOutcome argon2id_verify_encoded(std::string_view encoded,
                                      std::span<const std::uint8_t> password) {
    if (encoded.empty()) { return VerifyOutcome::Malformed; }

    // argon2id_verify takes a NUL-terminated encoded string.
    const std::string encoded_z{encoded};
    const int rc = ::argon2id_verify(encoded_z.c_str(), password.data(), password.size());

    if (rc == ARGON2_OK) { return VerifyOutcome::Match; }
    if (rc == ARGON2_VERIFY_MISMATCH) { return VerifyOutcome::Mismatch; }
    return VerifyOutcome::Malformed;
}

std::optional<Argon2Params> argon2id_parse_params(std::string_view encoded) noexcept {
    if (encoded.find("$argon2id$") != 0) { return std::nullopt; }

    const std::optional<std::uint32_t> memory = read_param(encoded, "m=");
    const std::optional<std::uint32_t> iterations = read_param(encoded, "t=");
    const std::optional<std::uint32_t> parallelism = read_param(encoded, "p=");

    if (!memory.has_value() || !iterations.has_value() || !parallelism.has_value()) {
        return std::nullopt;
    }
    return Argon2Params{
        .memory_kib = *memory,
        .iterations = *iterations,
        .parallelism = *parallelism,
    };
}

}  // namespace anvil::crypto
