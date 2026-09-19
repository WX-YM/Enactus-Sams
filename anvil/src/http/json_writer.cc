#include "anvil/http/json_writer.h"

#include <array>
#include <charconv>
#include <cstddef>

namespace anvil::http {
namespace {

constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

void append_u4(std::string& out, std::uint8_t byte) {
    out.append("\\u00");
    out.push_back(kHex[(byte >> 4U) & 0x0FU]);
    out.push_back(kHex[byte & 0x0FU]);
}

// Zero-padded, fixed width. std::to_string plus manual padding would allocate
// per component; this writes into the caller's buffer directly.
void append_padded(std::string& out, std::int64_t value, std::size_t width) {
    std::array<char, 8> digits{};
    std::size_t written = 0;
    for (std::size_t i = 0; i < width; ++i) {
        digits[width - 1 - i] = static_cast<char>('0' + (value % 10));
        value /= 10;
        ++written;
    }
    out.append(digits.data(), written);
}

// Civil date from days since the Unix epoch — the same algorithm as
// input::days_from_civil, inverted. No libc, no timezone database, no locale:
// localtime and gmtime are both banned on a request path, one for the global
// state and one for the allocation-free promise this function makes.
struct CivilDate final {
    std::int64_t year;
    std::int64_t month;
    std::int64_t day;
};

[[nodiscard]] CivilDate civil_from_days(std::int64_t days) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t year = year_of_era + era * 400;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t mp = (5 * day_of_year + 2) / 153;
    const std::int64_t day = day_of_year - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp + (mp < 10 ? 3 : -9);
    return CivilDate{year + (month <= 2 ? 1 : 0), month, day};
}

}  // namespace

void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        const auto byte = static_cast<std::uint8_t>(c);
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            default:
                if (byte < 0x20U) {
                    // The only remaining mandatory escapes. Everything at or
                    // above 0x20 — including every continuation byte of an
                    // Arabic character — is copied through untouched.
                    append_u4(out, byte);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

void append_json_key(std::string& out, std::string_view key) {
    append_json_string(out, key);
    out.push_back(':');
}

void append_json_int(std::string& out, std::int64_t value) {
    std::array<char, 24> buffer{};
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec == std::errc{}) {
        out.append(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
    } else {
        out.push_back('0');
    }
}

void append_json_time(std::string& out, std::int64_t epoch_ms) {
    // Floor division, so instants before 1970 do not round towards zero and
    // land a day late. The system stores nothing before 1970, but a corrupted
    // negative date must produce a wrong date rather than a malformed one.
    std::int64_t seconds = epoch_ms / 1000;
    std::int64_t millis = epoch_ms % 1000;
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }
    std::int64_t days = seconds / 86400;
    std::int64_t time_of_day = seconds % 86400;
    if (time_of_day < 0) {
        time_of_day += 86400;
        --days;
    }

    const CivilDate date = civil_from_days(days);
    out.push_back('"');
    append_padded(out, date.year, 4);
    out.push_back('-');
    append_padded(out, date.month, 2);
    out.push_back('-');
    append_padded(out, date.day, 2);
    out.push_back('T');
    append_padded(out, time_of_day / 3600, 2);
    out.push_back(':');
    append_padded(out, (time_of_day / 60) % 60, 2);
    out.push_back(':');
    append_padded(out, time_of_day % 60, 2);
    out.push_back('.');
    append_padded(out, millis, 3);
    out.append("Z\"");
}

void append_json_uuid(std::string& out, std::span<const std::uint8_t, 16> id) {
    out.push_back('"');
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) { out.push_back('-'); }
        out.push_back(kHex[(id[i] >> 4U) & 0x0FU]);
        out.push_back(kHex[id[i] & 0x0FU]);
    }
    out.push_back('"');
}

}  // namespace anvil::http
