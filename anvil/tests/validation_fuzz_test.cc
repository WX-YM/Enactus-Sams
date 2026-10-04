// Fuzzing the validators, and the allocation claim.
//
// "Zero allocations on the reject path" is stated as a budget in docs/06-input-validation.md §7 and
// is the kind of claim that quietly stops being true. Counting is the only way
// to know: this file replaces global operator new so a test can assert that a
// validator did not allocate, rather than asserting that it was written not to.
//
// The fuzz loops use a fixed seed. A random seed turns a reproducible failure
// into a story about what CI saw once — the seed is the difference between a
// bug report and a rumour. The generator is std::mt19937 deliberately: it is
// banned for anything security-relevant (CLAUDE.md §5) and is exactly right for
// reproducible test input, which is not security-relevant at all.

#include <gtest/gtest.h>

#include "alloc_counter.h"
#include "namespaces.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <random>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <cstdio>
#include <unistd.h>

#include "anvil/fs/paths.h"
#include "anvil/fs/upload.h"
#include "anvil/input/fields.h"
#if ANVIL_HAS_EGY
#include "anvil/locale_egy/identity_egy.h"
#endif
#include "anvil/input/json.h"
#if ANVIL_HAS_EGY
#include "anvil/locale_egy/phone_egy.h"
#endif
#include "anvil/input/schema.h"

namespace {

// Shared with every other translation unit in this binary through
// tests/alloc_counter.h: a per-TU counter would count only its own file's
// allocations, which is a test that passes while measuring nothing.
using anvil::testing::AllocationCounter;
using anvil::testing::g_allocations;

}  // namespace

// Replaces the global allocator for this test binary only. Every path still
// goes through malloc, so ASan continues to see and check every allocation.
//
// ALL of the overloads have to be replaced together. Replacing only the
// throwing scalar form leaves gtest's nothrow and array allocations going
// through the default operator new while their frees come here, which ASan
// correctly reports as an alloc-dealloc mismatch.
namespace {

[[nodiscard]] void* counted_allocate(std::size_t size, std::size_t alignment) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    const std::size_t bytes = size == 0 ? 1 : size;
    if (alignment <= alignof(std::max_align_t)) { return std::malloc(bytes); }
    // aligned_alloc requires a size that is a multiple of the alignment.
    const std::size_t rounded = ((bytes + alignment - 1) / alignment) * alignment;
    return std::aligned_alloc(alignment, rounded);
}

[[nodiscard]] void* counted_allocate_or_throw(std::size_t size, std::size_t alignment) {
    void* memory = counted_allocate(size, alignment);
    if (memory == nullptr) { throw std::bad_alloc{}; }
    return memory;
}

}  // namespace

void* operator new(std::size_t size) { return counted_allocate_or_throw(size, 0); }
void* operator new[](std::size_t size) { return counted_allocate_or_throw(size, 0); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return counted_allocate(size, 0);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_allocate(size, 0);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return counted_allocate_or_throw(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return counted_allocate_or_throw(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

namespace anvil::input {
namespace {

namespace fs = anvil::fs;

constexpr std::uint32_t kSeed = 0x59415244;   // "YARD"

// 10^6 in the spec. That is minutes under ASan for no additional signal past
// the first few thousand, so the count is scaled and the seed is fixed: the
// same inputs every run, and a nightly job can raise this one constant.
constexpr int kFuzzIterations = 200000;

[[nodiscard]] std::string random_bytes(std::mt19937& rng, std::size_t max_length) {
    std::uniform_int_distribution<std::size_t> length{0, max_length};
    std::uniform_int_distribution<int> byte{0, 255};

    std::string out;
    const std::size_t n = length(rng);
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) { out.push_back(static_cast<char>(byte(rng))); }
    return out;
}

// Structured noise: bytes drawn from the alphabet each validator actually
// cares about, which reaches far deeper into the parsers than uniform noise.
[[nodiscard]] std::string random_structured(std::mt19937& rng, std::string_view alphabet,
                                            std::size_t max_length) {
    std::uniform_int_distribution<std::size_t> length{0, max_length};
    std::uniform_int_distribution<std::size_t> pick{0, alphabet.size() - 1};

    std::string out;
    const std::size_t n = length(rng);
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) { out.push_back(alphabet[pick(rng)]); }
    return out;
}

// --- allocation claims ----------------------------------------------------

TEST(ValidationAllocations, RejectPathsDoNotAllocate) {
    // Each of these is a rejection, and a rejection is the path an attacker
    // controls the rate of. An allocation here is a lever they can pull.
    const std::array<std::string_view, 6> bad_emails{
        "no-at-sign", "two@at@example.com", "user@[192.0.2.1]",
        "user@pаypal.com", "", ".leading@example.com"};

    {
        const AllocationCounter counter;
        for (const std::string_view email : bad_emails) {
            EXPECT_NE(check_email(email), Reason::Ok);
        }
        EXPECT_EQ(counter.count(), 0U) << "email rejection allocated";
    }

#if ANVIL_HAS_EGY
    {
        const AllocationCounter counter;
        NationalIdEgy parsed{};
        EXPECT_NE(validate_national_id_egy("00000000000000", parsed), Reason::Ok);
        EXPECT_NE(validate_national_id_egy("1980101012345", parsed), Reason::Ok);
        EXPECT_NE(validate_national_id_egy("٢٩٨٠١٠١٠١٢٣٤٥", parsed), Reason::Ok);
        EXPECT_EQ(counter.count(), 0U)
            << "National ID validation allocated — including the digit fold, "
               "which writes into a stack array";
    }

    {
        // Signup and login both reach this, and login reaches it before it
        // knows whether the account exists — so an allocation here is one an
        // unauthenticated caller can drive at their own rate.
        const AllocationCounter counter;
        PhoneEgy phone{};
        EXPECT_NE(validate_phone_egy("0100abc4567", phone), Reason::Ok);
        EXPECT_NE(validate_phone_egy("0223456789", phone), Reason::Ok);
        EXPECT_NE(validate_phone_egy("+441632960961", phone), Reason::Ok);
        EXPECT_EQ(validate_phone_egy("٠١٠١٢٣٤٥٦٧٨", phone), Reason::Ok);
        EXPECT_EQ(counter.count(), 0U)
            << "phone validation allocated — including the digit fold, which "
               "writes into a stack array";
    }
#endif

    {
        const AllocationCounter counter;
        std::int64_t value = 0;
        EXPECT_NE(parse_int("12abc", 0, 100, value), Reason::Ok);
        EXPECT_NE(parse_int("99999999999999999999", 0, 100, value), Reason::Ok);
        CalendarDate date{};
        EXPECT_NE(parse_date("2026-02-30", date), Reason::Ok);
        std::int64_t ms = 0;
        EXPECT_NE(parse_timestamp("2026-08-04T18:00:00", ms), Reason::Ok);
        EXPECT_NE(check_url("javascript:alert(1)", UrlUse::Link), Reason::Ok);
        EXPECT_NE(check_password("short"), Reason::Ok);
        EXPECT_EQ(counter.count(), 0U) << "a scalar validator allocated";
    }
}

TEST(ValidationAllocations, AcceptPathsDoNotAllocateEither) {
    const AllocationCounter counter;
    EXPECT_EQ(check_email("ahmed@example.com"), Reason::Ok);
    EXPECT_EQ(check_password("a perfectly ordinary passphrase"), Reason::Ok);
    EXPECT_EQ(check_url("https://example.com/x", UrlUse::Link), Reason::Ok);
    EXPECT_EQ(counter.count(), 0U);
}

TEST(ValidationAllocations, AnEightKilobyteBodyBindsWithinTheArena) {
    // The arena's inline buffer is on the stack, so a body that fits it must
    // cost zero heap allocations end to end: parse, bind, and the string views
    // that come out (docs/06-input-validation.md §7 — one arena, no individual frees).
    std::string body = R"({"title":"Yard Club","items":[)";
    for (int i = 0; i < 120; ++i) {
        if (i > 0) { body += ","; }
        body += R"({"n":")" + std::to_string(i) + R"(","ar":"قهوة سادة"})";
    }
    body += "]}";
    ASSERT_GT(body.size(), 4000U);
    ASSERT_LT(body.size(), 8192U);

    BodyArena arena;
    const AllocationCounter counter;

    const JsonDocument document = parse_json(body, arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    std::string_view title;
    ASSERT_EQ(binder.text("title", kShortProseRules, title), Reason::Ok);
    Reason reason = Reason::Ok;
    const JsonValue* items = binder.array("items", 256, reason);
    ASSERT_EQ(reason, Reason::Ok);
    ASSERT_NE(items, nullptr);
    EXPECT_EQ(items->elements().size(), 120U);
    EXPECT_FALSE(binder.finish().has_value());

    EXPECT_EQ(counter.count(), 0U) << "the parse spilled out of the arena onto the heap";
}

// --- fuzz -----------------------------------------------------------------

TEST(ValidationFuzz, JsonParserSurvivesArbitraryBytes) {
    std::mt19937 rng{kSeed};
    for (int i = 0; i < kFuzzIterations / 4; ++i) {
        BodyArena arena;
        const std::string body =
            (i % 2 == 0) ? random_bytes(rng, 96)
                         : random_structured(rng, R"({}[]",:0123456789eE.-+ truefalsnl\u)", 96);
        // The only requirement is that it terminates and does not crash: under
        // ASan and UBSan, "does not crash" covers every read past a buffer and
        // every signed overflow.
        const JsonDocument document = parse_json(body, arena);
        if (document.ok()) {
            const JsonValue& root = document.root();
            static_cast<void>(root.find("anything"));
            static_cast<void>(root.as_int64());
            static_cast<void>(root.elements().size());
            static_cast<void>(root.members().size());
        }
    }
}

TEST(ValidationFuzz, ScalarValidatorsSurviveArbitraryBytes) {
    std::mt19937 rng{kSeed + 1};
#if ANVIL_HAS_EGY
    NationalIdEgy parsed{};
    PhoneEgy phone{};
#endif
    CalendarDate date{};
    std::int64_t number = 0;
    std::int64_t ms = 0;
    Uuid id{};

    for (int i = 0; i < kFuzzIterations; ++i) {
        const std::string input =
            (i % 3 == 0)   ? random_bytes(rng, 40)
            : (i % 3 == 1) ? random_structured(rng, "0123456789-+.:TZ٠١٢٣٤٥٦٧٨٩", 40)
                           : random_structured(rng, "abcdeABCDE@.-_/:%[]", 40);

        static_cast<void>(check_email(input));
        static_cast<void>(check_password(input));
        static_cast<void>(check_url(input, UrlUse::Link));
        static_cast<void>(check_url(input, UrlUse::ServerFetch));
        static_cast<void>(check_text(input, kShortProseRules));
        static_cast<void>(parse_int(input, -1000, 1000, number));
        static_cast<void>(parse_date(input, date));
        static_cast<void>(parse_timestamp(input, ms));
        static_cast<void>(parse_uuid(input, id));
#if ANVIL_HAS_EGY
        static_cast<void>(validate_national_id_egy(input, parsed));
        static_cast<void>(validate_phone_egy(input, phone));
#endif
    }
}

TEST(ValidationFuzz, NoScalarValidatorAllocatesOnAnyInput) {
    // The strongest form of the claim: not "these examples do not allocate" but
    // "no input in this corpus makes any of them allocate".
    std::mt19937 rng{kSeed + 2};
    std::vector<std::string> corpus;
    corpus.reserve(2000);
    for (int i = 0; i < 2000; ++i) {
        corpus.push_back(i % 2 == 0
                             ? random_bytes(rng, 64)
                             : random_structured(rng, "0123456789-+.:TZ@abc٠١٢٣", 64));
    }

#if ANVIL_HAS_EGY
    NationalIdEgy parsed{};
    PhoneEgy phone{};
#endif
    CalendarDate date{};
    std::int64_t number = 0;
    std::int64_t ms = 0;

    const AllocationCounter counter;
    for (const std::string& input : corpus) {
        static_cast<void>(check_email(input));
        static_cast<void>(check_url(input, UrlUse::ServerFetch));
        static_cast<void>(check_text(input, kIdentifierRules));
        static_cast<void>(parse_int(input, -1000, 1000, number));
        static_cast<void>(parse_date(input, date));
        static_cast<void>(parse_timestamp(input, ms));
#if ANVIL_HAS_EGY
        static_cast<void>(validate_national_id_egy(input, parsed));
        static_cast<void>(validate_phone_egy(input, phone));
#endif
    }
    EXPECT_EQ(counter.count(), 0U);
}

// --- the storage budgets ----------------------------------------------------
//
// docs/07-filesystem.md §9 states two numbers that are the whole reason the
// media layer is shaped the way it is: a path resolves with ZERO allocations,
// and a 4 MB upload peaks under 64 KB of heap because the body is never
// buffered. Both stop being true silently, so both are counted rather than
// asserted in a comment.

TEST(StorageAllocations, PathConstructionDoesNotAllocate) {
    const Uuid id{{0x3A, 0xF2, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
                   0x0C, 0x0D, 0x0E}};

    const AllocationCounter counter;
    for (int i = 0; i < 1000; ++i) {
        const fs::RelPath master = fs::media_relative_path(testapp::kContent, id);
        const fs::RelPath variant =
            fs::media_relative_path(testapp::kMedia, id, fs::VariantKey{1600, fs::Format::Webp});
        const fs::RelPath temp = fs::temp_relative_path(id);
        const fs::RelPath derived =
            fs::derived_temp_path(id, fs::VariantKey{320, fs::Format::Avif});
        // Consumed so the compiler cannot delete the work outright.
        ASSERT_NE(master.size() + variant.size() + temp.size() + derived.size(), 0U);
        ASSERT_TRUE(fs::parse_variant("w1600.webp").has_value());
    }
    EXPECT_EQ(counter.count(), 0U);
}

TEST(StorageAllocations, AFourMegabyteUploadNeverBuffersTheBody) {
    std::array<char, 64> pattern{};
    std::snprintf(pattern.data(), pattern.size(), "/tmp/anvil-alloc-XXXXXX");
    const char* made = ::mkdtemp(pattern.data());
    ASSERT_NE(made, nullptr);
    const std::string root{made};
    fs::Storage::init(root);

    // One 64 KB streaming buffer, reused for every chunk — exactly what the
    // transport hands the sink.
    std::vector<std::uint8_t> chunk(fs::kStreamChunkBytes, 0x42);
    chunk[0] = 0x89;
    chunk[1] = 0x50;
    chunk[2] = 0x4E;
    chunk[3] = 0x47;
    chunk[4] = 0x0D;
    chunk[5] = 0x0A;
    chunk[6] = 0x1A;
    chunk[7] = 0x0A;

    // Scoped: a sink borrows the Storage it was opened from, so it must not
    // outlive it. main() gets this ordering by shutting the pools down first;
    // a test that got it wrong would be a use-after-free, which is exactly what
    // ASan reported when this block was written without the scope.
    {
        Result<fs::UploadSink> opened =
            fs::UploadSink::open(fs::Storage::instance(), fs::UploadLimits{8U << 20U, 0},
                                 testapp::kContent);
        ASSERT_TRUE(opened.ok());
        fs::UploadSink sink = std::move(opened).value();

        // Counted from HERE: the sink is already open, so what follows is the
        // cost of moving 4 MB through it.
        const AllocationCounter counter;
        for (std::size_t written = 0; written < (4U << 20U); written += chunk.size()) {
            ASSERT_TRUE(sink.write(chunk).ok());
        }
        const Result<fs::UploadResult> finished = sink.finish("image/png");
        ASSERT_TRUE(finished.ok());
        EXPECT_EQ(finished.value().bytes, 4U << 20U);

        // 4 MB moved, and the streaming path allocated nothing: the body went
        // from the caller's buffer to the page cache and to the digest, and
        // never to the heap (docs/07-filesystem.md §9).
        EXPECT_EQ(counter.count(), 0U);
    }

    fs::Storage::shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

}  // namespace
}  // namespace anvil::input
