#pragma once

// Field validators: pure functions over string_view, no allocation, no
// exceptions, no std::regex (docs/06-input-validation.md §3, §4).
//
// std::regex is banned on every request path and the ban is enforced by
// tools/check-source-bans.sh, not by memory. It backtracks, so an RFC 5322
// pattern against crafted input pins a core for seconds; construction alone
// costs tens of microseconds; and its behaviour on non-ASCII bytes follows the
// locale, which is unacceptable for a UTF-8 Arabic system.
//
// Every validator here returns a Reason. Reasons are enum values, never
// messages and never the submitted value: the client already holds the
// bilingual text, and echoing input back is a reflected-XSS, log-injection and
// encoding hazard all at once.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "anvil/core/types.h"
#include "anvil/i18n/bidi.h"

namespace anvil::input {

enum class Reason : std::uint16_t {
    Ok = 0,
    Required,
    TooShort,
    TooLong,
    BadFormat,
    BadCharset,
    OutOfRange,
    NotAllowed,
    BadChecksum,
    Weak,
    Breached,
};

// The field name is a compile-time constant from the schema, never a key taken
// from the request: an unknown key is reported with an empty name precisely so
// that a client cannot choose what appears in a response or a log line.
struct FieldError final {
    std::string_view field;
    Reason           reason;
};

[[nodiscard]] constexpr bool is_ok(Reason reason) noexcept { return reason == Reason::Ok; }

// --- text -----------------------------------------------------------------

// Bounds are in CODE POINTS, never bytes. Arabic is two bytes per character in
// UTF-8, so a byte limit silently gives Arabic users half the allowance and a
// CJK script a third of it (ENGINEERING_RULES.md §8).
struct TextRules final {
    std::size_t     min_code_points;
    std::size_t     max_code_points;
    i18n::TextClass text_class;
    // Newline and tab are legitimate in prose and meaningless in an identifier.
    bool            allow_line_breaks;
};

inline constexpr TextRules kIdentifierRules{1, 64, i18n::TextClass::Identifier, false};
// A person's OWN name, in whatever script they wrote it in — one string, never
// a bilingual pair. `display_name` is the other thing entirely: staff-only,
// bilingual, and written at promotion.
//
// Prose rather than Identifier, because a name has spaces in it and is not
// compared against anything; 80 code points is generous in every script, and it
// is code points rather than bytes so an Arabic name gets all of it instead of
// half.
inline constexpr std::size_t kPersonNameMaxCodePoints = 80;
inline constexpr TextRules kPersonNameRules{1, kPersonNameMaxCodePoints,
                                            i18n::TextClass::Prose, false};
inline constexpr TextRules kShortProseRules{1, 200, i18n::TextClass::Prose, false};
inline constexpr TextRules kLongProseRules{1, 20000, i18n::TextClass::Prose, true};

// Assumes valid UTF-8 — the body was validated before the parse (json.h).
[[nodiscard]] Reason check_text(std::string_view text, const TextRules& rules) noexcept;

// --- password -------------------------------------------------------------

// 12-128 code points, NFC only, never trimmed, no composition rules — length
// plus a breach check, per NIST 800-63B. Composition rules measurably reduce
// entropy by pushing users to `Password1!` (docs/05-auth-sessions.md §3).
//
// The maximum is enforced HERE, before the value reaches Argon2: an unbounded
// password is a memory denial of service against hash_pool.
inline constexpr std::size_t kPasswordMinCodePoints = 12;
inline constexpr std::size_t kPasswordMaxCodePoints = 128;

[[nodiscard]] Reason check_password(std::string_view password) noexcept;

// True when the password is in the compiled-in breach set. No network call: an
// external API on the login path is an availability dependency and a plaintext
// disclosure at the same time.
[[nodiscard]] bool is_breached_password(std::string_view password) noexcept;

// --- email ----------------------------------------------------------------

// Linear scanner, per docs/05-auth-sessions.md §7. <= 254 bytes, exactly one
// unquoted '@', local part <= 64, label structure, no leading, trailing or
// doubled dots, a TLD present. Quoted local parts and comments are rejected:
// RFC 5322 permits them, no real mail system needs them, and they are a
// parser-differential source.
//
// A non-ASCII domain is rejected rather than folded, which is also what makes
// the Cyrillic-homoglyph domain in `pаypal.com` fail: domains are IDNA-encoded
// before they reach storage, so anything with a byte above 0x7F has not been.
[[nodiscard]] Reason check_email(std::string_view email) noexcept;

// --- numbers --------------------------------------------------------------

// Arabic-Indic digits are folded to ASCII first, into a stack buffer, so an
// Egyptian user typing ١٢٣ is not rejected by a defect dressed as a validation
// rule. Parsing is std::from_chars: no allocation, no locale, and overflow is
// REPORTED rather than wrapped or saturated.
[[nodiscard]] Reason parse_int(std::string_view text, std::int64_t min, std::int64_t max,
                               std::int64_t& out) noexcept;

// --- date and time --------------------------------------------------------

// ISO-8601 with an EXPLICIT offset, or a bare Z. A timestamp with no offset is
// ambiguous, and every scheduling feature in this system depends on it not
// being: "18:00" means nothing without a zone.
//
// The calendar date is validated, not just the digit pattern: 2026-02-30 and
// 2025-02-29 both match a pattern and neither is a date.
struct CalendarDate final {
    std::int32_t year;
    std::uint8_t month;
    std::uint8_t day;
};

[[nodiscard]] constexpr bool is_leap_year(std::int32_t year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] constexpr std::uint8_t days_in_month(std::int32_t year, std::uint8_t month) noexcept {
    constexpr std::array<std::uint8_t, 13> kDays{0, 31, 28, 31, 30, 31, 30,
                                                 31, 31, 30, 31, 30, 31};
    if (month == 0 || month > 12) { return 0; }
    if (month == 2 && is_leap_year(year)) { return 29; }
    return kDays[month];
}

// Days since 1970-01-01 for a proleptic Gregorian date. The civil-from-days
// algorithm, so no libc call, no timezone database, and no locale.
[[nodiscard]] constexpr std::int64_t days_from_civil(CalendarDate date) noexcept {
    std::int64_t year = date.year;
    year -= date.month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t year_of_era = year - era * 400;
    const std::int64_t day_of_year =
        (153 * (date.month + (date.month > 2 ? -3 : 9)) + 2) / 5 + date.day - 1;
    const std::int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

// The exact inverse of days_from_civil, and the same era arithmetic run
// backwards. It exists so a stored UTC instant can be rendered as a calendar
// date without localtime/gmtime — both are banned on a request path, one for
// the timezone database it consults and one for the static buffer it returns
// (tools/check-source-bans.sh).
[[nodiscard]] constexpr CalendarDate civil_from_days(std::int64_t days) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t year = year_of_era + era * 400;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t shifted_month = (5 * day_of_year + 2) / 153;
    const std::int64_t day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    const std::int64_t month = shifted_month + (shifted_month < 10 ? 3 : -9);
    return CalendarDate{static_cast<std::int32_t>(year + (month <= 2 ? 1 : 0)),
                        static_cast<std::uint8_t>(month), static_cast<std::uint8_t>(day)};
}

[[nodiscard]] Reason parse_date(std::string_view text, CalendarDate& out) noexcept;

// Milliseconds since the Unix epoch, UTC. The offset is applied and discarded:
// storage is always UTC, and conversion happens at the presentation layer only.
[[nodiscard]] Reason parse_timestamp(std::string_view text, std::int64_t& out_ms) noexcept;

// --- URL ------------------------------------------------------------------

enum class UrlUse : std::uint8_t {
    // A link the browser will follow. Scheme allow-list only.
    Link,
    // A URL this server will fetch itself — a webhook target. Everything Link
    // rejects, plus literal private and link-local addresses. The post-DNS and
    // connect-time re-checks that defeat rebinding live with the HTTP client,
    // because only it knows the address actually connected to.
    ServerFetch,
};

// Rejects javascript:, data:, vbscript: and every scheme not on the list;
// rejects credentials in the authority; rejects IP literals and non-ASCII
// hosts. Site-relative URLs ("/about") are accepted for Link.
[[nodiscard]] Reason check_url(std::string_view url, UrlUse use) noexcept;

// --- identifiers ----------------------------------------------------------

// Canonical hyphenated or base64url, decoded to 16 bytes on the stack. The
// string form NEVER reaches a query: the stored form is BinData(4)
// (docs/09-mongodb.md §2).
[[nodiscard]] Reason parse_uuid(std::string_view text, Uuid& out) noexcept;

// --- enums ----------------------------------------------------------------

template <typename E>
struct EnumEntry final {
    std::string_view name;
    E                value;
};

// Linear over a constexpr table in .rodata. An unmatched value is NotAllowed,
// and the string is never stored or compared again after this point.
template <typename E, std::size_t N>
[[nodiscard]] constexpr Reason parse_enum(std::string_view text,
                                          const std::array<EnumEntry<E>, N>& table,
                                          E& out) noexcept {
    for (const EnumEntry<E>& entry : table) {
        if (entry.name == text) {
            out = entry.value;
            return Reason::Ok;
        }
    }
    return Reason::NotAllowed;
}

}  // namespace anvil::input
