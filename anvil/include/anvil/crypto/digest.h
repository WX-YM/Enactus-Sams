#pragma once

// SHA-256 and HMAC-SHA256.
//
// Token storage uses sha256_with_pepper, not a slow KDF. Tokens carry >= 128
// bits of CSPRNG entropy, so there is nothing to brute-force and a slow hash on
// the lookup path is a denial-of-service vector rather than a defence
// (CLAUDE.md §5). Passwords are the opposite case and use Argon2id.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "anvil/crypto/secret.h"

// Forward-declared so this header does not pull <openssl/evp.h> into every
// translation unit that needs a hash.
struct evp_md_ctx_st;

namespace anvil::crypto {

[[nodiscard]] Digest256 sha256(std::span<const std::uint8_t> data);
[[nodiscard]] Digest256 sha256(std::string_view data);

// SHA-256(token ‖ pepper). The pepper lives in configuration, not in the
// database, so a database leak alone does not permit offline token lookup.
[[nodiscard]] Digest256 sha256_with_pepper(std::string_view token,
                                           std::span<const std::uint8_t> pepper);

[[nodiscard]] Digest256 hmac_sha256(std::span<const std::uint8_t> key,
                                    std::span<const std::uint8_t> message);
[[nodiscard]] Digest256 hmac_sha256(std::span<const std::uint8_t> key,
                                    std::string_view message);

// Blind index for PII: HMAC, never a plain hash. A plain SHA-256 of a
// structured 14-digit National ID is exhaustible in seconds, so the keyed
// construction is what makes the index safe to store.
[[nodiscard]] Digest256 blind_index(std::string_view normalised_value,
                                    std::span<const std::uint8_t> index_key);

// Incremental SHA-256, for data that is never fully resident.
//
// The upload path hashes a 4 MB image in one pass over a 64 KB streaming buffer
// (docs/07-filesystem.md §4 step 6). The alternative — hash the finished
// temporary file — is a second full read of every byte just written, and the
// alternative to THAT is holding the whole body in memory, which is the exact
// copy the architecture exists to avoid.
//
// One resource, RAII, rule of zero (CLAUDE.md §3.3). Move-only, because two
// handles to one OpenSSL context is a double free.
class Sha256Stream final {
public:
    Sha256Stream();
    ~Sha256Stream();

    Sha256Stream(Sha256Stream&&) noexcept;
    Sha256Stream& operator=(Sha256Stream&&) noexcept;
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;

    void update(std::span<const std::uint8_t> data);

    // Consumes the state: calling it twice throws rather than returning a
    // second, meaningless digest.
    [[nodiscard]] Digest256 finish();

private:
    struct CtxDeleter final {
        void operator()(evp_md_ctx_st* ctx) const noexcept;
    };

    std::unique_ptr<evp_md_ctx_st, CtxDeleter> ctx_;
};

}  // namespace anvil::crypto
