#pragma once

// The library's own version, so a consuming application can log what it linked.
//
// A compiled function rather than a macro: a header constant records the version
// the application was COMPILED against, which is the one number that cannot
// disagree with reality. This one records the version that was LINKED, and when
// the two differ that is exactly the bug worth finding.

#include <cstdint>
#include <string_view>

namespace anvil {

struct Version final {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t patch;
};

[[nodiscard]] Version version() noexcept;

// "0.1.0". Static storage, so the view is always valid.
[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace anvil
