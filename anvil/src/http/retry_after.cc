#include "anvil/http/retry_after.h"

#include <array>
#include <charconv>
#include <string>
#include <system_error>

namespace anvil::http {

namespace {

// The value has at most ten digits, so it is formatted on the stack: a
// std::to_string on a shed path is a heap allocation charged to the refusal,
// which is the request that must cost the least (ENGINEERING_RULES.md §2.1).
constexpr std::size_t kMaxDigits = 10;

// Every one of the three rules the header states, exercised at build time
// rather than trusted. Each of these is a header a client honours incorrectly
// if the arithmetic goes the other way.
constexpr RateLimitRule kMinuteRule{"t", std::chrono::seconds{60}, 1};

static_assert(retry_after_seconds(
                  RateLimitVerdict{.count = 2,
                                   .remaining = std::chrono::milliseconds{1400},
                                   .allowed = false,
                                   .degraded = false},
                  kMinuteRule) == 2,
              "a partial second rounds up, or the client returns before the window closes");

static_assert(retry_after_seconds(
                  RateLimitVerdict{.count = 2,
                                   .remaining = std::chrono::milliseconds{40},
                                   .allowed = false,
                                   .degraded = false},
                  kMinuteRule) == 1,
              "a nearly-closed window is one second, never zero: zero means retry now");

static_assert(retry_after_seconds(
                  RateLimitVerdict{.count = 2,
                                   .remaining = std::chrono::milliseconds::zero(),
                                   .allowed = false,
                                   .degraded = false},
                  kMinuteRule) == 1,
              "a closed window is one second, never zero");

static_assert(retry_after_seconds(
                  RateLimitVerdict{.count = 2,
                                   .remaining = std::chrono::hours{1},
                                   .allowed = false,
                                   .degraded = false},
                  kMinuteRule) == 60,
              "a key that outlived its rule is clamped to the rule, not honoured");

static_assert(retry_after_seconds(
                  RateLimitVerdict{.count = 2,
                                   .remaining = std::chrono::milliseconds{-5},
                                   .allowed = false,
                                   .degraded = false},
                  kMinuteRule) == 1,
              "a negative remaining window never becomes a negative header");

}  // namespace

void apply_retry_after(drogon::HttpResponse& response, std::uint32_t seconds) {
    std::array<char, kMaxDigits> digits{};
    const std::to_chars_result written =
        std::to_chars(digits.data(), digits.data() + digits.size(), seconds);
    if (written.ec != std::errc{}) { return; }

    response.addHeader(std::string{kRetryAfterHeader},
                       std::string{digits.data(),
                                   static_cast<std::size_t>(written.ptr - digits.data())});
}

}  // namespace anvil::http
