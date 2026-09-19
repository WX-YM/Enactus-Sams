#pragma once

// A field id: "f" followed by one to three digits, and nothing else.
//
// This is a STRUCTURAL defence rather than a validation rule. A `fid` becomes a
// key inside the `ans` subdocument, where a leading `$` is a BSON operator and a
// dot is a path separator — `$set` or `a.b` there is not a rejected value, it is
// an update expression the server would execute. Making the id a 5-byte value
// object whose ONLY constructor is a parser means there is no code path anywhere
// that produces an unvalidated one: the hostile key is unrepresentable rather
// than filtered, so no future caller can forget to filter it.
//
// It is also the cheapest thing to compare on the submission hot path. An answer
// is matched against its declared field by a 4-byte array compare, not by a
// string compare against a heap-allocated key.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace anvil::forms {

class Fid final {
public:
    // "f" plus at most three digits. Three digits is 1 000 ids against a
    // 100-field ceiling, so the bound is the table's rather than a limit anyone
    // reaches.
    static constexpr std::size_t kCapacity = 4;

    constexpr Fid() noexcept : chars_{}, size_{0} {}

    // The ONLY way to build a non-empty one. nullopt for anything that is not
    // exactly `f[0-9]{1,3}`: `$set` fails on the first byte, `a.b` fails on the
    // digits, and a 40-character id fails on the length.
    [[nodiscard]] static constexpr std::optional<Fid> parse(std::string_view text) noexcept {
        if (text.size() < 2 || text.size() > kCapacity) { return std::nullopt; }
        if (text.front() != 'f') { return std::nullopt; }
        for (std::size_t i = 1; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') { return std::nullopt; }
        }
        Fid fid{};
        for (std::size_t i = 0; i < text.size(); ++i) { fid.chars_[i] = text[i]; }
        fid.size_ = static_cast<std::uint8_t>(text.size());
        return fid;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view{chars_.data(), size_};
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr bool operator==(const Fid& other) const noexcept {
        return size_ == other.size_ && chars_ == other.chars_;
    }
    [[nodiscard]] constexpr bool operator!=(const Fid& other) const noexcept {
        return !(*this == other);
    }

private:
    // Unused bytes are zero, so operator== compares the whole array and stays
    // branch-free rather than looping to `size_`.
    std::array<char, kCapacity> chars_;
    std::uint8_t                size_;
};

static_assert(sizeof(Fid) == 5, "Fid must stay a small value type with no heap");
static_assert(std::is_trivially_copyable_v<Fid>,
              "answers are copied per submission; a Fid must cost a memcpy");

}  // namespace anvil::forms
