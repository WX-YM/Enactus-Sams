#pragma once

// A typed result for operations that fail as part of normal control flow.
//
// Every function that can fail returns one of these or throws a typed domain
// exception — never a bare bool with an out-parameter, never -1 (ENGINEERING_RULES.md §8).
//
// Exceptions are for genuinely exceptional conditions (a broken CSPRNG, a lost
// database connection). Result is for expected outcomes: a stale version, a
// consumed capability token, a validation failure. Those happen thousands of
// times a day and should not unwind the stack.

#include <cassert>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "anvil/core/types.h"

namespace anvil {

// A failure carries the code and, optionally, the field it relates to. It never
// carries the submitted value: echoing input back is a reflected-XSS and
// log-injection vector, and with Arabic it is an encoding hazard too — echoing input back is how a validation message becomes a reflected-XSS and log-injection vector, and with non-Latin text an encoding hazard too.
// Members are ordered largest-alignment-first so there is no interior padding,
// and the initialiser list matches declaration order (ENGINEERING_RULES.md §2.3, §3.2).
struct Failure final {
    std::string_view field;   // constexpr field name, or empty
    ErrorCode        code;

    constexpr explicit Failure(ErrorCode c, std::string_view f = {}) noexcept
        : field{f}, code{c} {}
};

template <typename T>
class [[nodiscard]] Result final {
public:
    // Implicit so `return value;` and `return Failure{...};` both read cleanly.
    constexpr Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : state_{std::move(value)} {}
    constexpr Result(Failure failure) noexcept : state_{failure} {}

    [[nodiscard]] constexpr bool ok() const noexcept {
        return std::holds_alternative<T>(state_);
    }
    constexpr explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] constexpr const T& value() const& noexcept {
        assert(ok() && "Result::value() on a failed result");
        return std::get<T>(state_);
    }
    [[nodiscard]] constexpr T&& value() && noexcept {
        assert(ok() && "Result::value() on a failed result");
        return std::get<T>(std::move(state_));
    }

    [[nodiscard]] constexpr Failure error() const noexcept {
        assert(!ok() && "Result::error() on a successful result");
        return std::get<Failure>(state_);
    }
    [[nodiscard]] constexpr ErrorCode code() const noexcept {
        return ok() ? ErrorCode::Ok : std::get<Failure>(state_).code;
    }

    // Falls back rather than asserting, for the callers that have a sensible
    // default and do not care why it failed.
    [[nodiscard]] constexpr T value_or(T fallback) const& {
        return ok() ? std::get<T>(state_) : std::move(fallback);
    }

private:
    std::variant<T, Failure> state_;
};

// void specialisation: "it worked" or "it failed, here is why".
template <>
class [[nodiscard]] Result<void> final {
public:
    constexpr Result() noexcept : failure_{std::nullopt} {}
    constexpr Result(Failure failure) noexcept : failure_{failure} {}

    [[nodiscard]] constexpr bool ok() const noexcept { return !failure_.has_value(); }
    constexpr explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] constexpr Failure error() const noexcept {
        assert(!ok() && "Result::error() on a successful result");
        return *failure_;
    }
    [[nodiscard]] constexpr ErrorCode code() const noexcept {
        return failure_.has_value() ? failure_->code : ErrorCode::Ok;
    }

private:
    std::optional<Failure> failure_;
};

using Status = Result<void>;

inline constexpr Status ok() noexcept { return Status{}; }

inline constexpr Failure fail(ErrorCode code, std::string_view field = {}) noexcept {
    return Failure{code, field};
}

}  // namespace anvil
