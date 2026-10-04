#include "anvil/crypto/digest.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <memory>

#include "anvil/crypto/errors.h"

namespace anvil::crypto {
namespace {

std::span<const std::uint8_t> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// RAII for the OpenSSL context: every early return and every exception must
// still free it (CLAUDE.md §3.3).
struct MdCtxDeleter final {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};
using MdCtx = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

}  // namespace

Digest256 sha256(std::span<const std::uint8_t> data) {
    Digest256 out{};
    unsigned int len = 0;

    const MdCtx ctx{EVP_MD_CTX_new()};
    if (!ctx) { throw CryptoError{"EVP_MD_CTX_new failed"}; }

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx.get(), out.data(), &len) != 1) {
        throw CryptoError{"SHA-256 failed"};
    }
    if (len != out.size()) { throw CryptoError{"SHA-256 produced an unexpected length"}; }
    return out;
}

Digest256 sha256(std::string_view data) { return sha256(as_bytes(data)); }

Digest256 sha256_with_pepper(std::string_view token, std::span<const std::uint8_t> pepper) {
    Digest256 out{};
    unsigned int len = 0;

    const MdCtx ctx{EVP_MD_CTX_new()};
    if (!ctx) { throw CryptoError{"EVP_MD_CTX_new failed"}; }

    // Streamed in two updates rather than concatenated into a temporary, so the
    // pepper is never copied into a buffer that would have to be wiped.
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), token.data(), token.size()) != 1 ||
        EVP_DigestUpdate(ctx.get(), pepper.data(), pepper.size()) != 1 ||
        EVP_DigestFinal_ex(ctx.get(), out.data(), &len) != 1) {
        throw CryptoError{"peppered SHA-256 failed"};
    }
    return out;
}

Digest256 hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> message) {
    Digest256 out{};
    unsigned int len = 0;

    const unsigned char* result =
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), message.data(),
             message.size(), out.data(), &len);

    if (result == nullptr || len != out.size()) { throw CryptoError{"HMAC-SHA256 failed"}; }
    return out;
}

Digest256 hmac_sha256(std::span<const std::uint8_t> key, std::string_view message) {
    return hmac_sha256(key, as_bytes(message));
}

Digest256 blind_index(std::string_view normalised_value, std::span<const std::uint8_t> index_key) {
    return hmac_sha256(index_key, normalised_value);
}

void Sha256Stream::CtxDeleter::operator()(evp_md_ctx_st* ctx) const noexcept {
    EVP_MD_CTX_free(ctx);
}

Sha256Stream::Sha256Stream() : ctx_{EVP_MD_CTX_new()} {
    if (!ctx_) { throw CryptoError{"EVP_MD_CTX_new failed"}; }
    if (EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr) != 1) {
        throw CryptoError{"streaming SHA-256 init failed"};
    }
}

Sha256Stream::~Sha256Stream() = default;
Sha256Stream::Sha256Stream(Sha256Stream&&) noexcept = default;
Sha256Stream& Sha256Stream::operator=(Sha256Stream&&) noexcept = default;

void Sha256Stream::update(std::span<const std::uint8_t> data) {
    if (!ctx_) { throw CryptoError{"streaming SHA-256 updated after finish"}; }
    if (data.empty()) { return; }
    if (EVP_DigestUpdate(ctx_.get(), data.data(), data.size()) != 1) {
        throw CryptoError{"streaming SHA-256 update failed"};
    }
}

Digest256 Sha256Stream::finish() {
    if (!ctx_) { throw CryptoError{"streaming SHA-256 finished twice"}; }
    Digest256 out{};
    unsigned int len = 0;
    const bool succeeded = EVP_DigestFinal_ex(ctx_.get(), out.data(), &len) == 1;
    // Released either way: a failed finalisation leaves the context unusable,
    // and keeping it would let a caller retry into an undefined state.
    ctx_.reset();
    if (!succeeded || len != out.size()) { throw CryptoError{"streaming SHA-256 final failed"}; }
    return out;
}

}  // namespace anvil::crypto
