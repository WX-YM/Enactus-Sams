#pragma once

// Egyptian mobile number validation and normalisation.
//
// One person writes the same number six ways on the same day:
//
//     01012345678   0101 234 5678   +201012345678
//     00201012345678   201012345678   ٠١٠١٢٣٤٥٦٧٨
//
// All six are one account, so all six have to reduce to one string before
// anything is stored or looked up. The pipeline mirrors validate_national_id_egy
// and runs entirely in automatic storage:
//
//   1. fold Arabic-Indic digits to ASCII
//   2. strip spaces, hyphens, parentheses and a leading '+' — and nothing else,
//      because a letter is a typo rather than noise to filter out
//   3. canonicalise the dialling prefix: 0020 -> 20, 20 -> 20, bare 01… -> 20
//   4. `+20`, then `1`, then an operator digit, then eight digits
//   5. store E.164 and NOTHING else. Two spellings of one number is how a
//      filter and its consumer come to disagree about what a string says
//
//
// A LANDLINE is rejected with NotAllowed rather than BadFormat. The field's
// stated purpose is "we'll text you if it goes before you get here", so a
// landline quietly accepted is a customer nobody can reach — and the two
// reasons let the UI say which of them happened.
//
// **No std::regex.** It is banned on every request path (CLAUDE.md §5) and this
// is a hand-written linear scan over a stack buffer, exactly as every other
// validator here is.

#include <array>
#include <cstdint>
#include <string_view>

#include "anvil/input/fields.h"

namespace anvil::input {

// The operators, and the whole of the table. Adding a fifth is one line and a
// recompile rather than an edit to a pattern nobody can read.
//
//   0 Vodafone   1 Etisalat   2 Orange   5 WE
inline constexpr std::array<char, 4> kEgyptianOperatorDigits{{'0', '1', '2', '5'}};

// The calling code, spelled once. The scanner writes it into `e164` and anything
// that reads a number back checks for it; two spellings of one constant is how
// the writer and the reader come to disagree about what a string says, which is
// the same argument the pipeline note above makes about storing one form.
inline constexpr std::string_view kEgyptianCallingCode = "20";

// True when `digit` is an operator prefix anybody was assigned. The membership
// test the scanner applies on the way in, available to anything checking a value
// on the way back out.
[[nodiscard]] constexpr bool is_assigned_operator_digit(char digit) noexcept {
    for (const char assigned : kEgyptianOperatorDigits) {
        if (assigned == digit) { return true; }
    }
    return false;
}

// "+20" + 10 national digits. Not NUL-terminated: it is compared and appended,
// never handed to a C API.
struct PhoneEgy final {
    std::array<char, 13> e164;
    std::uint8_t         operator_digit;
};

static_assert(sizeof(PhoneEgy) == 14, "a phone is 14 bytes of automatic storage");

// The longest input worth scanning. Fourteen Arabic-Indic digits are 42 bytes,
// and separators are generous on top of that; anything past it is not a number
// somebody typed.
inline constexpr std::size_t kMaxPhoneInputBytes = 64;

// Fills `out` on success. Zero allocation, no exceptions, no locale.
//
//   Ok          `out` holds the E.164 form and the operator digit
//   NotAllowed  a recognisably Egyptian number that is not a mobile
//   BadFormat   anything else: a fragment, a foreign number, a stray character
//   TooLong     more bytes than any written phone number has
[[nodiscard]] Reason validate_phone_egy(std::string_view text, PhoneEgy& out) noexcept;

}  // namespace anvil::input
