#pragma once

// Arabic-Indic digit folding.
//
// An Egyptian user typing their National ID on an Arabic keyboard produces
//     ٢٩٨٠١٠١٢٣٤٥٦٧
// which is fourteen digits that a `^[0-9]{14}$` check rejects. Rejecting it is
// a defect, not a security control.
//
// This runs BEFORE every numeric validation: National ID, phone numbers, dates,
// and every NUMBER form field.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace anvil::i18n {

// U+0660..U+0669 (Arabic-Indic) and U+06F0..U+06F9 (Extended, used for Persian
// and Urdu) both map to ASCII 0-9. They are visually distinct but semantically
// identical, and users copy-paste across both.
[[nodiscard]] std::string fold_digits(std::string_view text);

// True when the text contains at least one non-ASCII digit, so a caller can
// skip the allocation in the common all-ASCII case.
[[nodiscard]] bool has_non_ascii_digits(std::string_view text) noexcept;

// Folds into a caller-provided buffer and returns the count written, or nullopt
// if it does not fit. Zero allocation — this is the form the National ID
// validator uses to stay within its 200 ns budget.
[[nodiscard]] std::optional<std::size_t> fold_digits_into(std::string_view text,
                                                          std::span<char> out) noexcept;

}  // namespace anvil::i18n
