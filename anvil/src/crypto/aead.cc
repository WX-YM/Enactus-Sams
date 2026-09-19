#include "anvil/crypto/aead.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <memory>

#include "anvil/crypto/errors.h"
#include "anvil/crypto/random.h"

namespace anvil::crypto {
namespace {

constexpr std::size_t kKeyBytes = 32;

struct CipherCtxDeleter final {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept { EVP_CIPHER_CTX_free(ctx); }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

}  // namespace

std::vector<std::uint8_t> seal(std::span<const std::uint8_t> key, std::string_view plaintext,
                               std::span<const std::uint8_t> aad) {
    if (key.size() != kKeyBytes) { throw CryptoError{"seal: key must be 32 bytes"}; }

    const CipherCtx ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) { throw CryptoError{"EVP_CIPHER_CTX_new failed"}; }

    const std::array<std::uint8_t, kAeadIvBytes> iv = random_array<kAeadIvBytes>();

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(kAeadIvBytes), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), iv.data()) != 1) {
        throw CryptoError{"AES-256-GCM init failed"};
    }

    int len = 0;
    if (!aad.empty() &&
        EVP_EncryptUpdate(ctx.get(), nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1) {
        throw CryptoError{"AES-256-GCM aad failed"};
    }

    std::vector<std::uint8_t> envelope(kAeadOverhead + plaintext.size());
    envelope[0] = kAeadVersion;
    std::copy(iv.begin(), iv.end(), envelope.begin() + 1);

    std::uint8_t* ciphertext = envelope.data() + kAeadOverhead;
    int written = 0;
    if (EVP_EncryptUpdate(ctx.get(), ciphertext, &written,
                          reinterpret_cast<const std::uint8_t*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        throw CryptoError{"AES-256-GCM encrypt failed"};
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext + written, &final_len) != 1) {
        throw CryptoError{"AES-256-GCM finalise failed"};
    }

    // The tag is only available after finalisation, which is why it sits in a
    // fixed slot rather than being appended.
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kAeadTagBytes),
                            envelope.data() + 1 + kAeadIvBytes) != 1) {
        throw CryptoError{"AES-256-GCM tag extraction failed"};
    }

    envelope.resize(kAeadOverhead + static_cast<std::size_t>(written + final_len));
    return envelope;
}

std::optional<std::string> open(std::span<const std::uint8_t> key,
                                std::span<const std::uint8_t> envelope,
                                std::span<const std::uint8_t> aad) {
    if (key.size() != kKeyBytes) { throw CryptoError{"open: key must be 32 bytes"}; }

    // A truncated envelope is a malformed input, not a crypto failure — return
    // nullopt rather than throwing, so a corrupt row cannot 500 a listing page.
    if (envelope.size() < kAeadOverhead) { return std::nullopt; }
    if (envelope[0] != kAeadVersion) { return std::nullopt; }

    const CipherCtx ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) { throw CryptoError{"EVP_CIPHER_CTX_new failed"}; }

    const std::uint8_t* iv = envelope.data() + 1;
    const std::uint8_t* tag = envelope.data() + 1 + kAeadIvBytes;
    const std::uint8_t* ciphertext = envelope.data() + kAeadOverhead;
    const std::size_t ciphertext_len = envelope.size() - kAeadOverhead;

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(kAeadIvBytes), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), iv) != 1) {
        throw CryptoError{"AES-256-GCM decrypt init failed"};
    }

    int len = 0;
    if (!aad.empty() &&
        EVP_DecryptUpdate(ctx.get(), nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1) {
        return std::nullopt;
    }

    std::string plaintext(ciphertext_len, '\0');
    int written = 0;
    if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<std::uint8_t*>(plaintext.data()), &written,
                          ciphertext, static_cast<int>(ciphertext_len)) != 1) {
        return std::nullopt;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kAeadTagBytes),
                            const_cast<std::uint8_t*>(tag)) != 1) {
        return std::nullopt;
    }

    // EVP_DecryptFinal_ex is where the tag is actually verified. A non-positive
    // return means the ciphertext, the aad or the key is wrong — the plaintext
    // produced so far must be discarded, never returned.
    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<std::uint8_t*>(plaintext.data()) + written,
                            &final_len) <= 0) {
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        return std::nullopt;
    }

    plaintext.resize(static_cast<std::size_t>(written + final_len));
    return plaintext;
}

}  // namespace anvil::crypto
