#include "anvil/http/origin_check.h"

#include <cstddef>
#include <string_view>

namespace anvil::http {
namespace {

// "null" is what a browser sends from a sandboxed iframe, a data: document or a
// redirected cross-origin form post. It is never one of ours.
inline constexpr std::string_view kNullOrigin = "null";

// The rule, written once. Both public overloads differ only in how they look an
// origin up, and a second copy of the three steps below is a second place for
// the absent-is-a-rejection rule to be got wrong.
template <typename Contains>
[[nodiscard]] OriginVerdict decide(std::string_view method, std::string_view origin_header,
                                   OriginRequirement requirement,
                                   const Contains& contains) noexcept {
    if (requirement == OriginRequirement::ByMethod && !is_state_changing(method)) {
        return OriginVerdict::NotRequired;
    }

    // Treated as absent rather than as a value, which keeps it out of the
    // compare below where an "allowed" entry of "null" could ever match it.
    // `AllowedOrigins::parse` refuses such an entry as well, so the property
    // holds on both sides rather than resting on this line alone.
    if (origin_header.empty() || origin_header == kNullOrigin) { return OriginVerdict::Missing; }

    return contains(origin_header) ? OriginVerdict::Allowed : OriginVerdict::Mismatched;
}

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) { return {}; }
    const std::size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// Scheme, host and optional port. No path, no query, no fragment, no trailing
// slash — which is both what a browser puts in the header and what a
// byte-for-byte compare needs, so an entry in any other shape can never match
// and is a configuration error rather than a harmless extra.
[[nodiscard]] bool well_formed_origin(std::string_view entry) noexcept {
    if (entry.empty() || entry == kNullOrigin) { return false; }

    const std::size_t scheme_end = entry.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) { return false; }

    const std::string_view authority = entry.substr(scheme_end + 3);
    if (authority.empty()) { return false; }
    // A slash anywhere past the scheme is a path, including the trailing one an
    // operator copies out of a browser's address bar. That entry would never
    // match a header and the list would look configured while refusing
    // everything.
    if (authority.find('/') != std::string_view::npos) { return false; }
    if (authority.find_first_of("?# \t") != std::string_view::npos) { return false; }
    return true;
}

}  // namespace

OriginVerdict check_origin(std::string_view method, std::string_view origin_header,
                           std::span<const std::string_view> allowed,
                           OriginRequirement requirement) noexcept {
    return decide(method, origin_header, requirement, [allowed](std::string_view origin) {
        for (const std::string_view candidate : allowed) {
            // Exact, byte-for-byte. Never a suffix test: `evil-example.com` and
            // `example.com.attacker.test` both satisfy "ends with example.com".
            if (origin == candidate) { return true; }
        }
        return false;
    });
}

OriginVerdict check_origin(std::string_view method, std::string_view origin_header,
                           const AllowedOrigins& allowed,
                           OriginRequirement requirement) noexcept {
    return decide(method, origin_header, requirement,
                  [&allowed](std::string_view origin) { return allowed.contains(origin); });
}

bool AllowedOrigins::parse(std::string_view list) noexcept {
    count_ = 0;
    std::string_view rest = list;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view entry =
            trimmed(comma == std::string_view::npos ? rest : rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);

        // An empty entry is a stray comma, which is a typo rather than an
        // instruction. Refused rather than skipped: the list a deployment meant
        // to write is not the list it wrote.
        if (!well_formed_origin(entry)) {
            count_ = 0;
            return false;
        }
        if (count_ >= kMaxEntries) {
            count_ = 0;
            return false;
        }
        // assign rather than construct, so a second parse on the same object
        // reuses the capacity the first one allocated.
        entries_[count_].assign(entry);
        ++count_;
    }
    return count_ != 0;
}

bool AllowedOrigins::contains(std::string_view origin) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        // Exact, byte-for-byte, for the reason above the other overload.
        if (entries_[i] == origin) { return true; }
    }
    return false;
}

}  // namespace anvil::http
