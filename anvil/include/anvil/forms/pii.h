#pragma once

// The PII envelope: how an identity value is stored, searched and shown.
//
// An identity number is the worst thing a submissions collection can hold in
// plaintext. An Egyptian National ID encodes date of birth, governorate and
// gender; a passport number is a travel document; either way ONE database dump
// discloses full identity records for every applicant. Four properties answer
// that, and each is a decision rather than an implementation detail:
//
//   1. NEVER IN `ans`. The value is extracted before the submission document is
//      built, so there is no code path that writes it as an answer.
//   2. SEALED WITH A KEY THAT IS NOT THE DATABASE CREDENTIAL. AES-256-GCM from
//      anvil/crypto/aead.h. Holding the dump is not enough.
//   3. SEARCHABLE WITHOUT DECRYPTION, via HMAC-SHA256 under a SECOND key. A plain
//      SHA-256 of a structured 14-digit number is exhaustible in seconds — 10^14
//      candidates against a 7 GH/s hash is under a day on one GPU, and the
//      century and governorate digits cut the space by two more orders of
//      magnitude. The keyed construction is what makes the index safe to store
//      beside the ciphertext it indexes.
//   4. REDACTION HIDES THE PREFIX. In a structured number the FRONT is the
//      birth date, so a "partial" reveal of the first digits leaks far more than
//      the last four do.
//
// --- two keys, never one ----------------------------------------------------
//
// Encryption and blind indexing use SEPARATE keys held in one object, so a caller
// cannot pass the index key where the sealing key belongs. Deriving both from one
// master would mean a compromise of either purpose is a compromise of both — and
// the index key must be online for every write, while the sealing key need only
// be online for a decrypt.
//
// --- normalisation and redaction are POLICY, and anvil ships defaults --------
//
// Both were hard-coded in the system this was extracted from, and both are
// wrong for somebody. The default normaliser folds Arabic-Indic digits, strips
// spaces, hyphens and underscores, and uppercases ASCII — which is right for a
// national id and for a passport number, and wrong for anything whose separators
// are significant. The default redactor keeps the last four characters, which is
// right where the front carries the structure and wrong where the back does.
//
// So they are function pointers in a `PiiPolicy` the application hands over, with
// anvil's defaults named. What is NOT negotiable is that the blind index is over
// the NORMALISED value: two spellings of one identity must produce one digest, or
// duplicate detection silently stops working, and the application that changes
// the normaliser after the first row is written has changed what "duplicate"
// means for every row before it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/types.h"
#include "anvil/crypto/secret.h"
#include "anvil/forms/fid.h"

namespace anvil::forms {

inline constexpr std::size_t kPiiKeyBytes = 32;

// The whole authenticated context is a form id plus a field id, so it fits an
// array on the stack and never allocates.
inline constexpr std::size_t kPiiAadCapacity = 16 + Fid::kCapacity;

// The two keys, OWNED rather than borrowed. A span into configuration would be
// correct today and a dangling read the first time anything reloads config.
class PiiKeys final {
public:
    // Throws std::invalid_argument unless both spans are exactly 32 bytes and the
    // two DIFFER. One key used for both purposes means the ciphertext and the
    // index it is stored beside fall together.
    PiiKeys(std::span<const std::uint8_t> sealing, std::span<const std::uint8_t> indexing);

    [[nodiscard]] std::span<const std::uint8_t> sealing() const noexcept {
        return sealing_.span();
    }
    [[nodiscard]] std::span<const std::uint8_t> indexing() const noexcept {
        return indexing_.span();
    }

    PiiKeys(const PiiKeys&) = delete;
    PiiKeys& operator=(const PiiKeys&) = delete;

private:
    // Declaration order is initialisation order (ENGINEERING_RULES.md §3.2).
    crypto::SecretBuffer<kPiiKeyBytes> sealing_;
    crypto::SecretBuffer<kPiiKeyBytes> indexing_;
};

// The AEAD's additional authenticated data: the form id and the field id, so a
// ciphertext lifted into another field or another form fails to open rather than
// decrypting into the wrong record.
class PiiAad final {
public:
    [[nodiscard]] std::span<const std::uint8_t> span() const noexcept {
        return std::span<const std::uint8_t>{bytes_.data(), size_};
    }

    friend PiiAad pii_aad(const Uuid& form, std::string_view field_id) noexcept;

private:
    PiiAad() noexcept : bytes_{}, size_{0} {}

    std::array<std::uint8_t, kPiiAadCapacity> bytes_;
    std::uint8_t                              size_;
};

static_assert(sizeof(PiiAad) == kPiiAadCapacity + 1, "one small stack object, no heap");

// A field id longer than Fid::kCapacity is TRUNCATED rather than rejected: the
// only way to hold one is to have parsed it, so the caller has already proved the
// shape, and a silent overrun here would be worse than a binding one byte
// shorter. It is unreachable through `Fid` and checked anyway.
[[nodiscard]] PiiAad pii_aad(const Uuid& form, std::string_view field_id) noexcept;

// What gets stored: the sealed envelope and the digest that indexes it.
struct SealedIdentity final {
    std::vector<std::uint8_t> envelope;
    crypto::Digest256         blind_index;
};

// --- policy -----------------------------------------------------------------

// Canonicalises a value so that two spellings of one identity produce ONE blind
// index. It CANONICALISES; it does not validate — the field type's validator has
// already done that, and a normaliser that rejected would be a second, quieter
// place for a real document to be refused.
using IdentityNormaliser = std::string (*)(std::string_view raw);

// What a reader without the unseal authority is shown. Computed once, at seal
// time, from the value that is about to stop existing in plaintext — which is
// what keeps the DEFAULT read path key-free.
using IdentityRedactor = std::string (*)(std::string_view value);

// Folds Arabic-Indic and Extended Arabic-Indic digits to ASCII, drops spaces,
// hyphens and underscores, and uppercases ASCII letters. Nothing else is altered.
//
// The separator stripping is deliberate and it is what makes duplicate detection
// work: somebody who types "AB-123456" on one form and "AB123456" on the next is
// the same person, and an index that disagrees is an index that does not work.
[[nodiscard]] std::string normalise_identity(std::string_view raw);

// How many trailing CHARACTERS survive redaction. Four is the largest suffix that
// reveals nothing structural in the identity formats this was written against:
// the low digits of a serial carry no birth date, no governorate and no gender.
inline constexpr std::size_t kIdentityVisibleSuffix = 4;

// `••••••••••1234`. The mask is U+2022 per hidden CODE POINT, not per byte, so a
// value containing non-ASCII does not produce a mask three times its length. A
// value of four characters or fewer is masked ENTIRELY — revealing all of it
// because it is short is the opposite of the rule.
[[nodiscard]] std::string redact_identity(std::string_view value);

struct PiiPolicy final {
    IdentityNormaliser normalise;
    IdentityRedactor   redact;
};

inline constexpr PiiPolicy kDefaultPiiPolicy{&normalise_identity, &redact_identity};

// --- the envelope -----------------------------------------------------------

// HMAC-SHA256 over the NORMALISED value. A caller holding raw input must
// normalise first or two spellings of one identity will not collide.
[[nodiscard]] crypto::Digest256 identity_blind_index(const PiiKeys& keys,
                                                     std::string_view normalised);

// Seals and indexes in one call, so a caller cannot store one without the other.
// The IV is generated inside seal() and is never reused.
[[nodiscard]] SealedIdentity seal_identity(const PiiKeys& keys, std::string_view normalised,
                                           const PiiAad& aad);

// nullopt when the tag does not verify — tampered, truncated, wrong key, wrong
// form, wrong field. The caller cannot distinguish these, deliberately.
[[nodiscard]] std::optional<std::string> open_identity(const PiiKeys& keys,
                                                       std::span<const std::uint8_t> envelope,
                                                       const PiiAad& aad);

}  // namespace anvil::forms
