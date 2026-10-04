#include "anvil/auth/password.h"

#include <openssl/crypto.h>

#include <array>
#include <stdexcept>

#include "anvil/crypto/errors.h"
#include "anvil/crypto/random.h"
#include "anvil/i18n/normalize.h"

namespace anvil::auth {
namespace {

constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;

// A fixed password for the dummy verify. Its value is irrelevant — only the
// work performed matters — but it must not be a plausible real password, so
// that a stored hash of it could never match a user's.
constexpr std::string_view kDummyPassword = "\x01 anvil timing equaliser \x01";

[[nodiscard]] std::span<const std::uint8_t> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
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
    return crypto::argon2id_parse_params(encoded);
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

    std::string encoded;
    try {
        encoded = crypto::argon2id_hash_encoded(params_, as_bytes(normalized), salt, kHashBytes);
    } catch (...) {
        OPENSSL_cleanse(normalized.data(), normalized.size());
        throw;
    }

    // The plaintext is gone as soon as it is no longer needed. A plain memset
    // here would be removed by the optimiser as a dead store.
    OPENSSL_cleanse(normalized.data(), normalized.size());
    return encoded;
}

VerifyOutcome PasswordHasher::verify(std::string_view encoded,
                                     std::string_view password) const {
    // An oversized candidate is rejected without hashing. Argon2's cost is
    // driven by the parameters rather than the input length, but there is no
    // reason to copy and normalise a megabyte to reject it.
    if (password.size() > kMaxPasswordBytes) { return VerifyOutcome::Mismatch; }
    if (encoded.empty()) { return VerifyOutcome::Malformed; }

    std::string normalized = normalize_password(password);
    const VerifyOutcome outcome = crypto::argon2id_verify_encoded(encoded, as_bytes(normalized));

    OPENSSL_cleanse(normalized.data(), normalized.size());
    return outcome;
}

void PasswordHasher::consume_dummy_time() const noexcept {
    // Deliberately ignores the result. The point is the elapsed time and the
    // memory traffic, which must match a real verify with the same parameters.
    const VerifyOutcome outcome =
        crypto::argon2id_verify_encoded(dummy_encoded_, as_bytes(kDummyPassword));
    // Defeats any optimiser that might notice the result is unused. Marked
    // volatile so the call itself cannot be elided.
    static volatile VerifyOutcome sink = VerifyOutcome::Mismatch;
    sink = outcome;
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
