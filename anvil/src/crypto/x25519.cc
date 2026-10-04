#include "anvil/crypto/x25519.h"

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

struct PkeyCtxDeleter final {
    void operator()(EVP_PKEY_CTX* ctx) const noexcept { EVP_PKEY_CTX_free(ctx); }
};
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;

using Encoding = std::array<std::uint8_t, kX25519PublicKeyBytes>;

constexpr std::uint8_t kHighBit = 0x80U;

// The u-coordinates for which X25519 yields the all-zero output for every
// private key, little-endian as encoded, bit 255 clear. Bit 255 is ignored by
// X25519 itself (RFC 7748 §5 masks it), so a key is compared with it masked and
// each row also refuses its high-bit spelling.
//
// Source: libsodium's has_small_order blocklist
// (src/libsodium/crypto_scalarmult/curve25519/ref10/x25519_ref10.c), after
// Bernstein's list at https://cr.yp.to/ecdh.html#validate. Independently
// re-derived for this table as the Montgomery images of the Edwards torsion
// points, and each row checked to make OpenSSL's X25519 refuse.
//
// p + u for the two order-8 values is not here because it exceeds 2^255: with
// bit 255 masked off it is u - 19, an unrelated coordinate.
constexpr std::array<Encoding, 7> kLowOrderU{{
    // u = 0.
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // u = 1.
    {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // The two u-coordinates of the points of order 8.
    {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
     0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00},
    {0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1, 0x55, 0x9c, 0x83, 0xef, 0x5b,
     0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c, 0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0x57},
    // u = p - 1.
    {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    // u = p, a non-canonical 0.
    {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    // u = p + 1, a non-canonical 1.
    {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
}};

// u < p = 2^255 - 19, for a value whose bit 255 is already known to be clear.
// Only the 19 encodings ff..ff7f with a low byte of 0xed or more fail it.
[[nodiscard]] bool below_field_prime(const Encoding& u) noexcept {
    if (u[31] != 0x7fU) { return true; }
    if (!std::all_of(u.begin() + 1, u.begin() + 31, [](std::uint8_t b) { return b == 0xffU; })) {
        return true;
    }
    return u[0] < 0xedU;
}

[[nodiscard]] Pkey private_key_from(const X25519PrivateKey& private_key) {
    // OpenSSL copies the key into its own object and wipes that copy on free,
    // so no unwiped copy outlives the call on our side.
    Pkey key{EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(),
                                          private_key.size())};
    if (!key) { throw CryptoError{"X25519 private key import failed"}; }
    return key;
}

}  // namespace

bool x25519_public_key_is_valid(
    std::span<const std::uint8_t, kX25519PublicKeyBytes> public_key) noexcept {
    Encoding u{};
    std::copy(public_key.begin(), public_key.end(), u.begin());
    const bool high_bit_set = (u[31] & kHighBit) != 0;
    u[31] = static_cast<std::uint8_t>(u[31] & ~kHighBit);

    // A public key is not secret, so an early-exit comparison is fine here.
    if (std::ranges::any_of(kLowOrderU, [&u](const Encoding& bad) { return bad == u; })) {
        return false;
    }
    return !high_bit_set && below_field_prime(u);
}

X25519Keypair x25519_generate_keypair() {
    X25519PrivateKey private_key = random_secret<kX25519PrivateKeyBytes>();
    const Pkey key = private_key_from(private_key);

    X25519PublicKey public_key{};
    std::size_t len = public_key.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &len) != 1 ||
        len != public_key.size()) {
        throw CryptoError{"X25519 public key export failed"};
    }
    return X25519Keypair{.private_key = std::move(private_key), .public_key = public_key};
}

std::optional<X25519SharedSecret> x25519_derive(
    const X25519PrivateKey& private_key,
    std::span<const std::uint8_t, kX25519PublicKeyBytes> peer_public) {
    const Pkey key = private_key_from(private_key);
    const Pkey peer{EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_public.data(),
                                                peer_public.size())};
    if (!peer) { throw CryptoError{"X25519 public key import failed"}; }

    const PkeyCtx ctx{EVP_PKEY_CTX_new(key.get(), nullptr)};
    if (!ctx) { throw CryptoError{"EVP_PKEY_CTX_new failed"}; }
    if (EVP_PKEY_derive_init(ctx.get()) != 1 || EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) != 1) {
        throw CryptoError{"X25519 derive init failed"};
    }

    X25519SharedSecret shared;
    std::size_t len = shared.size();
    // OpenSSL fails the derive exactly when the output is all zero (the
    // low-order case RFC 7748 §6.1 says to abort on). Whatever it wrote before
    // failing is wiped by `shared` going out of scope.
    if (EVP_PKEY_derive(ctx.get(), shared.data(), &len) != 1 || len != shared.size()) {
        return std::nullopt;
    }
    return shared;
}

}  // namespace anvil::crypto
