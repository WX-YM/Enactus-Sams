#include "anvil/locale_egy/phone_egy.h"

#include <cstddef>
#include <optional>

#include "anvil/i18n/digits.h"

namespace anvil::input {
namespace {

// 1 + operator + 8 subscriber digits. The `1` is what makes a mobile a mobile:
// every Egyptian landline area code starts with something else, which is the
// whole of the landline check below.
constexpr std::size_t kNationalDigits = 10;
constexpr std::size_t kMaxDigits = 16;

constexpr std::string_view kInternationalPrefix = "00";

[[nodiscard]] constexpr bool is_separator(char c) noexcept {
    return c == ' ' || c == '-' || c == '(' || c == ')';
}

[[nodiscard]] constexpr bool starts_with(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

}  // namespace

Reason validate_phone_egy(std::string_view text, PhoneEgy& out) noexcept {
    if (text.size() > kMaxPhoneInputBytes) { return Reason::TooLong; }

    // 1. Digit-fold into a stack buffer. Only when there is something to fold:
    //    the all-ASCII case is the common one and pays nothing.
    std::array<char, kMaxPhoneInputBytes> folded{};
    std::string_view ascii = text;
    if (i18n::has_non_ascii_digits(text)) {
        const std::optional<std::size_t> written = i18n::fold_digits_into(text, folded);
        if (!written.has_value()) { return Reason::TooLong; }
        ascii = std::string_view{folded.data(), *written};
    }

    // 2. Separators out, everything else asserted. A letter anywhere is a typo
    //    and dropping it would hand the customer somebody else's number.
    std::array<char, kMaxDigits> digits{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < ascii.size(); ++i) {
        const char c = ascii[i];
        if (is_separator(c)) { continue; }
        // The `+` is a country-code marker and marks nothing anywhere else.
        if (c == '+') {
            if (count != 0) { return Reason::BadFormat; }
            continue;
        }
        if (c < '0' || c > '9') { return Reason::BadFormat; }
        if (count == kMaxDigits) { return Reason::TooLong; }
        digits[count++] = c;
    }

    // 3. The dialling prefixes, in this order and only in this order: the
    //    international access code, then the country code, then the trunk code.
    //    An Egyptian subscriber number begins with a digit that no prefix rule
    //    can eat, so none of the three can over-strip.
    std::string_view national{digits.data(), count};
    bool egyptian = false;
    if (starts_with(national, kInternationalPrefix)) {
        national.remove_prefix(kInternationalPrefix.size());
    }
    if (starts_with(national, kEgyptianCallingCode)) {
        national.remove_prefix(kEgyptianCallingCode.size());
        egyptian = true;
    } else if (starts_with(national, "0")) {
        national.remove_prefix(1);
        egyptian = true;
    }

    // 4. The shape. A number that announced itself as Egyptian and is not a
    //    mobile is NotAllowed — a Cairo landline is nine national digits behind
    //    a `2`, so the leading digit decides this BEFORE the length does, or
    //    every landline would report as a fragment.
    if (national.empty()) { return Reason::BadFormat; }
    if (egyptian && national.front() != '1') { return Reason::NotAllowed; }
    if (national.size() != kNationalDigits || national.front() != '1') {
        // Not Egyptian and not mobile-shaped: nothing here was recognised, which
        // is a format failure rather than a statement about the number's kind.
        return Reason::BadFormat;
    }
    // Mobile-SHAPED but on a prefix nobody was assigned. "Check the number" is
    // the useful answer, which is what BadFormat renders as; NotAllowed would
    // tell the customer their perfectly ordinary number is the wrong kind.
    if (!is_assigned_operator_digit(national[1])) { return Reason::BadFormat; }

    out.e164[0] = '+';
    out.e164[1] = kEgyptianCallingCode[0];
    out.e164[2] = kEgyptianCallingCode[1];
    for (std::size_t i = 0; i < kNationalDigits; ++i) { out.e164[3 + i] = national[i]; }
    out.operator_digit = static_cast<std::uint8_t>(national[1] - '0');
    return Reason::Ok;
}

}  // namespace anvil::input
