#include "anvil/crypto/ed25519.h"

#include <openssl/bn.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "anvil/crypto/errors.h"
#include "anvil/crypto/random.h"

namespace anvil::crypto {
namespace {

struct PkeyDeleter final {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

struct MdCtxDeleter final {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};
using MdCtx = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

struct BnCtxDeleter final {
    void operator()(BN_CTX* ctx) const noexcept { BN_CTX_free(ctx); }
};
using BnCtx = std::unique_ptr<BN_CTX, BnCtxDeleter>;

// BN_CTX_start / BN_CTX_end bracket, so every early return releases the frame.
class BnFrame final {
public:
    explicit BnFrame(BN_CTX* ctx) noexcept : ctx_{ctx} { BN_CTX_start(ctx_); }
    ~BnFrame() { BN_CTX_end(ctx_); }
    BnFrame(const BnFrame&) = delete;
    BnFrame& operator=(const BnFrame&) = delete;
    BnFrame(BnFrame&&) = delete;
    BnFrame& operator=(BnFrame&&) = delete;

private:
    BN_CTX* ctx_;
};

using Encoding = std::array<std::uint8_t, 32>;

// p = 2^255 - 19 and d = -121665/121666 mod p, big-endian for BN_bin2bn.
constexpr Encoding kFieldPrime{
    0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xed};
constexpr Encoding kCurveD{
    0x52, 0x03, 0x6c, 0xee, 0x2b, 0x6f, 0xfe, 0x73, 0x8c, 0xc7, 0x40, 0x79, 0x77, 0x79, 0xe8, 0x98,
    0x00, 0x70, 0x0a, 0x4d, 0x41, 0x41, 0xd8, 0xab, 0x75, 0xeb, 0x4d, 0xca, 0x13, 0x59, 0x78, 0xa3};

// The y-coordinates of the eight small-order points, little-endian as encoded,
// with the sign bit (bit 255) clear. Compared against a key with its sign bit
// masked, so each row refuses both signs of x.
//
// Source: libsodium's ge25519_has_small_order blocklist
// (src/libsodium/crypto_core/ed25519/ref10/ed25519_ref10.c), and independently
// recomputed for this table as the y-coordinates of [L]P for random points P.
// The last two rows are the non-canonical spellings of 0 and 1; the canonical
// check refuses them first, and they stay here so this table alone matches its
// source.
constexpr std::array<Encoding, 7> kSmallOrderY{{
    // y = 0: the two points of order 4.
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // y = 1: the identity.
    {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // The two y-coordinates shared by the four points of order 8.
    {0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0,
     0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05},
    {0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
     0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a},
    // y = p - 1: the point of order 2.
    {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    // y = p, a non-canonical 0.
    {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    // y = p + 1, a non-canonical 1.
    {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
}};

constexpr std::uint8_t kSignBit = 0x80U;

// RFC 8032 §5.1.3 decoding, as a yes/no: y < p, and x^2 = (y^2 - 1)/(d y^2 + 1)
// has a root, and x = 0 is not spelled with the sign bit. Done with BIGNUM
// rather than OpenSSL's Ed25519 decoder because that decoder is not exported,
// and the one reachable through verification accepts the non-canonical forms
// this exists to refuse. The key is public, so variable-time arithmetic leaks
// nothing.
[[nodiscard]] bool decodes_strictly(const Encoding& y_le, bool x_is_negative) noexcept {
    const BnCtx ctx{BN_CTX_new()};
    if (!ctx) { return false; }
    const BnFrame frame{ctx.get()};

    BIGNUM* const p = BN_CTX_get(ctx.get());
    BIGNUM* const d = BN_CTX_get(ctx.get());
    BIGNUM* const y = BN_CTX_get(ctx.get());
    BIGNUM* const y2 = BN_CTX_get(ctx.get());
    BIGNUM* const u = BN_CTX_get(ctx.get());
    BIGNUM* const v = BN_CTX_get(ctx.get());
    BIGNUM* const x2 = BN_CTX_get(ctx.get());
    // BN_CTX_get fails sticky: once one returns null, every later one does.
    if (x2 == nullptr) { return false; }

    if (BN_bin2bn(kFieldPrime.data(), static_cast<int>(kFieldPrime.size()), p) == nullptr ||
        BN_bin2bn(kCurveD.data(), static_cast<int>(kCurveD.size()), d) == nullptr ||
        BN_lebin2bn(y_le.data(), static_cast<int>(y_le.size()), y) == nullptr) {
        return false;
    }
    if (BN_cmp(y, p) >= 0) { return false; }

    // u = y^2 - 1, v = d*y^2 + 1, x^2 = u / v. v is never zero: -1/d is not a
    // square mod p, so BN_mod_inverse failing means an allocation failed.
    if (BN_mod_sqr(y2, y, p, ctx.get()) != 1 || BN_copy(u, y2) == nullptr ||
        BN_sub_word(u, 1) != 1 || BN_nnmod(u, u, p, ctx.get()) != 1 ||
        BN_mod_mul(v, d, y2, p, ctx.get()) != 1 || BN_add_word(v, 1) != 1 ||
        BN_nnmod(v, v, p, ctx.get()) != 1 || BN_mod_inverse(v, v, p, ctx.get()) == nullptr ||
        BN_mod_mul(x2, u, v, p, ctx.get()) != 1) {
        return false;
    }

    if (BN_is_zero(x2) == 1) { return !x_is_negative; }
    // The Legendre symbol: 1 for a non-zero square, -1 for a non-square, -2 on
    // error. Only a square has an x to recover.
    return BN_kronecker(x2, p, ctx.get()) == 1;
}

[[nodiscard]] Pkey private_key_from(const Ed25519Seed& seed) {
    // OpenSSL copies the seed into its own key object and wipes that copy when
    // the key is freed, so no unwiped copy outlives this call on our side.
    Pkey key{EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size())};
    if (!key) { throw CryptoError{"Ed25519 private key import failed"}; }
    return key;
}

// EVP's one-shot sign and verify accept a null message pointer only by
// accident of the current implementation; an empty span may carry one.
constexpr std::uint8_t kNoMessage = 0;

[[nodiscard]] const std::uint8_t* message_pointer(std::span<const std::uint8_t> message) noexcept {
    return message.empty() ? &kNoMessage : message.data();
}

}  // namespace

bool ed25519_public_key_is_valid(
    std::span<const std::uint8_t, kEd25519PublicKeyBytes> public_key) noexcept {
    Encoding y{};
    std::copy(public_key.begin(), public_key.end(), y.begin());
    const bool x_is_negative = (y[31] & kSignBit) != 0;
    y[31] = static_cast<std::uint8_t>(y[31] & ~kSignBit);

    // A public key is not secret, so an early-exit comparison is fine here.
    if (std::ranges::any_of(kSmallOrderY, [&y](const Encoding& bad) { return bad == y; })) {
        return false;
    }
    return decodes_strictly(y, x_is_negative);
}

bool ed25519_verify(std::span<const std::uint8_t, kEd25519PublicKeyBytes> public_key,
                    std::span<const std::uint8_t> message,
                    std::span<const std::uint8_t, kEd25519SignatureBytes> signature) noexcept {
    // OpenSSL decodes the key leniently and does not refuse small order, so the
    // strict check runs first; without it the identity key "verifies" any
    // signature whose R is [S]B.
    if (!ed25519_public_key_is_valid(public_key)) { return false; }

    const Pkey key{EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key.data(),
                                               public_key.size())};
    if (!key) { return false; }

    const MdCtx ctx{EVP_MD_CTX_new()};
    if (!ctx) { return false; }

    // A null digest is how EVP selects pure Ed25519: the message is hashed by
    // the signature scheme itself, not pre-hashed.
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        return false;
    }
    return EVP_DigestVerify(ctx.get(), signature.data(), signature.size(),
                            message_pointer(message), message.size()) == 1;
}

Ed25519PublicKey ed25519_public_key_from_seed(const Ed25519Seed& seed) {
    const Pkey key = private_key_from(seed);
    Ed25519PublicKey public_key{};
    std::size_t len = public_key.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &len) != 1 ||
        len != public_key.size()) {
        throw CryptoError{"Ed25519 public key export failed"};
    }
    return public_key;
}

Ed25519Signature ed25519_sign(const Ed25519Seed& seed, std::span<const std::uint8_t> message) {
    const Pkey key = private_key_from(seed);
    const MdCtx ctx{EVP_MD_CTX_new()};
    if (!ctx) { throw CryptoError{"EVP_MD_CTX_new failed"}; }

    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        throw CryptoError{"Ed25519 sign init failed"};
    }
    Ed25519Signature signature{};
    std::size_t len = signature.size();
    if (EVP_DigestSign(ctx.get(), signature.data(), &len, message_pointer(message),
                       message.size()) != 1 ||
        len != signature.size()) {
        throw CryptoError{"Ed25519 sign failed"};
    }
    return signature;
}

Ed25519Keypair ed25519_generate_keypair() {
    Ed25519Seed seed = random_secret<kEd25519SeedBytes>();
    const Ed25519PublicKey public_key = ed25519_public_key_from_seed(seed);
    return Ed25519Keypair{.seed = std::move(seed), .public_key = public_key};
}

}  // namespace anvil::crypto
