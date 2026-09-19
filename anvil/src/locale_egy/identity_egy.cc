#include "anvil/locale_egy/identity_egy.h"

#include <chrono>

#include "anvil/i18n/digits.h"

namespace anvil::input {
namespace {

constexpr std::size_t kIdLength = 14;

// Valid governorate codes: 01-35 plus 88 for "born abroad". The set is NOT
// contiguous — 36 through 87 are unassigned — so a range check would accept
// dozens of impossible values (docs/06-input-validation.md §5.6 step 5).
constexpr std::array<std::uint8_t, 28> kGovernorates{
    1,  2,  3,  4,  11, 12, 13, 14, 15, 16, 17, 18, 19, 21,
    22, 23, 24, 25, 26, 27, 28, 29, 31, 32, 33, 34, 35, 88};

// Weights for the check digit, applied to the first thirteen digits.
constexpr std::array<std::uint8_t, 13> kWeights{2, 7, 6, 5, 4, 3, 2, 7, 6, 5, 4, 3, 2};

[[nodiscard]] constexpr bool is_valid_governorate(std::uint8_t code) noexcept {
    for (const std::uint8_t valid : kGovernorates) {
        if (valid == code) { return true; }
    }
    return false;
}

[[nodiscard]] constexpr int digit_of(char c) noexcept { return c - '0'; }

// Today, in UTC, as a civil date. The birth-date-in-the-future check needs a
// day boundary and nothing finer, so a timezone would add a dependency without
// adding accuracy — and never localtime, which is the server's opinion rather
// than a fact (docs/06-input-validation.md §5.4).
[[nodiscard]] std::int64_t today_days() noexcept {
    const auto now = std::chrono::system_clock::now();
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return seconds / 86400;
}

}  // namespace

char national_id_egy_check_digit(std::string_view first_thirteen) noexcept {
    if (first_thirteen.size() < kWeights.size()) { return '\0'; }

    int sum = 0;
    for (std::size_t i = 0; i < kWeights.size(); ++i) {
        sum += digit_of(first_thirteen[i]) * kWeights[i];
    }
    const int remainder = sum % 11;
    const int check = 11 - remainder;
    // The documented handling of the two out-of-range results: 11 wraps to 0
    // and 10 wraps to 1, which is what keeps the check digit a single digit.
    const int normalised = check == 11 ? 0 : (check == 10 ? 1 : check);
    return static_cast<char>('0' + normalised);
}

Reason validate_national_id_egy(std::string_view text, NationalIdEgy& out,
                                ChecksumMode mode) noexcept {
    // 1. Digit-fold, into a stack buffer. Arabic-Indic digits are three bytes
    //    each in UTF-8, so the source can be up to 42 bytes for 14 digits.
    std::array<char, kIdLength> digits{};
    std::string_view ascii = text;
    if (i18n::has_non_ascii_digits(text)) {
        const std::optional<std::size_t> written = i18n::fold_digits_into(text, digits);
        if (!written.has_value()) { return Reason::TooLong; }
        ascii = std::string_view{digits.data(), *written};
    }

    // 2. Exactly 14 ASCII digits.
    if (ascii.size() != kIdLength) {
        return ascii.size() < kIdLength ? Reason::TooShort : Reason::TooLong;
    }
    for (const char c : ascii) {
        if (c < '0' || c > '9') { return Reason::BadCharset; }
    }

    // 3. Century digit. 4 joins the set in 2100; keeping it a table rather than
    //    a comparison is what makes that a one-line change.
    const int century = digit_of(ascii[0]);
    if (century != 2 && century != 3) { return Reason::BadFormat; }
    const std::int32_t year_base = century == 2 ? 1900 : 2000;

    // 4. A real calendar date, not a digit pattern. 250230 and 250229 both look
    //    like dates and neither is one.
    const CalendarDate birth{
        year_base + digit_of(ascii[1]) * 10 + digit_of(ascii[2]),
        static_cast<std::uint8_t>(digit_of(ascii[3]) * 10 + digit_of(ascii[4])),
        static_cast<std::uint8_t>(digit_of(ascii[5]) * 10 + digit_of(ascii[6])),
    };
    const std::uint8_t last_day = days_in_month(birth.year, birth.month);
    if (last_day == 0 || birth.day < 1 || birth.day > last_day) { return Reason::OutOfRange; }
    if (days_from_civil(birth) > today_days()) { return Reason::OutOfRange; }

    // 5. Governorate, against the table.
    const auto governorate =
        static_cast<std::uint8_t>(digit_of(ascii[7]) * 10 + digit_of(ascii[8]));
    if (!is_valid_governorate(governorate)) { return Reason::NotAllowed; }

    // 6. Serial 0000 is not issued, and it is what an all-zero number and most
    //    hand-typed placeholders reduce to.
    const int serial = digit_of(ascii[9]) * 1000 + digit_of(ascii[10]) * 100 +
                       digit_of(ascii[11]) * 10 + digit_of(ascii[12]);
    if (serial == 0) { return Reason::BadFormat; }

    // 7. Check digit.
    if (mode == ChecksumMode::Enforce &&
        national_id_egy_check_digit(ascii) != ascii[kIdLength - 1]) {
        return Reason::BadChecksum;
    }

    out = NationalIdEgy{
        birth,
        governorate,
        (digit_of(ascii[12]) % 2 == 1) ? Gender::Male : Gender::Female,
    };
    return Reason::Ok;
}

}  // namespace anvil::input
