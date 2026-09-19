#pragma once

// Egyptian National ID validation.
//
// `design idea.md` specifies "validates exact 14-digit numeric pattern". That
// accepts 00000000000000 and rejects almost nothing. The number
// is structured and self-checking:
//
//     C YY MM DD GG SSSS K
//     │ │  │  │  │  │    └─ 14    check digit
//     │ │  │  │  │  └────── 10-13 serial; the 13th digit is odd for male
//     │ │  │  │  └───────── 8-9   governorate code
//     │ └──┴──┴──────────── 2-7   birth date, YYMMDD
//     └──────────────────── 1     century: 2 = 1900-1999, 3 = 2000-2099
//
// Seven checks in order, over a std::array<char, 14> on the stack. Zero
// allocation, well inside the 200 ns budget in docs/06-input-validation.md §7.
//
// THE ID IS SENSITIVE PERSONAL DATA. It encodes date of birth, governorate and
// gender, which is why extraction is offered here but storage is not: it never
// sits in plaintext in a submissions collection (docs/13-dynamic-forms.md §6,
//).

#include <array>
#include <cstdint>
#include <string_view>

#include "anvil/input/fields.h"

namespace anvil::input {

enum class Gender : std::uint8_t { Female = 0, Male = 1 };

struct NationalIdEgy final {
    CalendarDate birth_date;
    std::uint8_t governorate;
    Gender       gender;
};

// Whether a failing check digit rejects the number.
//
// The other six checks are unambiguous. The checksum is not: the weighted-sum
// variant below is the one every public description gives, but no authoritative
// specification is published, and docs/06-input-validation.md §5.6 is explicit that an incorrect
// implementation rejecting valid citizens is far more damaging than accepting
// an invalid number.
//
// So the mode is a parameter with a fail-closed default, and Advisory exists so
// that a deployment seeing real rejections can drop to the six structural
// checks with one call-site change instead of an emergency patch. Confirm the
// variant against real sample data before launch, then delete Advisory.
enum class ChecksumMode : std::uint8_t { Enforce, Advisory };

// Validates and, on success, fills `out`. Arabic-Indic and Extended Arabic-Indic
// digits are folded to ASCII first: an Egyptian user typing ٢٩٨٠١٠١٢٣٤٥٦٧ on an
// Arabic keyboard is entering a valid number, and rejecting it is a defect
// wearing the clothes of a security control.
[[nodiscard]] Reason validate_national_id_egy(std::string_view text, NationalIdEgy& out,
                                              ChecksumMode mode = ChecksumMode::Enforce) noexcept;

// Exposed for the tests that generate valid numbers and for the one-off check
// against real sample data described above. `digits` is the 14 ASCII digits.
[[nodiscard]] char national_id_egy_check_digit(std::string_view first_thirteen) noexcept;

}  // namespace anvil::input
