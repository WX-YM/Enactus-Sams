#include "anvil/accounts/identifier.h"

#include "anvil/i18n/bidi.h"
#include "anvil/i18n/normalize.h"

namespace anvil::accounts {
namespace {

constexpr std::size_t kPhoneMinDigits = 8;
constexpr std::size_t kPhoneMaxDigits = 15;

// Bounds the input BEFORE normalisation. NFKC can expand a string several-fold,
// and nothing that reaches here is a legitimate identifier past this length:
// an email is at most 254 bytes and the other two are shorter.
constexpr std::size_t kMaxRawBytes = 320;

[[nodiscard]] input::Reason folded(std::string_view raw, std::string& out) {
    std::optional<std::string> result = i18n::normalize(raw, i18n::NormalizeMode::NfkcCaseFold);
    // ICU failing is a configuration or memory fault, never the person's input,
    // and no normalisation must never mean the raw bytes: a key stored raw
    // cannot be found again by a later lookup that did normalise.
    if (!result.has_value()) { return input::Reason::BadFormat; }
    out = std::move(*result);
    return input::Reason::Ok;
}

[[nodiscard]] input::Reason canonical_phone(std::string_view raw, std::string& out) {
    std::string digits;
    digits.reserve(kPhoneMaxDigits + 1);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (i == 0 && c == '+') {
            digits.push_back(c);
            continue;
        }
        if (c == ' ' || c == '-' || c == '.' || c == '(' || c == ')') { continue; }
        if (c < '0' || c > '9') { return input::Reason::BadFormat; }
        digits.push_back(c);
        if (digits.size() > kPhoneMaxDigits + 1) { return input::Reason::TooLong; }
    }
    if (digits.empty() || digits[0] != '+') { return input::Reason::BadFormat; }
    const std::size_t count = digits.size() - 1;
    if (count < kPhoneMinDigits) { return input::Reason::TooShort; }
    // A country code never begins with zero; a number that does was typed in
    // its national form and would be stored as a different subscriber.
    if (digits[1] == '0') { return input::Reason::BadFormat; }
    out = std::move(digits);
    return input::Reason::Ok;
}

}  // namespace

LoginIdentity kind_by_shape(std::string_view raw) noexcept {
    if (raw.find('@') != std::string_view::npos) { return LoginIdentity::Email; }
    // Leading whitespace is not stripped anywhere else, so it is not here
    // either: " +20…" is a username that canonicalise will refuse, which is the
    // correct answer for something nobody meant as a phone number.
    if (!raw.empty() && raw.front() == '+') { return LoginIdentity::Phone; }
    return LoginIdentity::Username;
}

input::Reason canonicalise(LoginIdentity kind, std::string_view raw, std::string& out) {
    if (raw.empty()) { return input::Reason::Required; }
    if (raw.size() > kMaxRawBytes) { return input::Reason::TooLong; }
    // The RAW input, before folding. NFKC case-folding deletes default-ignorable
    // code points — a bidi override, a zero-width joiner — so the lookup key of
    // "ab\u202Ecd" is a harmless "abcd"; but the DISPLAY spelling is stored as
    // typed, and a stored override is a name that renders as somebody else's in
    // every list it appears in. Refused here, where the typed form is still
    // visible.
    if (!i18n::is_acceptable(raw, i18n::TextClass::Identifier)) {
        return input::Reason::BadCharset;
    }

    switch (kind) {
        case LoginIdentity::Phone:
            return canonical_phone(raw, out);

        case LoginIdentity::Email: {
            std::string value;
            const input::Reason reason = folded(raw, value);
            if (reason != input::Reason::Ok) { return reason; }
            const input::Reason shape = input::check_email(value);
            if (shape != input::Reason::Ok) { return shape; }
            out = std::move(value);
            return input::Reason::Ok;
        }

        case LoginIdentity::Username: {
            std::string value;
            const input::Reason reason = folded(raw, value);
            if (reason != input::Reason::Ok) { return reason; }
            if (value.find('@') != std::string::npos || value.front() == '+') {
                return input::Reason::BadCharset;
            }
            const input::Reason text = input::check_text(
                value, input::TextRules{kUsernameMinCodePoints, kUsernameMaxCodePoints,
                                        i18n::TextClass::Identifier, false});
            if (text != input::Reason::Ok) { return text; }
            out = std::move(value);
            return input::Reason::Ok;
        }
    }
    return input::Reason::BadFormat;
}

}  // namespace anvil::accounts
