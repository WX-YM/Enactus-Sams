// Fuzzing the chat message validators, and what they allocate.
//
// chat_text_test proves each named rule with one vector apiece. That does not
// prove the rules are the whole list, so shaped noise and mutations of valid
// inputs are thrown at the properties every acceptance must have: the value is
// valid UTF-8, inside its bound, and validates the same way a second time.
//
// It lives in anvil_alloc_tests because the second claim is about allocation.
// A message, a mention list, a preview and a reaction all arrive at a rate the
// sender chooses, so none of them may cost a heap allocation, accepted or
// refused. Two allocators are counted: operator new, through alloc_counter.h,
// and ICU's own, which is malloc behind uprv_malloc and invisible to the
// first. The reaction path is where ICU could allocate — a character break
// iterator is several kilobytes — so the count after each thread's first call
// is the claim, and the 32-byte bound checked before ICU is reached is what
// makes an oversized reaction cost nothing at all.
//
// Fixed seeds, as in validation_fuzz_test.cc: a failure is a reproduction, not
// an anecdote. std::mt19937 is banned for anything security-relevant and is
// exactly right for reproducible test input.

#include <gtest/gtest.h>

#include "alloc_counter.h"

#include <unicode/uclean.h>
#include <unicode/utypes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/chat/text.h"
#include "anvil/core/types.h"
#include "anvil/i18n/utf8.h"
#include "anvil/input/fields.h"
#include "anvil/input/html.h"

namespace anvil::chat {
namespace {

using input::Reason;
using testing::AllocationCounter;

constexpr std::uint32_t kSeed = 0x43484154;   // "CHAT"
constexpr int kFuzzIterations = 50000;

// --- counting ICU's allocator -------------------------------------------------

std::atomic<std::size_t> g_icu_allocations{0};

void* icu_alloc(const void*, std::size_t size) {
    g_icu_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size);
}
void* icu_realloc(const void*, void* memory, std::size_t size) {
    g_icu_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::realloc(memory, size);
}
void icu_free(const void*, void* memory) { std::free(memory); }

// Forwarding to the same malloc ICU uses by default, so a block allocated
// before the hook went in is still freed correctly after it.
void install_icu_counter() {
    static const bool installed = [] {
        UErrorCode status = U_ZERO_ERROR;
        u_setMemoryFunctions(nullptr, icu_alloc, icu_realloc, icu_free, &status);
        return U_SUCCESS(status);
    }();
    ASSERT_TRUE(installed);
}

class BothAllocators final {
public:
    BothAllocators() noexcept : icu_start_{g_icu_allocations.load(std::memory_order_relaxed)} {}

    [[nodiscard]] std::size_t heap() const noexcept { return heap_.count(); }
    [[nodiscard]] std::size_t icu() const noexcept {
        return g_icu_allocations.load(std::memory_order_relaxed) - icu_start_;
    }

private:
    AllocationCounter heap_;
    std::size_t       icu_start_;
};

// The NFC singleton and this thread's break iterator are created on first use;
// that one-off cost is not the per-call claim.
void warm_up() {
    ASSERT_EQ(validate_message_text("caf\u00E9", kMaxMessageCodePoints), Reason::Ok);
    ASSERT_EQ(validate_reaction("\U0001F44D"), Reason::Ok);
}

[[nodiscard]] Uuid user(std::uint8_t tag) noexcept {
    Uuid id{};
    id[0] = tag;
    return id;
}

// --- shaped input ---------------------------------------------------------------

// Fragments chosen for what the validators decide on: line breaks in every
// spelling, controls, C1, bidi controls and isolates, the blank characters,
// combining marks, ZWJ, regional indicators and skin tones, non-characters,
// and broken UTF-8.
constexpr std::array<std::string_view, 40> kFragments{{
    "a", "Z", " ", "\n", "\t", "\r", "\r\n", "\x01", "\x7F", "\xC2\x85", "\xC2\x9B",
    "\u0628", "\u0645\u0631\u062D\u0628\u0627", "\u064E", "\u0301", "e", "\u00E9", "\u0958",
    "\u00A0", "\u3000", "\u3164", "\u200B", "\u200C", "\u200D", "\u200E", "\u061C",
    "\xE2\x80\xAE", "\xE2\x80\xAA", "\xE2\x81\xA7", "\xE2\x81\xA9", "\u2028", "\uFEFF",
    "\U0001F44D", "\U0001F3FD", "\U0001F1EA", "\U0001F468", "\uFE0F",
    "\xC3", "\xED\xA0\x80", "\xEF\xBF\xBE",
}};

[[nodiscard]] std::string shaped(std::mt19937& rng, std::size_t max_fragments) {
    std::uniform_int_distribution<std::size_t> count{0, max_fragments};
    std::uniform_int_distribution<std::size_t> pick{0, kFragments.size() - 1};
    std::string out;
    const std::size_t n = count(rng);
    out.reserve(n * 4);
    for (std::size_t i = 0; i < n; ++i) { out.append(kFragments[pick(rng)]); }
    return out;
}

// One byte-level mutation: the interesting edges are a byte away from a valid
// value, where shaped noise rarely lands.
void mutate(std::mt19937& rng, std::string& text) {
    std::uniform_int_distribution<int> byte{0, 255};
    const std::size_t size = text.size();
    const std::size_t at = size == 0 ? 0 : rng() % size;
    switch (rng() % 5) {
        case 0:
            if (size != 0) { text[at] = static_cast<char>(text[at] ^ (1 << (rng() % 8))); }
            break;
        case 1:
            if (size != 0) { text[at] = static_cast<char>(byte(rng)); }
            break;
        case 2:
            text.insert(at, 1, static_cast<char>(byte(rng)));
            break;
        case 3:
            if (size != 0) { text.erase(at, 1); }
            break;
        default:
            text.resize(at);
            break;
    }
}

// Mutation seeds: values every validator accepts.
constexpr std::array<std::string_view, 6> kValidTexts{{
    "hello there",
    "\u0645\u0631\u062D\u0628\u0627 \u0623\u062D\u0645\u062F",
    "line one\nline two\tend",
    "the word \xE2\x81\xA7\u0645\u0631\u062D\u0628\u0627\xE2\x81\xA9 here",
    "caf\u00E9 \U0001F44D",
    "\u0628\u064E\u0627\u0628",
}};

constexpr std::array<std::string_view, 5> kValidUrls{{
    "https://example.com/",
    "https://example.com/a?b=c#d",
    "HTTPS://Sub.Example.org:8443/path/to?q=1",
    "https://xn--mgbh0fb.example/x",
    "https://example.com/%E2%9C%93",
}};

constexpr std::array<std::string_view, 6> kValidReactions{{
    "\U0001F44D",
    "\U0001F44D\U0001F3FD",
    "\U0001F468\u200D\U0001F469\u200D\U0001F467\u200D\U0001F466",
    "\U0001F1EA\U0001F1EC",
    "1\uFE0F\u20E3",
    "\u0628\u064E",
}};

// --- properties -----------------------------------------------------------------

void expect_sound_message(std::string_view text, std::uint32_t max_code_points,
                          Reason reason) {
    // Re-validated from a separate copy: a verdict that depended on the
    // address, or on state left by the previous call, would differ here.
    const std::string copy{text};
    ASSERT_EQ(validate_message_text(copy, max_code_points), reason);
    if (reason != Reason::Ok) { return; }
    ASSERT_EQ(i18n::validate(text), i18n::Utf8Error::Ok) << "accepted invalid UTF-8";
    ASSERT_GE(i18n::count_code_points(text), 1U);
    ASSERT_LE(i18n::count_code_points(text), max_code_points);
    ASSERT_EQ(text.find('\r'), std::string_view::npos);
}

void expect_sound_reaction(std::string_view reaction, Reason reason) {
    const std::string copy{reaction};
    ASSERT_EQ(validate_reaction(copy), reason);
    if (reason != Reason::Ok) { return; }
    ASSERT_EQ(i18n::validate(reaction), i18n::Utf8Error::Ok) << "accepted invalid UTF-8";
    ASSERT_LE(reaction.size(), kMaxReactionBytes);
    ASSERT_LE(i18n::count_code_points(reaction), kMaxReactionCodePoints);
}

void expect_sound_preview(const LinkPreview& preview,
                          const std::optional<input::FieldError>& error) {
    const std::optional<input::FieldError> again = validate_link_preview(preview);
    ASSERT_EQ(again.has_value(), error.has_value());
    if (error.has_value()) {
        ASSERT_EQ(again->field, error->field);
        ASSERT_EQ(again->reason, error->reason);
        ASSERT_NE(error->reason, Reason::Ok);
        return;
    }
    ASSERT_GE(preview.url.size(), 8U);
    for (const char c : preview.url) {
        const auto byte = static_cast<unsigned char>(c);
        ASSERT_TRUE(byte > 0x20U && byte < 0x7FU) << "accepted a non-graphic URL byte";
    }
    ASSERT_TRUE(input::is_safe_link_target(preview.url));
    ASSERT_EQ(input::check_url(preview.url, input::UrlUse::Link), Reason::Ok);
    ASSERT_EQ(i18n::validate(preview.title), i18n::Utf8Error::Ok);
    ASSERT_EQ(i18n::validate(preview.description), i18n::Utf8Error::Ok);
    ASSERT_EQ(preview.title.find('\n'), std::string_view::npos);
}

// An independent statement of the span rules, so the fuzz compares two
// implementations rather than one against itself.
[[nodiscard]] bool spans_are_sound(std::string_view text, const std::vector<MentionSpan>& spans) {
    if (spans.size() > kMaxMentions) { return false; }
    const std::size_t total = i18n::count_code_points(text);
    std::size_t covered_up_to = 0;
    for (const MentionSpan& span : spans) {
        if (is_nil(span.user) || span.length == 0) { return false; }
        if (span.offset < covered_up_to) { return false; }
        if (std::size_t{span.offset} + std::size_t{span.length} > total) { return false; }
        covered_up_to = std::size_t{span.offset} + std::size_t{span.length};
    }
    return true;
}

// --- allocation claims ------------------------------------------------------------

TEST(ChatTextAllocations, NoValidatorAllocatesAcceptedOrRefused) {
    install_icu_counter();
    warm_up();

    const std::string arabic_at_bound = [] {
        std::string out;
        for (std::uint32_t i = 0; i < kMaxMessageCodePoints; ++i) { out.append("\u0628"); }
        return out;
    }();
    const std::string hostile(1U << 20U, 'a');
    const std::array<MentionSpan, 2> spans{{{user(1), 0, 2}, {user(2), 3, 2}}};
    const std::array<MentionSpan, 2> overlapping{{{user(1), 0, 3}, {user(2), 2, 2}}};

    const BothAllocators counter;

    EXPECT_EQ(validate_message_text(arabic_at_bound, kMaxMessageCodePoints), Reason::Ok);
    EXPECT_EQ(validate_message_text(kValidTexts[3], kMaxMessageCodePoints), Reason::Ok);
    EXPECT_EQ(validate_message_text(hostile, kMaxMessageCodePoints), Reason::TooLong);
    EXPECT_EQ(validate_message_text("cafe\u0301", kMaxMessageCodePoints), Reason::BadFormat);
    EXPECT_EQ(validate_message_text("\u200B\u3164", kMaxMessageCodePoints), Reason::Required);
    EXPECT_EQ(validate_message_text("a\rb", kMaxMessageCodePoints), Reason::BadCharset);

    EXPECT_EQ(validate_mentions("ab cd", spans), Reason::Ok);
    EXPECT_EQ(validate_mentions("abcdef", overlapping), Reason::BadFormat);

    EXPECT_FALSE(validate_link_preview({"https://example.com/x", "Title", "Text"}).has_value());
    EXPECT_TRUE(validate_link_preview({"javascript:alert(1)", "", ""}).has_value());
    EXPECT_TRUE(validate_link_preview({hostile, "", ""}).has_value());
    EXPECT_TRUE(validate_link_preview({"https://example.com/", hostile, ""}).has_value());

    EXPECT_EQ(validate_reaction(kValidReactions[2]), Reason::Ok);
    EXPECT_EQ(validate_reaction("\U0001F1EA\U0001F1EC"), Reason::Ok);
    EXPECT_EQ(validate_reaction("\U0001F44D\U0001F44D"), Reason::BadFormat);
    EXPECT_EQ(validate_reaction(hostile), Reason::TooLong);

    EXPECT_EQ(counter.heap(), 0U) << "a chat validator allocated through operator new";
    EXPECT_EQ(counter.icu(), 0U) << "a chat validator allocated through ICU";
}

// --- fuzz -------------------------------------------------------------------------

TEST(ChatTextFuzz, MessageTextHoldsItsPropertiesUnderNoise) {
    install_icu_counter();
    warm_up();
    std::mt19937 rng{kSeed};
    std::uniform_int_distribution<std::uint32_t> bound{0, kMaxMessageCodePoints + 8U};
    std::size_t allocations = 0;
    std::size_t accepted = 0;
    for (int i = 0; i < kFuzzIterations; ++i) {
        std::string text = shaped(rng, 24);
        if ((i & 1) != 0) {
            text.assign(kValidTexts[rng() % kValidTexts.size()]);
            mutate(rng, text);
        }
        const std::uint32_t max_code_points = (i % 3 == 0) ? bound(rng) % 32U : bound(rng);
        const BothAllocators counter;
        const Reason reason = validate_message_text(text, max_code_points);
        allocations += counter.heap() + counter.icu();
        accepted += reason == Reason::Ok ? 1U : 0U;
        expect_sound_message(text, std::min(max_code_points, kMaxMessageCodePoints), reason);
        if (::testing::Test::HasFatalFailure()) { return; }
    }
    EXPECT_EQ(allocations, 0U);
    EXPECT_GT(accepted, 1000U) << "the fuzz has stopped reaching the acceptance path";
}

TEST(ChatTextFuzz, MentionsAgreeWithAnIndependentStatement) {
    std::mt19937 rng{kSeed + 1};
    std::uniform_int_distribution<std::size_t> how_many{0, kMaxMentions + 2};
    std::uniform_int_distribution<std::uint32_t> small{0, 24};
    std::size_t allocations = 0;
    for (int i = 0; i < kFuzzIterations; ++i) {
        const std::string text{kValidTexts[rng() % kValidTexts.size()]};
        std::vector<MentionSpan> spans(how_many(rng));
        std::uint32_t cursor = 0;
        for (MentionSpan& span : spans) {
            // Mostly well-formed and ascending, so the checks past the first
            // one are reached; a fifth of fields are raw noise.
            const auto tag = static_cast<std::uint8_t>(1 + rng() % 9);
            span.user = (rng() % 50 == 0) ? kNilUuid : user(tag);
            span.offset =
                (rng() % 5 == 0) ? static_cast<std::uint32_t>(rng()) : cursor + small(rng) % 3;
            span.length = (rng() % 5 == 0) ? static_cast<std::uint32_t>(rng()) : small(rng) % 4;
            cursor = span.offset + span.length;
        }
        const AllocationCounter counter;
        const Reason reason = validate_mentions(text, spans);
        allocations += counter.count();
        ASSERT_EQ(reason == Reason::Ok, spans_are_sound(text, spans));
    }
    EXPECT_EQ(allocations, 0U);
}

TEST(ChatTextFuzz, LinkPreviewHoldsItsPropertiesUnderNoise) {
    install_icu_counter();
    warm_up();
    std::mt19937 rng{kSeed + 2};
    std::size_t allocations = 0;
    std::size_t accepted = 0;
    for (int i = 0; i < kFuzzIterations; ++i) {
        std::string url{kValidUrls[rng() % kValidUrls.size()]};
        for (std::size_t m = 0, n = rng() % 3; m < n; ++m) { mutate(rng, url); }
        std::string title = (rng() % 3 == 0) ? std::string{} : shaped(rng, 8);
        std::string description = shaped(rng, 16);
        if ((rng() & 1) != 0) {
            title.assign(kValidTexts[rng() % kValidTexts.size()]);
            if ((rng() & 1) != 0) { mutate(rng, title); }
        }
        const LinkPreview preview{url, title, description};
        const BothAllocators counter;
        const std::optional<input::FieldError> error = validate_link_preview(preview);
        allocations += counter.heap() + counter.icu();
        accepted += error.has_value() ? 0U : 1U;
        expect_sound_preview(preview, error);
        if (::testing::Test::HasFatalFailure()) { return; }
    }
    EXPECT_EQ(allocations, 0U);
    EXPECT_GT(accepted, 1000U) << "the fuzz has stopped reaching the acceptance path";
}

TEST(ChatTextFuzz, ReactionHoldsItsPropertiesUnderNoise) {
    install_icu_counter();
    warm_up();
    std::mt19937 rng{kSeed + 3};
    std::size_t allocations = 0;
    std::size_t accepted = 0;
    for (int i = 0; i < kFuzzIterations; ++i) {
        std::string reaction = shaped(rng, 4);
        if ((i & 1) != 0) {
            reaction.assign(kValidReactions[rng() % kValidReactions.size()]);
            if ((rng() & 3) != 0) { mutate(rng, reaction); }
        }
        const BothAllocators counter;
        const Reason reason = validate_reaction(reaction);
        allocations += counter.heap() + counter.icu();
        accepted += reason == Reason::Ok ? 1U : 0U;
        expect_sound_reaction(reaction, reason);
        if (::testing::Test::HasFatalFailure()) { return; }
    }
    EXPECT_EQ(allocations, 0U);
    EXPECT_GT(accepted, 1000U) << "the fuzz has stopped reaching the break iterator";
}

TEST(ChatTextFuzz, ArbitraryBytesNeverCrashAnyValidator) {
    std::mt19937 rng{kSeed + 4};
    std::uniform_int_distribution<std::size_t> length{0, 64};
    std::uniform_int_distribution<int> byte{0, 255};
    for (int i = 0; i < kFuzzIterations; ++i) {
        std::string bytes(length(rng), '\0');
        for (char& c : bytes) { c = static_cast<char>(byte(rng)); }
        expect_sound_message(bytes, kMaxMessageCodePoints,
                             validate_message_text(bytes, kMaxMessageCodePoints));
        expect_sound_reaction(bytes, validate_reaction(bytes));
        const LinkPreview preview{bytes, bytes, bytes};
        expect_sound_preview(preview, validate_link_preview(preview));
        if (::testing::Test::HasFatalFailure()) { return; }
    }
}

}  // namespace
}  // namespace anvil::chat
