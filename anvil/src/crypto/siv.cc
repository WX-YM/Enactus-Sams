#include "anvil/crypto/siv.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <limits>
#include <memory>

#include "anvil/crypto/errors.h"

namespace anvil::crypto {
namespace {

struct CipherDeleter final {
    void operator()(EVP_CIPHER* cipher) const noexcept { EVP_CIPHER_free(cipher); }
};
using Cipher = std::unique_ptr<EVP_CIPHER, CipherDeleter>;

struct CipherCtxDeleter final {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept { EVP_CIPHER_CTX_free(ctx); }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

// OpenSSL's SIV provider decides what an update call means from its pointers:
// a null input is "finalise", a null output is "associated data". An empty
// std::span may carry a null data(), which would silently turn an empty AAD
// component into a premature finalise and an empty plaintext into an AAD
// component. Every pointer handed to an update is therefore non-null, whatever
// the length.
constexpr std::uint8_t kNoInput = 0;

[[nodiscard]] const std::uint8_t* input_pointer(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.empty() ? &kNoInput : bytes.data();
}

// EVP lengths are int. A longer input would be truncated by the cast, and the
// tag would then authenticate a prefix of what the caller passed.
[[nodiscard]] bool fits_in_int(std::size_t size) noexcept {
    return size <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

// Fetched per call rather than cached in a static: a static EVP_CIPHER would be
// freed by a static destructor whose order against OpenSSL's own atexit cleanup
// is not something this library controls.
[[nodiscard]] CipherCtx start(std::span<const std::uint8_t> key, bool encrypt) {
    const Cipher cipher{EVP_CIPHER_fetch(nullptr, "AES-256-SIV", nullptr)};
    if (!cipher) { throw CryptoError{"AES-256-SIV is not available"}; }

    CipherCtx ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) { throw CryptoError{"EVP_CIPHER_CTX_new failed"}; }

    const int ok = encrypt
        ? EVP_EncryptInit_ex2(ctx.get(), cipher.get(), key.data(), nullptr, nullptr)
        : EVP_DecryptInit_ex2(ctx.get(), cipher.get(), key.data(), nullptr, nullptr);
    if (ok != 1) { throw CryptoError{"AES-256-SIV init failed"}; }
    return ctx;
}

}  // namespace

std::vector<std::uint8_t> siv_seal(std::span<const std::uint8_t> key,
                                   std::span<const std::uint8_t> plaintext,
                                   std::span<const std::uint8_t> aad) {
    if (key.size() != kSivKeyBytes) { throw CryptoError{"siv_seal: key must be 64 bytes"}; }
    if (!fits_in_int(plaintext.size()) || !fits_in_int(aad.size())) {
        throw CryptoError{"siv_seal: input too large"};
    }

    const CipherCtx ctx = start(key, true);

    int len = 0;
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &len, input_pointer(aad),
                          static_cast<int>(aad.size())) != 1) {
        throw CryptoError{"AES-256-SIV aad failed"};
    }

    std::vector<std::uint8_t> sealed(kSivTagBytes + plaintext.size());
    // One past the tag is never null, even for an empty plaintext, so the
    // provider reads this as the plaintext call rather than another AAD one.
    std::uint8_t* const ciphertext = sealed.data() + kSivTagBytes;
    if (EVP_EncryptUpdate(ctx.get(), ciphertext, &len, input_pointer(plaintext),
                          static_cast<int>(plaintext.size())) != 1) {
        throw CryptoError{"AES-256-SIV encrypt failed"};
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext, &final_len) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kSivTagBytes),
                            sealed.data()) != 1) {
        throw CryptoError{"AES-256-SIV finalise failed"};
    }
    return sealed;
}

std::optional<std::vector<std::uint8_t>> siv_open(std::span<const std::uint8_t> key,
                                                  std::span<const std::uint8_t> sealed,
                                                  std::span<const std::uint8_t> aad) {
    if (key.size() != kSivKeyBytes) { throw CryptoError{"siv_open: key must be 64 bytes"}; }

    // Malformed input from a URL is an outcome, not an exception: a mangled
    // grant must not 500 the media origin.
    if (sealed.size() < kSivTagBytes) { return std::nullopt; }
    if (!fits_in_int(sealed.size()) || !fits_in_int(aad.size())) { return std::nullopt; }

    const std::span<const std::uint8_t> tag = sealed.first(kSivTagBytes);
    const std::span<const std::uint8_t> ciphertext = sealed.subspan(kSivTagBytes);

    const CipherCtx ctx = start(key, false);

    // The provider copies the tag; the ctrl signature is merely not const.
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kSivTagBytes),
                            const_cast<std::uint8_t*>(tag.data())) != 1) {
        return std::nullopt;
    }

    int len = 0;
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &len, input_pointer(aad),
                          static_cast<int>(aad.size())) != 1) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> plaintext(ciphertext.size());
    // An empty vector's data() may be null, which the provider would read as an
    // AAD call; any non-null pointer will do because nothing is written.
    std::uint8_t sink = 0;
    std::uint8_t* const out = plaintext.empty() ? &sink : plaintext.data();

    // The synthetic IV is recomputed and compared, in constant time, inside the
    // decrypt update; Final reports the same verdict. Either failing means the
    // plaintext buffer holds unauthenticated bytes, which are wiped, not returned.
    int final_len = 0;
    if (EVP_DecryptUpdate(ctx.get(), out, &len, input_pointer(ciphertext),
                          static_cast<int>(ciphertext.size())) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(), out, &final_len) != 1) {
        if (!plaintext.empty()) { OPENSSL_cleanse(plaintext.data(), plaintext.size()); }
        return std::nullopt;
    }
    return plaintext;
}

}  // namespace anvil::crypto
