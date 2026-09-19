#pragma once

// Unicode normalisation.
//
// Normalise ONCE, at the trust boundary, then trust the stored form. Never
// normalise on read: that is per-request ICU work on a hot path
// (docs/03-i18n-utf8.md §6).
//
// The mode is not a preference, it is a correctness requirement per field
// class, and getting it wrong produces bugs that only appear for Arabic users:
//
//   Password   NFC only. Never trimmed, never case-folded. The same passphrase
//              typed on two keyboards otherwise hashes differently and the user
//              simply cannot log in.
//   Identifier NFKC. Arabic presentation forms (U+FB50..U+FEFF) are legacy
//              compatibility code points that NFC does NOT collapse, so without
//              NFKC `ﻗﻬﻮﺓ` and `قهوة` register as two distinct usernames.
//   Display    NFC. Preserves what the user typed, including tashkeel.
//   Search     NFC plus aggressive Arabic folding — stored in a SEPARATE field,
//              never overwriting the display text.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::i18n {

enum class NormalizeMode : std::uint8_t {
    Nfc,           // display text, passwords
    Nfkc,          // identifiers
    NfkcCaseFold,  // usernames, email local parts
};

// Returns nullopt only if ICU itself fails, which is a configuration or memory
// problem rather than bad input — malformed UTF-8 is rejected earlier.
[[nodiscard]] std::optional<std::string> normalize(std::string_view text, NormalizeMode mode);

// True when `text` is already in the target form. Cheaper than normalising, so
// the common case (already-normalised ASCII) avoids the allocation entirely.
[[nodiscard]] bool is_normalized(std::string_view text, NormalizeMode mode);

// Search key: NFC, then strip tatweel (U+0640) and tashkeel (U+064B..U+0652),
// then fold the letter variants users type interchangeably —
//   أ إ آ ٱ -> ا      ة -> ه      ى -> ي      ؤ -> و      ئ -> ي
// This is what makes searching for "قهوه" find a record stored as "قهوة".
//
// Store this in its own field. Users must see their own tashkeel; the index
// must not care about it.
[[nodiscard]] std::string fold_for_search(std::string_view text);

// Confusable skeleton for identifier collision detection: maps visually
// interchangeable characters to a single representative, so Cyrillic `а` and
// Latin `a` collide and a homoglyph username cannot be registered alongside a
// real one. Compare skeletons, store the original.
[[nodiscard]] std::string confusable_skeleton(std::string_view text);

}  // namespace anvil::i18n
