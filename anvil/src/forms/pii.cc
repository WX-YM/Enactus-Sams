#include "anvil/forms/pii.h"

#include <algorithm>
#include <stdexcept>

#include "anvil/crypto/aead.h"
#include "anvil/crypto/constant_time.h"
#include "anvil/crypto/digest.h"
#include "anvil/i18n/digits.h"
#include "anvil/i18n/utf8.h"

namespace anvil::forms {
namespace {

// U+2022 BULLET.
constexpr std::string_view kMaskCharacter = "\xE2\x80\xA2";

// Separators a human types into an identity number. DROPPED rather than
// rejected: "2980101 234567 8" and "29801012345678" are one identity, and a blind
// index that disagrees about which is which stops detecting duplicates.
[[nodiscard]] constexpr bool is_identity_separator(char c) noexcept {
    return c == ' ' || c == '-' || c == '_' || c == '\t';
}

}  // namespace

PiiKeys::PiiKeys(std::span<const std::uint8_t> sealing, std::span<const std::uint8_t> indexing)
    : sealing_{}, indexing_{} {
    if (sealing.size() != kPiiKeyBytes || indexing.size() != kPiiKeyBytes) {
        throw std::invalid_argument{"PiiKeys: both keys must be exactly 32 bytes"};
    }
    // Constant time, though neither value is attacker-supplied: the comparison
    // exists so that a misconfiguration is caught, and CLAUDE.md §5 admits no
    // exception for "this one cannot leak" — the next reader cannot tell which
    // comparisons were exempt and why.
    if (crypto::secure_equal(sealing, indexing)) {
        throw std::invalid_argument{
            "PiiKeys: the sealing and indexing keys must differ; one key for both purposes "
            "means a compromise of either is a compromise of both"};
    }
    std::copy(sealing.begin(), sealing.end(), sealing_.data());
    std::copy(indexing.begin(), indexing.end(), indexing_.data());
}

PiiAad pii_aad(const Uuid& form, std::string_view field_id) noexcept {
    PiiAad aad{};
    std::copy(form.begin(), form.end(), aad.bytes_.begin());
    aad.size_ = static_cast<std::uint8_t>(form.size());
    const std::size_t taken = std::min(field_id.size(), Fid::kCapacity);
    for (std::size_t i = 0; i < taken; ++i) {
        aad.bytes_[aad.size_ + i] = static_cast<std::uint8_t>(field_id[i]);
    }
    aad.size_ = static_cast<std::uint8_t>(aad.size_ + taken);
    return aad;
}

std::string normalise_identity(std::string_view raw) {
    // Folded first: the separators below are ASCII, and a folded string is the
    // only form in which the digit positions are meaningful.
    const std::string folded =
        i18n::has_non_ascii_digits(raw) ? i18n::fold_digits(raw) : std::string{raw};

    std::string out;
    out.reserve(folded.size());
    for (const char c : folded) {
        if (is_identity_separator(c)) { continue; }
        out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    return out;
}

crypto::Digest256 identity_blind_index(const PiiKeys& keys, std::string_view normalised) {
    return crypto::hmac_sha256(keys.indexing(), normalised);
}

SealedIdentity seal_identity(const PiiKeys& keys, std::string_view normalised,
                             const PiiAad& aad) {
    SealedIdentity sealed{};
    sealed.envelope = crypto::seal(keys.sealing(), normalised, aad.span());
    sealed.blind_index = identity_blind_index(keys, normalised);
    return sealed;
}

std::optional<std::string> open_identity(const PiiKeys& keys,
                                         std::span<const std::uint8_t> envelope,
                                         const PiiAad& aad) {
    return crypto::open(keys.sealing(), envelope, aad.span());
}

std::string redact_identity(std::string_view value) {
    const std::size_t code_points = i18n::count_code_points(value);
    // A short value reveals nothing: masking it entirely is the conservative
    // reading of "never the structural prefix", not a special case bolted on.
    const std::size_t visible =
        code_points > kIdentityVisibleSuffix ? kIdentityVisibleSuffix : 0;
    const std::size_t hidden = code_points - visible;

    // The hidden prefix AS A VIEW gives the byte offset the visible suffix starts
    // at, on a code-point boundary. Slicing at an arbitrary byte offset would
    // split a multi-byte sequence and produce invalid UTF-8, which then fails
    // validation everywhere downstream.
    const std::size_t suffix_offset = i18n::truncate_to_code_points(value, hidden).size();

    std::string out;
    out.reserve(hidden * kMaskCharacter.size() + (value.size() - suffix_offset));
    for (std::size_t i = 0; i < hidden; ++i) { out.append(kMaskCharacter); }
    if (visible != 0) { out.append(value.substr(suffix_offset)); }
    return out;
}

}  // namespace anvil::forms
