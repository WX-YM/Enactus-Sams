#include "anvil/auth/password.h"

#include <argon2.h>
#include <openssl/crypto.h>

#include <array>
#include <charconv>
#include <stdexcept>

#include "anvil/crypto/errors.h"
#include "anvil/crypto/random.h"
#include "anvil/i18n/normalize.h"

namespace anvil::auth {
namespace {

constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;

// Comfortably above the encoded form for our parameters; argon2 reports
// ARGON2_OUTPUT_TOO_SHORT rather than overflowing if this were ever too small.
constexpr std::size_t kEncodedBufferBytes = 256;

// A fixed password for the dummy verify. Its value is irrelevant — only the
// work performed matters — but it must not be a plausible real password, so
// that a stored hash of it could never match a user's.
constexpr std::string_view kDummyPassword = "\x01 anvil timing equaliser \x01";

// Reads "name=<digits>" out of an Argon2 encoded string. Hand-written rather
// than sscanf: from_chars neither allocates nor consults the locale, and
// std::regex is banned on any path reachable from a request (ENGINEERING_RULES.md §5).
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

// NFC, and nothing else. Applied identically on both paths.
[[nodiscard]] std::string normalize_password(std::string_view password) {
    std::optional<std::string> normalized =
        i18n::normalize(password, i18n::NormalizeMode::Nfc);
    // ICU failing is a configuration or memory problem, not bad input. Falling
    // back to the raw bytes would produce a hash that a later successful
    // normalisation could never reproduce, locking the user out permanently.
    if (!normalized.has_value()) {
        throw crypto::CryptoError{"password normalisation failed"};
    }
    return std::move(*normalized);
}

}  // namespace

std::optional<Argon2Params> parse_encoded_params(std::string_view encoded) noexcept {
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

PasswordHasher::PasswordHasher(Argon2Params params)
    : params_{params},
      // Computed once here, single-threaded at construction, so the timing
      // equaliser costs nothing extra on the login path and no lazy
      // initialisation races between concurrent logins.
      dummy_encoded_{} {
    if (params_.memory_kib < 8192 || params_.iterations < 2 || params_.parallelism < 1) {
        throw std::invalid_argument{"Argon2 parameters below policy"};
    }
    dummy_encoded_ = hash(kDummyPassword);
}

std::string PasswordHasher::hash(std::string_view password) const {
    if (password.size() > kMaxPasswordBytes) {
        throw std::invalid_argument{"password exceeds the hard byte cap"};
    }

    std::string normalized = normalize_password(password);
    const std::array<std::uint8_t, kSaltBytes> salt = crypto::random_array<kSaltBytes>();

    std::array<char, kEncodedBufferBytes> encoded{};
    const int rc = argon2id_hash_encoded(
        params_.iterations, params_.memory_kib, params_.parallelism, normalized.data(),
        normalized.size(), salt.data(), salt.size(), kHashBytes, encoded.data(),
        encoded.size());

    // The plaintext is gone as soon as it is no longer needed. A plain memset
    // here would be removed by the optimiser as a dead store.
    OPENSSL_cleanse(normalized.data(), normalized.size());

    if (rc != ARGON2_OK) {
        throw crypto::CryptoError{std::string{"argon2id hashing failed: "} +
                                  argon2_error_message(rc)};
    }
    return std::string{encoded.data()};
}

VerifyOutcome PasswordHasher::verify(std::string_view encoded,
                                     std::string_view password) const {
    // An oversized candidate is rejected without hashing. Argon2's cost is
    // driven by the parameters rather than the input length, but there is no
    // reason to copy and normalise a megabyte to reject it.
    if (password.size() > kMaxPasswordBytes) { return VerifyOutcome::Mismatch; }
    if (encoded.empty()) { return VerifyOutcome::Malformed; }

    std::string normalized = normalize_password(password);
    // argon2id_verify takes a NUL-terminated encoded string.
    const std::string encoded_z{encoded};

    const int rc =
        argon2id_verify(encoded_z.c_str(), normalized.data(), normalized.size());

    OPENSSL_cleanse(normalized.data(), normalized.size());

    if (rc == ARGON2_OK) { return VerifyOutcome::Match; }
    // A wrong password and an unparseable stored hash are distinguished for the
    // SERVER's benefit — a Malformed result means a corrupt row worth alerting
    // on. Callers must map both to the same client-visible failure.
    if (rc == ARGON2_VERIFY_MISMATCH) { return VerifyOutcome::Mismatch; }
    return VerifyOutcome::Malformed;
}

void PasswordHasher::consume_dummy_time() const noexcept {
    // Deliberately ignores the result. The point is the elapsed time and the
    // memory traffic, which must match a real verify with the same parameters.
    const int rc = argon2id_verify(dummy_encoded_.c_str(), kDummyPassword.data(),
                                   kDummyPassword.size());
    // Defeats any optimiser that might notice the result is unused. Marked
    // volatile so the call itself cannot be elided.
    static volatile int sink = 0;
    sink = rc;
    (void)sink;
}

bool PasswordHasher::needs_rehash(std::string_view encoded) const noexcept {
    const std::optional<Argon2Params> stored = parse_encoded_params(encoded);
    // Unparseable counts as needing a rehash: whatever produced it is not what
    // policy now requires.
    if (!stored.has_value()) { return true; }

    return stored->memory_kib < params_.memory_kib ||
           stored->iterations < params_.iterations ||
           stored->parallelism < params_.parallelism;
}

}  // namespace anvil::auth
