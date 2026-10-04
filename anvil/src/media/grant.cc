#include "anvil/media/grant.h"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <vector>

#include "anvil/crypto/base64url.h"

namespace anvil::media {
namespace {

// The associated data names what a sealed value is FOR, so a value minted for
// one purpose never opens as another even under the same key. The key id rides
// in it too, so the cleartext byte that chose the key is itself authenticated.
constexpr std::string_view kGrantPurpose = "anvil.media.grant";
constexpr std::string_view kUploadPurpose = "anvil.media.upload";

constexpr std::size_t kBodyBytes = 1 + 16 + 4;
constexpr std::size_t kSealedBytes = 1 + crypto::kSivTagBytes + kBodyBytes;
constexpr std::size_t kHandleBodyBytes = kBodyBytes + 16;
constexpr std::size_t kHandleSealedBytes = 1 + crypto::kSivTagBytes + kHandleBodyBytes;

static_assert(crypto::base64url_decoded_size(kGrantChars) == kSealedBytes,
              "kGrantChars must be exactly the encoded length of a grant");
static_assert(crypto::base64url_decoded_size(kUploadHandleChars) == kHandleSealedBytes,
              "kUploadHandleChars must be exactly the encoded length of a handle");

[[nodiscard]] std::array<std::uint8_t, 32> associated_data(std::string_view purpose,
                                                           std::uint8_t kid,
                                                           std::size_t& used) noexcept {
    std::array<std::uint8_t, 32> aad{};
    used = 0;
    for (const char c : purpose) { aad[used++] = static_cast<std::uint8_t>(c); }
    aad[used++] = kid;
    return aad;
}

void put_u32(std::span<std::uint8_t> out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 24U);
    out[1] = static_cast<std::uint8_t>(value >> 16U);
    out[2] = static_cast<std::uint8_t>(value >> 8U);
    out[3] = static_cast<std::uint8_t>(value);
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::uint8_t> in) noexcept {
    return (std::uint32_t{in[0]} << 24U) | (std::uint32_t{in[1]} << 16U) |
           (std::uint32_t{in[2]} << 8U) | std::uint32_t{in[3]};
}

// An expiry past 2106 does not fit the body's four bytes. Clamped rather than
// wrapped: a wrapped expiry is a grant that is already expired, which is safe,
// but a clamped one is the honest value for a clock that far out.
[[nodiscard]] std::uint32_t to_u32_seconds(std::int64_t unix_seconds) noexcept {
    if (unix_seconds <= 0) { return 0; }
    constexpr std::int64_t kMax = std::int64_t{UINT32_MAX};
    return static_cast<std::uint32_t>(std::min(unix_seconds, kMax));
}

}  // namespace

GrantKeys::GrantKeys(std::uint8_t current_kid, std::span<const std::uint8_t> current_key)
    : current_{},
      previous_{},
      current_kid_{current_kid},
      previous_kid_{0},
      has_previous_{false} {
    if (current_key.size() != kKeyBytes) {
        throw std::invalid_argument{"grant key must be 64 bytes"};
    }
    std::copy(current_key.begin(), current_key.end(), current_.data());
}

GrantKeys::GrantKeys(std::uint8_t current_kid, std::span<const std::uint8_t> current_key,
                     std::uint8_t previous_kid, std::span<const std::uint8_t> previous_key)
    : current_{},
      previous_{},
      current_kid_{current_kid},
      previous_kid_{previous_kid},
      has_previous_{true} {
    if (current_key.size() != kKeyBytes || previous_key.size() != kKeyBytes) {
        throw std::invalid_argument{"grant keys must be 64 bytes"};
    }
    if (current_kid == previous_kid) {
        throw std::invalid_argument{"grant keys must have distinct ids"};
    }
    std::copy(current_key.begin(), current_key.end(), current_.data());
    std::copy(previous_key.begin(), previous_key.end(), previous_.data());
}

std::span<const std::uint8_t> GrantKeys::key_for(std::uint8_t kid) const noexcept {
    if (kid == current_kid_) { return {current_.data(), current_.size()}; }
    if (has_previous_ && kid == previous_kid_) { return {previous_.data(), previous_.size()}; }
    return {};
}

namespace {

// Seals `body` under the current key for `purpose` and encodes it with the key
// id in front. One body layout for both purposes, so the two cannot drift.
template <std::size_t Body>
[[nodiscard]] std::string seal(const GrantKeys& keys, std::string_view purpose,
                               const std::array<std::uint8_t, Body>& body) {
    std::size_t aad_size = 0;
    const auto aad = associated_data(purpose, keys.current_kid(), aad_size);
    const std::vector<std::uint8_t> sealed =
        crypto::siv_seal(keys.current(), body, std::span<const std::uint8_t>{aad.data(), aad_size});

    std::array<std::uint8_t, 1 + crypto::kSivTagBytes + Body> out{};
    out[0] = keys.current_kid();
    std::copy(sealed.begin(), sealed.end(), out.begin() + 1);
    return crypto::base64url_encode(out);
}

// The authenticated body of an encoded value, or nullopt for anything that is
// not exactly that: wrong length, wrong alphabet, an unknown key id, a tag that
// does not verify, or the other purpose's value.
template <std::size_t Body>
[[nodiscard]] std::optional<std::vector<std::uint8_t>> unseal(const GrantKeys& keys,
                                                              std::string_view purpose,
                                                              std::string_view encoded,
                                                              std::size_t encoded_chars) noexcept {
    if (encoded.size() != encoded_chars) { return std::nullopt; }
    std::array<std::uint8_t, 1 + crypto::kSivTagBytes + Body> raw{};
    const std::optional<std::size_t> decoded = crypto::base64url_decode_into(encoded, raw);
    if (!decoded.has_value() || *decoded != raw.size()) { return std::nullopt; }

    const std::uint8_t kid = raw[0];
    const std::span<const std::uint8_t> key = keys.key_for(kid);
    if (key.empty()) { return std::nullopt; }

    std::size_t aad_size = 0;
    const auto aad = associated_data(purpose, kid, aad_size);
    std::optional<std::vector<std::uint8_t>> body;
    try {
        body = crypto::siv_open(key, std::span<const std::uint8_t>{raw}.subspan(1),
                                std::span<const std::uint8_t>{aad.data(), aad_size});
    } catch (...) {
        // An allocation failure or a key the constructor already sized: either
        // way not a value, and this runs on a request path that must not throw.
        return std::nullopt;
    }
    if (!body.has_value() || body->size() != Body) { return std::nullopt; }
    return body;
}

template <std::size_t Body>
[[nodiscard]] std::array<std::uint8_t, Body> body_of(fs::Ns ns, const Uuid& id,
                                                     std::int64_t expires_unix) noexcept {
    std::array<std::uint8_t, Body> body{};
    body[0] = ns.index();
    std::copy(id.begin(), id.end(), body.begin() + 1);
    put_u32(std::span<std::uint8_t>{body}.subspan(17, 4), to_u32_seconds(expires_unix));
    return body;
}

}  // namespace

std::string mint_grant(const GrantKeys& keys, fs::Ns ns, const Uuid& id, std::int64_t now_unix) {
    return seal(keys, kGrantPurpose, body_of<kBodyBytes>(ns, id, grant_expiry(now_unix)));
}

std::optional<MediaGrant> open_grant(const GrantKeys& keys, std::string_view grant,
                                     std::int64_t now_unix) noexcept {
    const std::optional<std::vector<std::uint8_t>> body =
        unseal<kBodyBytes>(keys, kGrantPurpose, grant, kGrantChars);
    if (!body.has_value()) { return std::nullopt; }

    // Every field below is authenticated: nothing is branched on until the
    // tag has verified, the same rule auth::decode keeps.
    const std::optional<fs::Ns> ns = fs::Ns::from_index((*body)[0]);
    if (!ns.has_value()) { return std::nullopt; }
    const std::uint32_t expires = get_u32(std::span<const std::uint8_t>{*body}.subspan(17, 4));
    if (now_unix >= std::int64_t{expires}) { return std::nullopt; }

    Uuid id{};
    std::copy_n(body->begin() + 1, id.size(), id.begin());
    return MediaGrant{std::int64_t{expires}, id, *ns};
}

std::string mint_upload_handle(const GrantKeys& keys, fs::Ns ns, const Uuid& id,
                               const Uuid& uploader, std::int64_t now_unix) {
    std::array<std::uint8_t, kHandleBodyBytes> body =
        body_of<kHandleBodyBytes>(ns, id, now_unix + kUploadHandleSeconds);
    std::copy(uploader.begin(), uploader.end(), body.begin() + kBodyBytes);
    return seal(keys, kUploadPurpose, body);
}

std::optional<UploadClaim> open_upload_handle(const GrantKeys& keys, std::string_view handle,
                                              const Uuid& redeemer,
                                              std::int64_t now_unix) noexcept {
    const std::optional<std::vector<std::uint8_t>> body =
        unseal<kHandleBodyBytes>(keys, kUploadPurpose, handle, kUploadHandleChars);
    if (!body.has_value()) { return std::nullopt; }

    const std::optional<fs::Ns> ns = fs::Ns::from_index((*body)[0]);
    if (!ns.has_value()) { return std::nullopt; }
    const std::uint32_t expires = get_u32(std::span<const std::uint8_t>{*body}.subspan(17, 4));
    if (now_unix >= std::int64_t{expires}) { return std::nullopt; }
    // Not a secret comparison — the uploader is an account id the redeemer
    // already knows is theirs — so a plain compare is the right one.
    if (!std::equal(redeemer.begin(), redeemer.end(), body->begin() + kBodyBytes)) {
        return std::nullopt;
    }

    Uuid id{};
    std::copy_n(body->begin() + 1, id.size(), id.begin());
    return UploadClaim{id, *ns};
}

}  // namespace anvil::media
