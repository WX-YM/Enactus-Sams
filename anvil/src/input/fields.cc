#include "anvil/input/fields.h"

#include <array>
#include <charconv>
#include <cstring>

#include "anvil/core/uuid.h"
#include "anvil/i18n/digits.h"
#include "anvil/i18n/utf8.h"
#include "anvil/input/breach_filter.h"

namespace anvil::input {
namespace {

// C0 controls other than the two that legitimately appear in prose. DEL is
// included: it is invisible, it survives every "printable" filter, and nothing
// types it on purpose.
[[nodiscard]] bool has_forbidden_control(std::string_view text, bool allow_line_breaks) noexcept {
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte == 0x7F) { return true; }
        if (byte >= 0x20) { continue; }
        if (allow_line_breaks && (byte == '\n' || byte == '\t' || byte == '\r')) { continue; }
        return true;
    }
    return false;
}

}  // namespace

// --- text -----------------------------------------------------------------

Reason check_text(std::string_view text, const TextRules& rules) noexcept {
    if (text.empty() && rules.min_code_points > 0) { return Reason::Required; }
    if (has_forbidden_control(text, rules.allow_line_breaks)) { return Reason::BadCharset; }

    // Short-circuits as soon as the maximum is exceeded, so a hostile 10 MB
    // value does not pay for a full count before being rejected.
    if (!i18n::within_code_point_bounds(text, rules.min_code_points, rules.max_code_points)) {
        return i18n::count_code_points(text) < rules.min_code_points ? Reason::TooShort
                                                                     : Reason::TooLong;
    }

    // U+202E reverses everything after it, so `exe.<U+202E>gnp.evil` renders in
    // a dashboard as `evil.png` while remaining an executable. Isolates are
    // legitimate in prose and never in an identifier.
    if (i18n::check(text, rules.text_class) != i18n::BidiIssue::Ok) { return Reason::BadCharset; }

    return Reason::Ok;
}

// --- password -------------------------------------------------------------

bool is_breached_password(std::string_view password) noexcept {
    return breach::probably_contains(password);
}

Reason check_password(std::string_view password) noexcept {
    // Length in code points, and checked BEFORE anything expensive: an
    // unbounded password reaching Argon2 is a memory denial of service against
    // hash_pool, not a validation nicety.
    if (!i18n::within_code_point_bounds(password, kPasswordMinCodePoints,
                                        kPasswordMaxCodePoints)) {
        return i18n::count_code_points(password) < kPasswordMinCodePoints ? Reason::TooShort
                                                                          : Reason::TooLong;
    }
    // Deliberately absent: trimming, case folding, and every composition rule.
    // Trimming changes a password the user chose; composition rules measurably
    // reduce entropy by pushing users to `Password1!` (docs/05-auth-sessions.md §3).
    if (is_breached_password(password)) { return Reason::Breached; }
    return Reason::Ok;
}

// --- numbers --------------------------------------------------------------

Reason parse_int(std::string_view text, std::int64_t min, std::int64_t max,
                 std::int64_t& out) noexcept {
    // Longest int64 is 20 characters with the sign. Anything longer cannot be
    // in range, so the bound is a rejection rather than a truncation.
    constexpr std::size_t kMaxDigits = 20;
    std::array<char, kMaxDigits> folded{};

    std::string_view digits = text;
    if (i18n::has_non_ascii_digits(text)) {
        const std::optional<std::size_t> written = i18n::fold_digits_into(text, folded);
        if (!written.has_value()) { return Reason::TooLong; }
        digits = std::string_view{folded.data(), *written};
    }
    if (digits.empty() || digits.size() > kMaxDigits) { return Reason::BadFormat; }

    std::int64_t value = 0;
    const char* const first = digits.data();
    const char* const last = first + digits.size();
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec == std::errc::result_out_of_range) { return Reason::OutOfRange; }
    // Partial consumption means trailing junk ("12abc"), which is a format
    // error and never a silently truncated 12.
    if (result.ec != std::errc{} || result.ptr != last) { return Reason::BadFormat; }
    if (value < min || value > max) { return Reason::OutOfRange; }

    out = value;
    return Reason::Ok;
}

// --- identifiers ----------------------------------------------------------

Reason parse_uuid(std::string_view text, Uuid& out) noexcept {
    if (const std::optional<Uuid> canonical = uuid::parse(text); canonical.has_value()) {
        out = *canonical;
        return Reason::Ok;
    }
    if (const std::optional<Uuid> compact = uuid::from_base64url(text); compact.has_value()) {
        out = *compact;
        return Reason::Ok;
    }
    return Reason::BadFormat;
}

}  // namespace anvil::input
