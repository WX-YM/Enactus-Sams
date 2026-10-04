#pragma once

// Grants: the server's agreement to serve one private object, carried to an
// origin where it cannot be re-made (docs/22-chat.md §6.1).
//
// Nothing in storage is public. Every byte leaves through a handler that chose
// to issue X-Accel-Redirect, so the question for a PRIVATE namespace is never
// "is it protected" but "what can that handler check". It runs on the media
// origin, where the host-only session cookie never arrives, so it cannot ask
// who is calling. Two fixes are refused:
//
//   * serving private media from the site origin, with the session — a stored
//     object is bytes a user chose, and serving them from the origin that holds
//     the session is the XSS the origin split exists to prevent;
//   * a capability cookie on the media origin, as the preview uses — the
//     preview is a top-level navigation, and an <img> or fetch on the site's
//     page is a CROSS-SITE subresource request, on which a browser sends no
//     SameSite=Lax cookie.
//
// So access is decided on the SITE origin, where the user is known, by the code
// that is already reading the object's owning document, and that decision is
// written down as a grant. The media origin opens it.
//
// --- sealed, not signed -----------------------------------------------------
//
// AES-256-SIV over `ns ‖ id ‖ expiry`. Sealed so the client cannot read the
// object id out of it: a client is told it can pull this, and nothing taken
// from storage (§6.2). Deterministic so the same object in the same expiry
// bucket is the same URL and the browser cache works; equality is the one thing
// it reveals, and equality is what a cache needs to see.
//
// --- the same machinery is the upload handle ---------------------------------
//
// An upload into a private namespace answers with a HANDLE, `ns ‖ id ‖
// uploader ‖ expiry` sealed the same way, instead of the object id, and nothing
// else: no hash, no size, no owner, no "this already existed". Only a request
// from the same account can redeem it, so an object id learned anywhere else —
// a log, a screenshot of devtools, another application on the namespace —
// cannot be attached to a document its holder owns and turned into a grant.
// Grants and handles are told apart by their associated data, so neither ever
// opens as the other.
//
// --- layout -----------------------------------------------------------------
//
//   [0]     key id, in the clear, so open() picks the key rather than trying
//           both — the shape auth::TokenKeys uses for rotation
//   [1..]   AES-256-SIV(aad = purpose ‖ key id) of the body
//
// Body, big-endian:  ns(1) ‖ id(16) ‖ expires_at_unix(4) [‖ uploader(16)]
//
// No version byte: both live minutes, so a format change needs no migration,
// and every byte is a byte of URL.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "anvil/core/types.h"
#include "anvil/crypto/secret.h"
#include "anvil/crypto/siv.h"
#include "anvil/fs/namespace.h"

namespace anvil::media {

// How long a serving grant lives: until the end of the NEXT bucket, so between
// one and two buckets. Bucketed rather than "now plus ten minutes" because the
// expiry is in the sealed body, and a body that changed every second would be a
// new URL every second. The upper bound is the revocation latency for a member
// removed from a conversation: grants they were already handed keep working for
// at most this long, for objects they were entitled to when they got them.
inline constexpr std::int64_t kGrantBucketSeconds = 600;

// A handle has to survive a slow composer, not a scroll.
inline constexpr std::int64_t kUploadHandleSeconds = 3600;

// The encoded lengths, exact. open() refuses anything else before it decodes a
// byte, so a hostile path segment costs a length comparison.
inline constexpr std::size_t kGrantChars = 51;
inline constexpr std::size_t kUploadHandleChars = 72;

// The keys grants and handles are sealed under. Immutable after construction,
// shared as a shared_ptr<const GrantKeys>, swapped atomically on rotation
// (CLAUDE.md §4). Mint with CURRENT, open with whichever the key id names.
//
// Rotation costs nothing visible: a grant lives at most two buckets, so a
// deployment that keeps the previous key for that long breaks no open page.
class GrantKeys final {
public:
    static constexpr std::size_t kKeyBytes = crypto::kSivKeyBytes;

    // Throws std::invalid_argument on a wrong-sized key or on two keys sharing
    // an id: an id that named both would make open() unable to say which.
    GrantKeys(std::uint8_t current_kid, std::span<const std::uint8_t> current_key);
    GrantKeys(std::uint8_t current_kid, std::span<const std::uint8_t> current_key,
              std::uint8_t previous_kid, std::span<const std::uint8_t> previous_key);

    GrantKeys(const GrantKeys&) = delete;
    GrantKeys& operator=(const GrantKeys&) = delete;

    [[nodiscard]] std::uint8_t current_kid() const noexcept { return current_kid_; }
    [[nodiscard]] std::span<const std::uint8_t> current() const noexcept {
        return {current_.data(), current_.size()};
    }
    // Empty when the id names no key held.
    [[nodiscard]] std::span<const std::uint8_t> key_for(std::uint8_t kid) const noexcept;

private:
    crypto::SecretBuffer<kKeyBytes> current_;
    crypto::SecretBuffer<kKeyBytes> previous_;
    std::uint8_t                    current_kid_;
    std::uint8_t                    previous_kid_;
    bool                            has_previous_;
};

// The one argument that lets a PRIVATE namespace be served
// (accel_redirect_response). Only open_grant() makes one, so an application's
// own media handler — or any refactor of it — cannot serve a private object by
// passing an id it found somewhere: there is no way to construct the argument.
class MediaGrant final {
public:
    [[nodiscard]] const Uuid& id() const noexcept { return id_; }
    [[nodiscard]] fs::Ns ns() const noexcept { return ns_; }
    // When the grant stops opening, in Unix seconds: the most a response
    // served under it may be cached for (media/grant_route.h).
    [[nodiscard]] std::int64_t expires_unix() const noexcept { return expires_unix_; }

private:
    friend std::optional<MediaGrant> open_grant(const GrantKeys&, std::string_view,
                                                std::int64_t) noexcept;
    MediaGrant(std::int64_t expires_unix, const Uuid& id, fs::Ns ns) noexcept
        : expires_unix_{expires_unix}, id_{id}, ns_{ns} {}

    std::int64_t expires_unix_;
    Uuid         id_;
    fs::Ns       ns_;
};

// The expiry a grant minted now carries: the end of the next bucket.
[[nodiscard]] constexpr std::int64_t grant_expiry(std::int64_t now_unix) noexcept {
    const std::int64_t bucket = now_unix >= 0 ? now_unix / kGrantBucketSeconds : 0;
    return (bucket + 2) * kGrantBucketSeconds;
}

// Called by code that has ALREADY decided the caller may see this object. It
// checks nothing itself; it writes the decision down.
[[nodiscard]] std::string mint_grant(const GrantKeys& keys, fs::Ns ns, const Uuid& id,
                                     std::int64_t now_unix);

// nullopt for anything but a well-formed, authentic, unexpired grant — and the
// caller cannot tell which, deliberately: the input is a URL segment anyone can
// edit, and a distinguishable failure is an oracle. Answers the stealth 404.
[[nodiscard]] std::optional<MediaGrant> open_grant(const GrantKeys& keys, std::string_view grant,
                                                   std::int64_t now_unix) noexcept;

// What an upload handle names, once opened by the account that made it.
struct UploadClaim final {
    Uuid   id;
    fs::Ns ns;
};

[[nodiscard]] std::string mint_upload_handle(const GrantKeys& keys, fs::Ns ns, const Uuid& id,
                                             const Uuid& uploader, std::int64_t now_unix);

// nullopt unless the handle is authentic, unexpired AND was made for
// `redeemer`. The uploader is compared inside the sealed body, so a handle
// passed from one account to another is refused exactly like a forged one.
[[nodiscard]] std::optional<UploadClaim> open_upload_handle(const GrantKeys& keys,
                                                            std::string_view handle,
                                                            const Uuid& redeemer,
                                                            std::int64_t now_unix) noexcept;

}  // namespace anvil::media
