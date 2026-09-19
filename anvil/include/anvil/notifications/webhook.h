#pragma once

// Signed webhooks: the envelope, the signature, and what a receiver has to do to
// verify one.
//
// The HTTP POST is not here. What is here is every part that has to be right for
// the receiver to be able to trust the request at all, and all of it is a pure
// function of its inputs — so it is testable without a network, and a receiver in
// another language can be written from this header.
//
// --- the signature covers a TIMESTAMP, not just the body --------------------
//
//     v1=HMAC-SHA256(secret, "<timestamp>.<body>")
//
// A signature over the body alone is replayable forever: an attacker who
// observes one valid delivery can resend it a year later and it still verifies.
// Binding the instant into the signed string means the receiver can reject
// anything outside a tolerance window, and the attacker cannot move the window
// without invalidating the signature.
//
// The receiver's side of that bargain is the half anvil cannot enforce, so it is
// stated where the sender is implemented rather than left implicit:
//
//   1. Reject if `timestamp` is outside the tolerance (kReplayTolerance).
//   2. Recompute over "<timestamp>.<body>" using the RAW body — not a reparsed,
//      reserialised copy of it. A JSON round trip reorders keys and changes
//      spacing, and the signature is over bytes.
//   3. Compare in CONSTANT TIME. A receiver using == leaks the expected
//      signature one byte at a time to anyone who can measure it.
//
// --- the secret is shown once ------------------------------------------------
//
// It is stored sealed and there is no read path that decrypts it back to an
// operator's screen. A signing secret that can be re-read is a signing secret
// that appears in a support ticket.
//
// --- where the URL is checked ------------------------------------------------
//
// A webhook URL is an operator-supplied address this process will make a request
// to, which is server-side request forgery by design. It is only safe because it
// is constrained: the check belongs at REGISTRATION, where refusing is free,
// rather than at delivery, where a URL that resolved to a private address
// yesterday has already been trusted.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/secret.h"
#include "anvil/db/codec.h"

namespace anvil::notifications {

// The header carrying the signature, and the one carrying the instant it was
// signed at. Named here rather than at the call site because a receiver in
// another language has to spell them identically.
inline constexpr std::string_view kSignatureHeader = "X-Anvil-Signature";
inline constexpr std::string_view kTimestampHeader = "X-Anvil-Timestamp";
inline constexpr std::string_view kDeliveryHeader = "X-Anvil-Delivery";

// The scheme prefix on the signature value, so a future construction can be
// introduced without a receiver having to guess which one it is looking at. A
// receiver that ignores it is a receiver that cannot be migrated.
inline constexpr std::string_view kSignatureVersion = "v1";

// How far out of step a delivery's timestamp may be before it is refused.
//
// Five minutes each way. Wide enough to absorb ordinary clock skew between two
// machines that are not synchronised to each other, narrow enough that a captured
// delivery is worthless within the hour. Tightening it further starts rejecting
// honest deliveries from hosts whose clocks drift, which presents as an
// intermittent integration failure nobody can reproduce.
inline constexpr std::chrono::seconds kReplayTolerance{300};

// A webhook signing secret: 256 bits of CSPRNG output.
//
// Not derived from anything and not a password. It is compared by HMAC rather
// than looked up, so there is nothing to brute-force offline and no reason for a
// slow KDF — the same reasoning `crypto/digest.h` gives for token storage.
inline constexpr std::size_t kWebhookSecretBytes = 32;
using WebhookSecret = std::array<std::uint8_t, kWebhookSecretBytes>;

// Hex, lowercase, 64 characters. The signature a receiver compares against.
inline constexpr std::size_t kSignatureHexChars = 64;

// The string that is actually signed: "<unix seconds>.<body>".
//
// Exposed because a receiver has to build the identical string, and a description
// in prose is a description two implementations can disagree about.
[[nodiscard]] std::string signing_string(std::int64_t timestamp_seconds, std::string_view body);

// `v1=<64 lowercase hex>`.
[[nodiscard]] std::string sign(std::span<const std::uint8_t> secret,
                               std::int64_t timestamp_seconds, std::string_view body);

// Verify a signature the way a RECEIVER must.
//
// Shipped alongside the sender on purpose: a receiver is the half that gets this
// wrong, and it gets it wrong in ways that do not show up in testing — a `==`
// comparison passes every test and leaks the expected signature to anybody who
// can time it, and a tolerance nobody applied passes every test and accepts a
// year-old replay.
//
// `now` is a parameter rather than a call to the clock so that a test can assert
// the window's edges rather than sleep through them.
[[nodiscard]] bool verify(std::span<const std::uint8_t> secret, std::string_view signature,
                          std::int64_t timestamp_seconds, std::string_view body,
                          db::TimeMs now) noexcept;

// A fresh signing secret. CSPRNG, never a hash of anything the operator chose.
[[nodiscard]] WebhookSecret generate_secret();

// --- where a webhook may point ----------------------------------------------

enum class UrlVerdict : std::uint8_t {
    Ok = 0,
    // Not `https://`. A signed delivery over plaintext is a signed delivery
    // anybody on the path can read; the signature proves who sent it, not that
    // nobody else saw it.
    NotHttps = 1,
    // Malformed, over-long, or carrying credentials, a fragment or a non-ASCII
    // host. Each of those is a parser disagreement waiting to happen — the
    // userinfo form especially, which several HTTP clients read differently from
    // the host that appears in a log.
    Malformed = 2,
    // A literal IP address in a range that is not routable on the public
    // internet: loopback, link-local, the cloud metadata address, RFC 1918. This
    // is the SSRF refusal, and it is the reason this function exists.
    NotPublic = 3,
    // A port outside the allowed set. An arbitrary port turns a webhook into a
    // port scanner whose results are visible in the delivery log.
    PortNotAllowed = 4,
};

// The only two ports a webhook may use. Anything else is a request this process
// makes to a port an operator chose, and the delivery log reports whether it
// connected — which is a port scanner with a nice interface.
inline constexpr std::uint16_t kHttpsPort = 443;
inline constexpr std::uint16_t kAlternateHttpsPort = 8443;

// At registration, never at delivery.
//
// A URL that resolved to a private address yesterday has already been trusted, so
// a check at delivery time is a check that runs after the damage. It also cannot
// be the LAST line of defence: this rejects literal addresses, and a hostname
// that resolves to one is only caught by refusing the connection after
// resolution — which is the HTTP client's job and is stated here so nobody
// mistakes this for the whole answer.
[[nodiscard]] UrlVerdict check_webhook_url(std::string_view url) noexcept;

// True for an IPv4 or IPv6 literal that must never be connected to: loopback,
// any-address, link-local (including the 169.254.169.254 metadata address),
// unique-local, and the RFC 1918 ranges.
//
// Exposed separately because it is what an HTTP client has to call again AFTER
// resolving a hostname — the check above cannot see where a name points.
[[nodiscard]] bool is_private_address(std::string_view host) noexcept;

}  // namespace anvil::notifications
