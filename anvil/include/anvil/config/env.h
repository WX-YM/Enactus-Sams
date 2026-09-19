#pragma once

// Reading configuration out of the environment, once, at boot.
//
// anvil ships the READERS. It does not ship a Config struct, because every
// application's settings are its own — the system anvil came from had a fixed
// fifty-member struct and a process singleton, and roughly a third of those
// members meant nothing outside that product.
//
// Two rules the readers enforce rather than document:
//
//   1. A configuration error is a BOOT FAILURE, never a degraded mode. Every
//      function here throws ConfigError naming the variable and the rule it
//      broke. A server that starts with no signing key, or with its storage root
//      inside the web root, is worse than a server that does not start: the first
//      is a silent compromise and the second is a pager.
//   2. An EMPTY variable is an UNSET variable. A deployment that exports
//      SESSION_PEPPER="" has made a mistake, and treating it as present produces
//      a much later and much stranger failure than treating it as missing.
//
// Secrets are decoded straight into a SecretBuffer and never pass through a
// std::string, which would leave an un-zeroable copy on the heap for the
// allocator to hand to whoever asks next.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "anvil/crypto/base64url.h"
#include "anvil/crypto/secret.h"

namespace anvil::config {

// Thrown by every reader here. The message names the variable and states the
// rule it broke; it never contains the offending value, because a configuration
// error reaches a log and a secret that reaches a log has leaked.
class ConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Injected rather than calling getenv directly, so a test can supply a fixed map.
// Calling setenv from a test is a data race against every other test in the
// binary, and the failure is intermittent and blamed on something else.
using EnvLookup = std::function<const char*(const char*)>;

// The real environment.
[[nodiscard]] EnvLookup system_environment() noexcept;

// nullopt for unset OR empty. Every reader below is built on this.
[[nodiscard]] std::optional<std::string_view> lookup(const EnvLookup& env, const char* key);

// --- strings ---------------------------------------------------------------
[[nodiscard]] std::string required_string(const EnvLookup& env, const char* key);
[[nodiscard]] std::string optional_string(const EnvLookup& env, const char* key,
                                          std::string_view fallback);

// --- numbers ---------------------------------------------------------------
// from_chars, never stoull: stoull throws a type that says nothing about which
// variable was wrong, and it accepts leading whitespace and a sign.
[[nodiscard]] std::uint64_t optional_number(const EnvLookup& env, const char* key,
                                            std::uint64_t fallback);
[[nodiscard]] std::uint64_t bounded_number(const EnvLookup& env, const char* key,
                                           std::uint64_t fallback, std::uint64_t min,
                                           std::uint64_t max);

// "1", "true", "yes", "on" and their negatives, case-insensitively. Anything else
// is an error rather than a silent false: a typo in a variable that disables a
// security control must not read as "disabled".
[[nodiscard]] bool optional_bool(const EnvLookup& env, const char* key, bool fallback);

// --- keys ------------------------------------------------------------------
// Exactly N bytes of unpadded base64url, decoded straight into `out`.
//
// `required` distinguishes a key whose absence is a boot failure from one whose
// absence disables an optional transport. The distinction matters: a deployment
// that booted without a sealing key would encrypt under a zero key and nothing
// downstream would report it, whereas a deployment with no push key simply has
// no push.
template <std::size_t N>
void load_key(const EnvLookup& env, const char* key, crypto::SecretBuffer<N>& out,
              bool required) {
    const std::optional<std::string_view> text = lookup(env, key);
    if (!text.has_value()) {
        if (required) {
            throw ConfigError{std::string{key} + " is required: " + std::to_string(N) +
                              " CSPRNG bytes as unpadded base64url"};
        }
        return;
    }
    const std::optional<std::size_t> written =
        crypto::base64url_decode_into(*text, out.mutable_span());
    if (!written.has_value() || *written != N) {
        throw ConfigError{std::string{key} + " must be exactly " + std::to_string(N) +
                          " bytes of unpadded base64url"};
    }
}

// --- shapes ----------------------------------------------------------------

// An origin is compared BYTE FOR BYTE against the Origin header, which browsers
// send as scheme "://" host [":" port] and nothing else. Accepting a trailing
// slash or a path here produces a comparison that can never match, and the
// symptom is every mutating request failing its CSRF check — in production only,
// because a developer's origin usually has neither.
//
// http is permitted for localhost so a developer is not forced to terminate TLS
// locally. Anything else must be https: `__Host-` cookies and SameSite
// guarantees are meaningless over cleartext.
void require_origin_shape(const char* key, std::string_view origin);

// Two settings that must differ, named in the message so the operator knows
// which pair. Used for any two keys where deriving one from the other would make
// a compromise of either a compromise of both.
void require_distinct(const char* key_a, std::string_view a,
                      const char* key_b, std::string_view b);

// The origin split, as ONE call: the shape of both, then the separation between
// them (docs/19-server-side-rendering.md §6).
//
// It is one function rather than three calls at the boot site because both
// failure directions are silent. A deployment that sets CONTENT_ORIGIN equal to
// SITE_ORIGIN still works, still serves every page, and has lost the isolation
// that contains an XSS in assembled HTML — `__Host-` session cookies are
// host-only only with respect to a DIFFERENT host. A boot site that called the
// shape check and forgot the separation would look correct in review.
//
// The comparison is on the HOST, with the scheme and any port removed, and that
// is not a convenience: cookies are not port-scoped. `https://x.test` and
// `https://x.test:8443` are two byte-distinct origins that a browser treats as
// one host for every cookie it holds, so a plain inequality would accept a
// deployment that has the split in its configuration and not in its browser.
//
// What it does NOT check is that the two are different REGISTRABLE domains,
// which would need a public-suffix list this library does not ship. A sibling
// subdomain satisfies the property this check is about — a host-only cookie is
// never sent to it — and leaves the wider one to the deployment.
void require_origin_split(const char* site_key, std::string_view site_origin,
                          const char* content_key, std::string_view content_origin);

// A database name reaches the driver as part of a namespace, so the characters
// the server forbids in one are refused here rather than at the first query.
// None of this is request data — a namespace is never derived from a request —
// but an operator typo should fail at boot with the variable named, not as an
// unexplained driver error hours later.
void require_database_name(const char* key, std::string_view name);

// An absolute path that exists, is a directory, and is not a symlink.
//
// Returns the CANONICAL path: resolving once at boot and storing the result is
// what lets every later open be relative to a descriptor rather than resolved by
// name again, which is the whole TOCTOU defence in anvil/fs/paths.h.
[[nodiscard]] std::string require_directory(const char* key, std::string_view path);

// --- the boot summary ------------------------------------------------------
//
// An application logs one of these at startup. It exists so an operator can
// confirm what took effect — a count is often the only thing that distinguishes
// a correct deployment from a silently broken one, as with the trusted-proxy list
// where both an empty and an over-wide value behave plausibly.
//
// It has no access to key material, which is the point: an application that can
// only render its configuration through this cannot leak a secret by logging.
class RedactedSummary final {
public:
    RedactedSummary& line(std::string_view key, std::string_view value);
    RedactedSummary& line(std::string_view key, std::uint64_t value);
    // Renders "set" or "UNSET" — never the value, never its length. A length is
    // a meaningful hint about a short secret.
    RedactedSummary& secret(std::string_view key, bool present);

    [[nodiscard]] std::string str() const& { return text_; }
    [[nodiscard]] std::string str() && { return std::move(text_); }

private:
    std::string text_;
};

}  // namespace anvil::config
