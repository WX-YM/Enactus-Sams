#pragma once

// The one reusable Argon2 component.
//
// Two callers need the same primitive for two different reasons:
// `auth::PasswordHasher` hashes a password directly, and `auth::prehash`
// (docs/05-auth-sessions.md, client prehash) hashes the SAME way at the client
// stage during offline enrolment, and again — over a different input, at the
// server's own parameters — for the optional Argon2id server stage. A second
// copy of the argon2_context plumbing is a second place to get the version, the
// flag or the error mapping wrong, so this header is that plumbing and nothing
// else: no policy, no salt derivation, no encoding choices beyond what
// libargon2 itself defines.
//
// Three shapes, because the two callers need different subsets:
//
//   argon2_hash_raw          the general primitive. Any of the three Argon2
//                            types, an explicit version, and an optional secret
//                            and associated data — everything argon2_context
//                            exposes. This is what a known-answer test needs and
//                            what the encoded forms below are built from.
//   argon2id_hash_encoded    the PHC string form, argon2id only, no secret or
//                            associated data (libargon2's own *_hash_encoded
//                            entry points carry neither into the string). This
//                            is `auth::PasswordHasher::hash`'s wire format.
//   argon2id_verify_encoded  the matching verify, and argon2id_parse_params the
//                            matching parameter reader.
//
// Every call here BLOCKS and, at policy parameters, reserves tens of megabytes.
// It must run on hash_pool and never on a Trantor event-loop thread
// (CLAUDE.md §4) — this header enforces neither; the pool-aware wrapper does.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace anvil::crypto {

enum class Argon2Type : std::uint8_t { Argon2d = 0, Argon2i = 1, Argon2id = 2 };

// The version RFC 9106 and every hash in this codebase use. Named rather than
// left as a bare 0x13 at every call site.
inline constexpr std::uint32_t kArgon2Version13 = 0x13;

struct Argon2Params final {
    std::uint32_t memory_kib;
    std::uint32_t iterations;
    std::uint32_t parallelism;

    [[nodiscard]] constexpr bool operator==(const Argon2Params&) const noexcept = default;
};

enum class VerifyOutcome : std::uint8_t {
    Match = 0,
    Mismatch,
    Malformed,   // the stored/encoded form is not one this primitive can parse
};

// The general primitive: fills `out` completely. `secret` and `associated_data`
// may be empty spans — RFC 9106 §5.1-5.3's vectors are the reason they exist at
// all here, since neither `auth::PasswordHasher` nor the prehash client stage
// uses them, and the argon2id server stage (docs/05 client-prehash) does not
// either. Throws CryptoError on any failure: a bad parameter combination is a
// programmer error, not a request-shaped one, because every parameter here
// comes from configuration or from a stage policy, never from the wire.
void argon2_hash_raw(Argon2Type type, std::uint32_t version, const Argon2Params& params,
                     std::span<const std::uint8_t> password, std::span<const std::uint8_t> salt,
                     std::span<const std::uint8_t> secret,
                     std::span<const std::uint8_t> associated_data, std::span<std::uint8_t> out);

// The PHC-encoded form: argon2id, version 0x13, no secret or associated data.
// Throws CryptoError on failure (including a hash_bytes/params combination too
// large for the internal encode buffer).
[[nodiscard]] std::string argon2id_hash_encoded(const Argon2Params& params,
                                                std::span<const std::uint8_t> password,
                                                std::span<const std::uint8_t> salt,
                                                std::size_t hash_bytes);

// Match / Mismatch is a plain wrong password; Malformed is the encoded string
// itself failing to parse, which callers must map to the SAME client-visible
// answer as Mismatch — Malformed exists only so the server side can tell a
// corrupt row worth alerting on from an ordinary failed login.
[[nodiscard]] VerifyOutcome argon2id_verify_encoded(std::string_view encoded,
                                                    std::span<const std::uint8_t> password);

// Cost parameters out of an argon2id PHC string, or nullopt if it does not
// parse. Hand-written (from_chars over a linear scan), never std::regex
// (CLAUDE.md §5).
[[nodiscard]] std::optional<Argon2Params> argon2id_parse_params(
    std::string_view encoded) noexcept;

}  // namespace anvil::crypto
