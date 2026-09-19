#include <array>
#include <cstddef>
#include <string_view>

#include "anvil/input/fields.h"

// A linear email scanner. One pass, no backtracking, no allocation, no regex.
//
// `design idea.md` asks for "strict RFC 5322". A full RFC 5322 pattern in
// std::regex is a denial of service — the engine backtracks, so a crafted local
// part pins a core for seconds — and construction alone costs tens of
// microseconds before any input is seen.
//
// What is deliberately NOT accepted, though RFC 5322 permits it:
//
//   quoted local parts  "john doe"@example.com
//   comments            john(work)@example.com
//   address literals    john@[192.0.2.1]
//   non-ASCII bytes     anywhere
//
// No mail system in use needs any of them, each is a parser-differential source
// (this scanner and the receiving MTA disagreeing about who the address is),
// and rejecting non-ASCII is what makes a Cyrillic homoglyph domain fail:
// domains are IDNA-encoded before storage, so a byte above 0x7F proves the
// value has not been through that step (docs/05-auth-sessions.md §7).

namespace anvil::input {
namespace {

constexpr std::size_t kMaxEmailBytes = 254;   // RFC 5321 path limit
constexpr std::size_t kMaxLocalBytes = 64;
constexpr std::size_t kMaxLabelBytes = 63;

[[nodiscard]] constexpr bool is_ascii_letter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// The atext set of RFC 5322, minus the characters that are legal but have no
// legitimate use and complicate every downstream consumer.
[[nodiscard]] constexpr bool is_local_character(char c) noexcept {
    if (is_ascii_letter(c) || is_ascii_digit(c)) { return true; }
    constexpr std::string_view kAllowed = "!#$%&'*+-/=?^_`{|}~.";
    return kAllowed.find(c) != std::string_view::npos;
}

[[nodiscard]] constexpr bool is_label_character(char c) noexcept {
    return is_ascii_letter(c) || is_ascii_digit(c) || c == '-';
}

[[nodiscard]] Reason check_local_part(std::string_view local) noexcept {
    if (local.empty()) { return Reason::TooShort; }
    if (local.size() > kMaxLocalBytes) { return Reason::TooLong; }
    if (local.front() == '.' || local.back() == '.') { return Reason::BadFormat; }

    char previous = '\0';
    for (const char c : local) {
        if (!is_local_character(c)) { return Reason::BadCharset; }
        if (c == '.' && previous == '.') { return Reason::BadFormat; }
        previous = c;
    }
    return Reason::Ok;
}

[[nodiscard]] Reason check_domain(std::string_view domain) noexcept {
    if (domain.empty()) { return Reason::TooShort; }
    if (domain.size() > kMaxEmailBytes - 2) { return Reason::TooLong; }
    if (domain.front() == '.' || domain.back() == '.') { return Reason::BadFormat; }
    // An address literal is a valid RFC 5321 destination and a red flag in a
    // signup form; it also bypasses every domain-reputation control.
    if (domain.front() == '[') { return Reason::NotAllowed; }

    std::size_t label_length = 0;
    std::size_t label_count = 0;
    bool        last_label_is_alphabetic = false;

    for (std::size_t i = 0; i <= domain.size(); ++i) {
        const bool at_separator = i == domain.size() || domain[i] == '.';
        if (!at_separator) {
            const char c = domain[i];
            if (!is_label_character(c)) { return Reason::BadCharset; }
            // A leading or trailing hyphen is invalid, and `xn--` aside, a
            // label starting with a hyphen is a common spoofing shape.
            if (c == '-' && (label_length == 0 || i + 1 == domain.size() ||
                             domain[i + 1] == '.')) {
                return Reason::BadFormat;
            }
            if (label_length == 0) { last_label_is_alphabetic = is_ascii_letter(c); }
            ++label_length;
            continue;
        }

        if (label_length == 0) { return Reason::BadFormat; }        // empty or doubled dot
        if (label_length > kMaxLabelBytes) { return Reason::TooLong; }
        ++label_count;
        label_length = 0;
    }

    // A TLD must exist and must not be numeric: `user@192.0.2.1` is an IP
    // literal wearing a dotted-name costume, and it must fail like one.
    if (label_count < 2) { return Reason::BadFormat; }
    if (!last_label_is_alphabetic) { return Reason::NotAllowed; }
    return Reason::Ok;
}

}  // namespace

Reason check_email(std::string_view email) noexcept {
    if (email.empty()) { return Reason::Required; }
    if (email.size() > kMaxEmailBytes) { return Reason::TooLong; }

    std::size_t at_position = std::string_view::npos;
    for (std::size_t i = 0; i < email.size(); ++i) {
        const char c = email[i];
        // One pass covers the character-set rule too. Any byte above 0x7F is a
        // non-IDNA domain or an SMTPUTF8 local part; both are rejected here
        // rather than normalised, which is what stops the Cyrillic `а` in
        // `pаypal.com` from passing as Latin.
        if (static_cast<unsigned char>(c) > 0x7F) { return Reason::BadCharset; }
        if (c == '"' || c == '(' || c == ')' || c == '\\' || c == ',' || c == ';' || c == ' ') {
            return Reason::NotAllowed;   // quoted parts, comments, and separators
        }
        if (c != '@') { continue; }
        if (at_position != std::string_view::npos) { return Reason::BadFormat; }   // two '@'
        at_position = i;
    }
    if (at_position == std::string_view::npos) { return Reason::BadFormat; }

    if (const Reason reason = check_local_part(email.substr(0, at_position)); !is_ok(reason)) {
        return reason;
    }
    return check_domain(email.substr(at_position + 1));
}

}  // namespace anvil::input
