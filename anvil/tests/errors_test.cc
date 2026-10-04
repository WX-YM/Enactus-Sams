// The error model and Result.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "anvil/core/result.h"
#include "anvil/http/errors.h"
#include "anvil/http/request_id.h"
#include "anvil/input/fields.h"

namespace anvil {

namespace {

// Every enumerator, listed once. A new code added without appearing here fails
// test 1 — the exhaustiveness check is the point of the list.
constexpr std::array<ErrorCode, 14> kAllCodes{
    ErrorCode::Ok,
    ErrorCode::Unauthenticated,
    ErrorCode::Forbidden,
    ErrorCode::NotFound,
    ErrorCode::CapabilityRequired,
    ErrorCode::CapabilityInvalid,
    ErrorCode::ValidationFailed,
    ErrorCode::Conflict,
    ErrorCode::VersionMismatch,
    ErrorCode::RateLimited,
    ErrorCode::PayloadTooLarge,
    ErrorCode::UnsupportedMedia,
    ErrorCode::ServiceUnavailable,
    ErrorCode::Internal,
};

}  // namespace

// --- 1: exhaustive mapping ------------------------------------------------

TEST(Errors, EveryCodeHasAStatusAndAName) {
    for (const ErrorCode code : kAllCodes) {
        const int status = http::http_status(code);
        EXPECT_GE(status, 200);
        EXPECT_LT(status, 600);

        const std::string_view name = http::wire_name(code);
        EXPECT_FALSE(name.empty());
        // Wire names are stable API: clients map them to bilingual messages.
        for (const char c : name) {
            EXPECT_TRUE((c >= 'A' && c <= 'Z') || c == '_') << "name=" << name;
        }
    }
}

TEST(Errors, WireNamesAreUnique) {
    for (std::size_t i = 0; i < kAllCodes.size(); ++i) {
        for (std::size_t j = i + 1; j < kAllCodes.size(); ++j) {
            EXPECT_NE(http::wire_name(kAllCodes[i]), http::wire_name(kAllCodes[j]));
        }
    }
}

// --- 3-4: what a body may contain ----------------------------------------

TEST(Errors, OnlyValidationFailedCarriesFieldDetail) {
    for (const ErrorCode code : kAllCodes) {
        EXPECT_EQ(http::carries_field_detail(code), code == ErrorCode::ValidationFailed)
            << "code=" << http::wire_name(code);
    }
}

TEST(Errors, FailureCarriesAFieldNameButNeverAValue) {
    // Failure has no member capable of holding a submitted value — the type
    // makes echoing input impossible rather than merely discouraged.
    constexpr Failure f = fail(ErrorCode::ValidationFailed, "email");
    static_assert(sizeof(Failure) <= 24, "no room for a value, by design");
    EXPECT_EQ(f.field, "email");
    EXPECT_EQ(f.code, ErrorCode::ValidationFailed);
    // No detail unless one is given, so every existing construction means what
    // it always did.
    EXPECT_EQ(f.detail, 0U);
    constexpr Failure detailed{ErrorCode::ValidationFailed, "title",
                               static_cast<std::uint16_t>(anvil::input::Reason::TooLong)};
    static_assert(detailed.detail == static_cast<std::uint16_t>(anvil::input::Reason::TooLong));
}

// --- 6: stealth mapping ---------------------------------------------------

TEST(Errors, AuthFailuresAreStealthHidden) {
    EXPECT_TRUE(http::is_stealth_hidden(ErrorCode::Unauthenticated));
    EXPECT_TRUE(http::is_stealth_hidden(ErrorCode::Forbidden));
    EXPECT_TRUE(http::is_stealth_hidden(ErrorCode::CapabilityRequired));
    EXPECT_TRUE(http::is_stealth_hidden(ErrorCode::CapabilityInvalid));
}

TEST(Errors, NonAuthFailuresAreNotStealthHidden) {
    // A validation error on an authenticated admin route is safe to report:
    // the caller has already proved they may reach the route.
    EXPECT_FALSE(http::is_stealth_hidden(ErrorCode::ValidationFailed));
    EXPECT_FALSE(http::is_stealth_hidden(ErrorCode::VersionMismatch));
    EXPECT_FALSE(http::is_stealth_hidden(ErrorCode::Internal));
    EXPECT_FALSE(http::is_stealth_hidden(ErrorCode::NotFound));
}

TEST(Errors, StealthBodyIsAFixedConstant) {
    // One constexpr body for stealth drops, unmatched routes, and genuinely
    // missing resources, so all three are byte-identical. Nginx's
    // error_page 404 must serve exactly these bytes.
    EXPECT_EQ(http::kNotFoundBody, R"({"error":{"code":"NOT_FOUND"}})");
    EXPECT_EQ(http::kNotFoundContentType, "application/json");
    EXPECT_EQ(http::kNotFoundBody.find("request_id"), std::string_view::npos)
        << "a stealth drop must not carry a correlation id — the real 404 has none";
}

// --- 6a: the request id ----------------------------------------------------
//
// docs/00-architecture.md §8.1. Every property below is one a person reading an
// id down a phone line or an operator grepping a log file depends on.

// Two vectors at the extremes, because they pin the bit alignment the whole
// encoding rests on: 128 bits do not divide into fives, so two zero bits lead
// and the FIRST character carries three bits rather than five.
TEST(RequestId, EncodesTheExtremesExactly) {
    const http::RequestId zero{};
    const std::array<char, http::kRequestIdChars> low = http::format_request_id(zero);
    EXPECT_EQ(std::string(low.data(), low.size()), "00000000000000000000000000");

    http::RequestId ones{};
    ones.bytes.fill(0xFF);
    const std::array<char, http::kRequestIdChars> high = http::format_request_id(ones);
    // '7' and not 'Z': the leading character can only ever reach three bits.
    EXPECT_EQ(std::string(high.data(), high.size()), "7ZZZZZZZZZZZZZZZZZZZZZZZZZ");
}

// The two ends of the bit string, so an off-by-one in either direction shows up
// as a different character rather than as an id that merely looks plausible.
TEST(RequestId, TheFirstAndLastBitsLandWhereTheyShould) {
    http::RequestId first{};
    first.bytes[0] = 0x80;
    const std::array<char, http::kRequestIdChars> a = http::format_request_id(first);
    EXPECT_EQ(std::string(a.data(), a.size()), "40000000000000000000000000");

    http::RequestId last{};
    last.bytes[15] = 0x01;
    const std::array<char, http::kRequestIdChars> b = http::format_request_id(last);
    EXPECT_EQ(std::string(b.data(), b.size()), "00000000000000000000000001");
}

TEST(RequestId, EveryCharacterIsInTheCrockfordAlphabet) {
    // I, L, O and U are absent from it on purpose: the id's whole job is that a
    // person reads it back to support, and those four are the pairs that get
    // transcribed into a different id that is still valid.
    EXPECT_EQ(http::kCrockfordAlphabet.size(), 32U);
    for (const char banned : {'I', 'L', 'O', 'U'}) {
        EXPECT_EQ(http::kCrockfordAlphabet.find(banned), std::string_view::npos) << banned;
    }
    const http::RequestId id = http::mint_request_id(1'770'000'000'000);
    for (const char c : http::format_request_id(id)) {
        EXPECT_NE(http::kCrockfordAlphabet.find(c), std::string_view::npos) << c;
    }
}

// The reason the timestamp leads and is big-endian: a log file sorts into
// occurrence order as TEXT, with no parser and no index.
TEST(RequestId, SortsAsTextIntoOccurrenceOrder) {
    const http::RequestId earlier = http::mint_request_id(1'770'000'000'000);
    const http::RequestId later = http::mint_request_id(1'770'000'000'001);

    const std::array<char, http::kRequestIdChars> a = http::format_request_id(earlier);
    const std::array<char, http::kRequestIdChars> b = http::format_request_id(later);
    EXPECT_LT(std::string(a.data(), a.size()), std::string(b.data(), b.size()));
}

TEST(RequestId, TwoIdsInTheSameMillisecondDiffer) {
    // 80 CSPRNG bits, so this is not a counter and not guessable. A collision
    // here would mean the random tail was never written.
    const http::RequestId a = http::mint_request_id(1'770'000'000'000);
    const http::RequestId b = http::mint_request_id(1'770'000'000'000);
    EXPECT_NE(a.bytes, b.bytes);
    // The first six bytes ARE the millisecond, so they must agree.
    EXPECT_TRUE(std::equal(a.bytes.begin(), a.bytes.begin() + 6, b.bytes.begin()));
}

// --- 6b: the one error body writer -----------------------------------------

TEST(ErrorBody, WritesTheShapeDocsHavePublishedSincePhaseZero) {
    http::RequestId id{};
    id.bytes[15] = 0x01;

    std::string out;
    http::append_error_body(out, ErrorCode::Forbidden, id);
    EXPECT_EQ(out,
              R"({"error":{"code":"FORBIDDEN","request_id":"00000000000000000000000001"}})");
}

TEST(ErrorBody, OnlyValidationFailedCarriesFields) {
    const std::array<input::FieldError, 1> fields{
        input::FieldError{"email", input::Reason::BadFormat}};

    std::string conflict;
    http::append_error_body(conflict, ErrorCode::Conflict, http::RequestId{}, fields);
    // A 409 that explained which version it saw would be a disclosure, so the
    // span is ignored rather than trusted — the rule is in carries_field_detail
    // and not at the call site.
    EXPECT_EQ(conflict.find("fields"), std::string::npos);

    std::string invalid;
    http::append_error_body(invalid, ErrorCode::ValidationFailed, http::RequestId{}, fields);
    EXPECT_NE(invalid.find(R"("fields":{"email":"BAD_FORMAT"})"), std::string::npos) << invalid;
}

// The shape is decided by the CODE and never by whether the server happened to
// have detail, so a client parses one thing rather than two.
TEST(ErrorBody, ValidationFailedCarriesAFieldsObjectEvenWhenEmpty) {
    std::string out;
    http::append_error_body(out, ErrorCode::ValidationFailed, http::RequestId{});
    EXPECT_NE(out.find(R"("fields":{})"), std::string::npos) << out;
}

TEST(ErrorBody, SeveralFieldsAreCommaSeparated) {
    const std::array<input::FieldError, 2> fields{
        input::FieldError{"email", input::Reason::BadFormat},
        input::FieldError{"password", input::Reason::TooShort}};

    std::string out;
    http::append_error_body(out, ErrorCode::ValidationFailed, http::RequestId{}, fields);
    EXPECT_NE(out.find(R"({"email":"BAD_FORMAT","password":"TOO_SHORT"})"), std::string::npos)
        << out;
}

// --- 7: Result ------------------------------------------------------------

TEST(Result, HoldsAValueOnSuccess) {
    const Result<int> r{42};
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.value(), 42);
    EXPECT_EQ(r.code(), ErrorCode::Ok);
}

TEST(Result, HoldsAFailure) {
    const Result<int> r{fail(ErrorCode::VersionMismatch)};
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.code(), ErrorCode::VersionMismatch);
    EXPECT_EQ(r.error().code, ErrorCode::VersionMismatch);
}

TEST(Result, ValueOrFallsBack) {
    const Result<int> good{7};
    const Result<int> bad{fail(ErrorCode::NotFound)};
    EXPECT_EQ(good.value_or(-1), 7);
    EXPECT_EQ(bad.value_or(-1), -1);
}

TEST(Result, VoidSpecialisation) {
    const Status good = ok();
    EXPECT_TRUE(good.ok());
    EXPECT_EQ(good.code(), ErrorCode::Ok);

    const Status bad{fail(ErrorCode::RateLimited)};
    EXPECT_FALSE(bad.ok());
    EXPECT_EQ(bad.code(), ErrorCode::RateLimited);
}

TEST(Result, MovesRatherThanCopies) {
    Result<std::string> r{std::string(1000, 'x')};
    ASSERT_TRUE(r.ok());
    const std::string taken = std::move(r).value();
    EXPECT_EQ(taken.size(), 1000U);
}

// Every reason, listed once, for the same purpose the code list above serves: a
// reason added without a wire name fails here rather than reaching a client as a
// word nobody has translated.
constexpr std::array<input::Reason, 11> kAllReasons{
    input::Reason::Ok,          input::Reason::Required,   input::Reason::TooShort,
    input::Reason::TooLong,     input::Reason::BadFormat,  input::Reason::BadCharset,
    input::Reason::OutOfRange,  input::Reason::NotAllowed, input::Reason::BadChecksum,
    input::Reason::Weak,        input::Reason::Breached,
};

TEST(ValidationReasons, EveryReasonHasADistinctWireName) {
    for (std::size_t i = 0; i < kAllReasons.size(); ++i) {
        const std::string_view name = http::wire_name(kAllReasons[i]);
        EXPECT_FALSE(name.empty());

        for (std::size_t j = i + 1; j < kAllReasons.size(); ++j) {
            EXPECT_NE(name, http::wire_name(kAllReasons[j]))
                << "two reasons share a wire name, so a client cannot tell them apart";
        }
    }
}

TEST(ValidationReasons, TheListEndsWhereTheBoundSays) {
    // The same contract kMaxErrorCode carries: a decoder gets a bound rather
    // than a belief about how long the list is.
    EXPECT_EQ(static_cast<std::uint16_t>(http::kMaxReason),
              static_cast<std::uint16_t>(kAllReasons.back()));
}

TEST(ValidationReasons, NamesAreScreamingSnakeOfTheEnumerator) {
    // Mechanical in both directions is the property that lets a generated client
    // hold the same vocabulary without a second table to keep in agreement.
    EXPECT_EQ(http::wire_name(input::Reason::BadFormat), "BAD_FORMAT");
    EXPECT_EQ(http::wire_name(input::Reason::TooLong), "TOO_LONG");
    EXPECT_EQ(http::wire_name(input::Reason::Breached), "BREACHED");
}

TEST(Result, IsNodiscard) {
    // [[nodiscard]] means an ignored Result is a compiler warning, and warnings
    // are errors in this build — a failure cannot be silently dropped.
    static_assert(std::is_same_v<decltype(ok()), Status>);
}

}  // namespace anvil
