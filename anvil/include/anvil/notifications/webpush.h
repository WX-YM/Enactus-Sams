#pragma once

// Web Push: the VAPID authorization header (RFC 8292) and the encrypted payload
// (RFC 8291, aes128gcm).
//
// The HTTP POST is not here. Everything that has to be cryptographically right
// is, and all of it is a pure function of its inputs — which is what makes it
// testable against the published vectors instead of against a push service.
//
// --- the server never learns what it sent -----------------------------------
//
// The payload is encrypted to a key pair the BROWSER generated and whose private
// half never leaves the device. The push service relays ciphertext it cannot
// read, and neither can we after the fact: there is no stored key that decrypts a
// delivery. That is the property that makes it acceptable to put a notification's
// text through somebody else's infrastructure at all.
//
//     ecdh      = ECDH(our ephemeral private, the subscription's p256dh)
//     IKM       = HKDF(salt = auth secret, ikm = ecdh,
//                      info = "WebPush: info\0" || ua_public || as_public, 32)
//     salt      = 16 CSPRNG bytes, FRESH PER MESSAGE
//     CEK       = HKDF(salt, IKM, "Content-Encoding: aes128gcm\0", 16)
//     nonce     = HKDF(salt, IKM, "Content-Encoding: nonce\0", 12)
//     body      = salt || rs(4) || idlen(1) || as_public(65) || AES-128-GCM(...)
//
// The ephemeral key pair and the salt are both per message, and both have to be:
// AES-GCM under a repeated key and nonce is not merely weakened, it leaks the
// XOR of the two plaintexts and the authentication key with it. Every input that
// feeds the nonce is therefore generated here rather than passed in, so there is
// no signature by which a caller can supply a reused one.
//
// --- VAPID identifies the SENDER, it does not authorize the delivery ---------
//
// The JWT tells the push service which application server is asking. It is signed
// over the service's own origin as `aud`, which is what stops one push service
// replaying our token to another. `exp` is short for the same reason: a captured
// token is a token somebody else can send our notifications with, until it
// expires.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"

namespace anvil::notifications {

// P-256, so every one of these is fixed and none of them is a parameter.
inline constexpr std::size_t kP256PrivateBytes = 32;
inline constexpr std::size_t kP256PublicBytes = 65;   // 0x04 || X(32) || Y(32)
inline constexpr std::size_t kEcdsaSignatureBytes = 64;   // r(32) || s(32)

// RFC 8291. The subscription's auth secret is 16 bytes and its public key is an
// uncompressed P-256 point.
inline constexpr std::size_t kAuthSecretBytes = 16;
inline constexpr std::size_t kSaltBytes = 16;
inline constexpr std::size_t kContentKeyBytes = 16;   // AES-128
inline constexpr std::size_t kNonceBytes = 12;
inline constexpr std::size_t kGcmTagBytes = 16;

// The aes128gcm header: salt(16) || record size(4) || key id length(1) || key id.
// The key id is our ephemeral public key, so the length is always 65 here.
inline constexpr std::size_t kHeaderBytes = kSaltBytes + 4 + 1 + kP256PublicBytes;

// What a push service is required to accept. The payload plus its padding
// delimiter and tag must fit inside one record, so this is also the ceiling on
// what can be sent — a longer notification is truncated by the caller or not sent
// at all, never split across records that a browser is not obliged to reassemble.
inline constexpr std::uint32_t kRecordSize = 4096;
inline constexpr std::size_t kMaxPlaintextBytes = kRecordSize - kGcmTagBytes - 1;

// How long a VAPID token is good for.
//
// Twelve hours is the ceiling RFC 8292 allows, and shorter is better for exactly
// one reason: a captured token lets somebody else send notifications in our name
// until it expires. Twelve hours is used because the tokens are minted per
// delivery batch rather than cached, so a short lifetime costs nothing — if that
// ever changes, this is the number that has to come down with it.
inline constexpr std::chrono::seconds kVapidLifetime{12 * 3600};

// A subscription, exactly as a browser produced it.
struct PushSubscription final {
    // The push service's endpoint URL. Its ORIGIN is what the VAPID token is
    // signed over; anything else and the service refuses it, which is the check
    // that stops one service replaying our token to another.
    std::string_view                            endpoint;
    std::array<std::uint8_t, kP256PublicBytes>  p256dh;
    std::array<std::uint8_t, kAuthSecretBytes>  auth;
};

// The application server's identity. The private half signs; the public half
// goes in the `k=` parameter and is what the browser pinned when it subscribed.
struct VapidKey final {
    std::array<std::uint8_t, kP256PrivateBytes> private_key;
    std::array<std::uint8_t, kP256PublicBytes>  public_key;
};

// `mailto:` or `https:`. The push service uses it to reach an operator when our
// traffic is the problem, so it is required rather than optional — an application
// server nobody can contact is one a service blocks rather than warns.
struct VapidIdentity final {
    std::string_view subject;
};

// The origin a VAPID token must be signed over: scheme + "://" + host[:port].
//
// Exposed because getting it wrong is the single most common VAPID failure and it
// fails as an opaque 401 from the push service. Empty when `endpoint` is not a
// usable absolute https URL.
[[nodiscard]] std::string push_origin(std::string_view endpoint);

// A fresh application-server key pair. Generated once and STORED — rotating it
// silently invalidates every existing subscription, because the browser pinned
// the public half at subscribe time and the push service checks it.
[[nodiscard]] Result<VapidKey> generate_vapid_key();

// The public key as the `applicationServerKey` a browser expects: unpadded
// base64url of the uncompressed point.
[[nodiscard]] std::string vapid_public_key_for_browser(const VapidKey& key);

// The complete `Authorization` header value: `vapid t=<jwt>, k=<public key>`.
//
// `now` is a parameter rather than a clock read so a test can assert the `exp`
// claim rather than infer it.
[[nodiscard]] Result<std::string> vapid_authorization(const VapidKey& key,
                                                      const VapidIdentity& identity,
                                                      std::string_view endpoint,
                                                      db::TimeMs now);

// The ES256 JWT on its own, for a test and for an operator debugging a 401.
[[nodiscard]] Result<std::string> vapid_token(const VapidKey& key,
                                              const VapidIdentity& identity,
                                              std::string_view audience, db::TimeMs now);

// The encrypted body, ready to POST with `Content-Encoding: aes128gcm`.
//
// A fresh ephemeral key pair and a fresh salt are generated INSIDE, never passed
// in. AES-GCM under a repeated key and nonce leaks the XOR of the two plaintexts
// and the authentication key with it, so there is deliberately no signature by
// which a caller can supply either.
//
// ValidationFailed when the plaintext is over kMaxPlaintextBytes or the
// subscription's public key is not a point on P-256 — a key that is not on the
// curve is either corruption or an invalid-curve attack, and multiplying by it
// leaks our ephemeral scalar.
[[nodiscard]] Result<std::vector<std::uint8_t>> encrypt_payload(
    const PushSubscription& subscription, std::string_view plaintext);

// Decrypt one, given the subscription's PRIVATE key.
//
// The browser's side, shipped for the same reason the webhook receiver's
// verification is: it is what makes the encryption testable end to end rather
// than self-consistent, and a test that only ever checks our own output against
// our own expectations proves nothing about the wire format.
[[nodiscard]] Result<std::string> decrypt_payload(
    std::span<const std::uint8_t> ua_private,
    std::span<const std::uint8_t> auth_secret,
    std::span<const std::uint8_t> body);

// HKDF-SHA256 (RFC 5869), extract and expand.
//
// Exposed because Web Push uses it three times with three different infos and a
// reader has to be able to check each against the RFC — and because a KDF folded
// invisibly into a larger function is one nobody can test against a published
// vector.
[[nodiscard]] std::vector<std::uint8_t> hkdf_sha256(std::span<const std::uint8_t> salt,
                                                    std::span<const std::uint8_t> ikm,
                                                    std::span<const std::uint8_t> info,
                                                    std::size_t length);

}  // namespace anvil::notifications
