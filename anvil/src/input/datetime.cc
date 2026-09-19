#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "anvil/input/fields.h"

// ISO-8601 parsing by hand, for two reasons.
//
// C++20's std::chrono::parse allocates and goes through a stream and a locale;
// strptime is locale-dependent in a way that changes meaning between machines.
// Neither belongs on a request path (docs/06-input-validation.md §5.4).
//
// More importantly, an offset is REQUIRED. A timestamp with no zone is
// ambiguous, and every scheduling feature in this system rests on it not being:
// a note scheduled for "18:00" means nothing until someone decides whose 18:00
// it was, and that decision silently becomes "the server's", which changes when
// the server moves. `Z` counts as an explicit offset; a bare
// `2026-08-04T18:00:00` does not.

namespace anvil::input {
namespace {

constexpr std::int32_t kMinYear = 1970;
constexpr std::int32_t kMaxYear = 2100;

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Fixed-width unsigned field. Returns false rather than consuming a shorter
// run: `2026-8-4` is not an ISO-8601 date, and accepting it means two systems
// disagree about whether `26-08-04` is a year or a day.
[[nodiscard]] bool read_fixed(std::string_view text, std::size_t offset, std::size_t width,
                              std::int32_t& out) noexcept {
    if (offset + width > text.size()) { return false; }
    std::int32_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        const char c = text[offset + i];
        if (!is_digit(c)) { return false; }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

}  // namespace

Reason parse_date(std::string_view text, CalendarDate& out) noexcept {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') { return Reason::BadFormat; }

    std::int32_t year = 0;
    std::int32_t month = 0;
    std::int32_t day = 0;
    if (!read_fixed(text, 0, 4, year) || !read_fixed(text, 5, 2, month) ||
        !read_fixed(text, 8, 2, day)) {
        return Reason::BadFormat;
    }
    if (year < kMinYear || year > kMaxYear) { return Reason::OutOfRange; }

    // The calendar, not the pattern. 2026-02-30 and 2025-02-29 both match every
    // digit rule and neither is a day that exists.
    const auto month_value = static_cast<std::uint8_t>(month);
    const std::uint8_t last_day = days_in_month(year, month_value);
    if (last_day == 0 || day < 1 || day > last_day) { return Reason::OutOfRange; }

    out = CalendarDate{year, month_value, static_cast<std::uint8_t>(day)};
    return Reason::Ok;
}

Reason parse_timestamp(std::string_view text, std::int64_t& out_ms) noexcept {
    // The shortest accepted form is 1970-01-01T00:00:00Z.
    if (text.size() < 20) { return Reason::BadFormat; }
    if (text[10] != 'T' && text[10] != 't') { return Reason::BadFormat; }

    CalendarDate date{};
    if (const Reason reason = parse_date(text.substr(0, 10), date); !is_ok(reason)) {
        return reason;
    }

    std::int32_t hour = 0;
    std::int32_t minute = 0;
    std::int32_t second = 0;
    if (text[13] != ':' || text[16] != ':') { return Reason::BadFormat; }
    if (!read_fixed(text, 11, 2, hour) || !read_fixed(text, 14, 2, minute) ||
        !read_fixed(text, 17, 2, second)) {
        return Reason::BadFormat;
    }
    // 24:00 and leap seconds are both representable in ISO-8601 and neither
    // survives a round trip through a database or a comparison, so both fail.
    if (hour > 23 || minute > 59 || second > 59) { return Reason::OutOfRange; }

    std::size_t position = 19;
    std::int32_t millisecond = 0;
    if (position < text.size() && text[position] == '.') {
        ++position;
        std::size_t digits = 0;
        while (position < text.size() && is_digit(text[position])) {
            // Milliseconds are the resolution BSON stores; anything finer is
            // truncated here rather than pretending to survive (anvil/db/codec.h).
            if (digits < 3) { millisecond = millisecond * 10 + (text[position] - '0'); }
            ++digits;
            ++position;
        }
        if (digits == 0) { return Reason::BadFormat; }
        for (std::size_t i = digits; i < 3; ++i) { millisecond *= 10; }
    }

    if (position >= text.size()) { return Reason::BadFormat; }   // no offset at all

    std::int32_t offset_minutes = 0;
    const char zone = text[position];
    if (zone == 'Z' || zone == 'z') {
        if (position + 1 != text.size()) { return Reason::BadFormat; }
    } else if (zone == '+' || zone == '-') {
        // ±HH:MM only. ±HHMM and ±HH are legal ISO-8601 and are two more ways
        // for two parsers to disagree.
        if (position + 6 != text.size() || text[position + 3] != ':') {
            return Reason::BadFormat;
        }
        std::int32_t offset_hour = 0;
        std::int32_t offset_minute = 0;
        if (!read_fixed(text, position + 1, 2, offset_hour) ||
            !read_fixed(text, position + 4, 2, offset_minute)) {
            return Reason::BadFormat;
        }
        if (offset_hour > 14 || offset_minute > 59) { return Reason::OutOfRange; }
        offset_minutes = offset_hour * 60 + offset_minute;
        if (zone == '-') { offset_minutes = -offset_minutes; }
    } else {
        return Reason::BadFormat;
    }

    const std::int64_t days = days_from_civil(date);
    const std::int64_t seconds_of_day = hour * 3600LL + minute * 60LL + second;
    // The offset is applied and then discarded: what is stored is UTC, always.
    // Conversion back to a local rendering belongs to the presentation layer.
    const std::int64_t utc_seconds = days * 86400LL + seconds_of_day - offset_minutes * 60LL;

    out_ms = utc_seconds * 1000LL + millisecond;
    return Reason::Ok;
}

}  // namespace anvil::input
