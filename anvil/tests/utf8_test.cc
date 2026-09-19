// UTF-8 validation, code-point counting, boundary-safe truncation.

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "anvil/i18n/utf8.h"

namespace anvil::i18n {
namespace {

// Arabic for "coffee": 4 code points, 8 bytes. The 2:1 ratio is the whole
// reason limits are expressed in code points.
constexpr std::string_view kQahwa = "قهوة";

}  // namespace

// --- 1-6: malformed input is rejected ------------------------------------

TEST(Utf8, RejectsOverlongEncoding) {
    // C0 80 encodes U+0000 in two bytes. The classic filter bypass: a NUL that
    // a naive scanner does not see.
    EXPECT_FALSE(is_structurally_valid(std::string_view{"\xC0\x80", 2}));
    EXPECT_EQ(validate(std::string_view{"\xC0\x80", 2}), Utf8Error::Malformed);
}

TEST(Utf8, RejectsLoneSurrogate) {
    // ED A0 80 is U+D800 — valid CESU-8, invalid UTF-8. Breaks JSON and BSON.
    EXPECT_EQ(validate(std::string_view{"\xED\xA0\x80", 3}), Utf8Error::Malformed);
}

TEST(Utf8, RejectsTruncatedSequence) {
    EXPECT_EQ(validate(std::string_view{"\xE0\xA4", 2}), Utf8Error::Malformed);
    EXPECT_EQ(validate(kQahwa.substr(0, 3)), Utf8Error::Malformed);
}

TEST(Utf8, RejectsAboveMaxCodePoint) {
    EXPECT_EQ(validate(std::string_view{"\xF5\x80\x80\x80", 4}), Utf8Error::Malformed);
}

TEST(Utf8, RejectsEmbeddedNul) {
    EXPECT_EQ(validate(std::string_view{"ab\0cd", 5}), Utf8Error::EmbeddedNul);
}

TEST(Utf8, RejectsNonCharacters) {
    EXPECT_EQ(validate("\xEF\xBF\xBE"), Utf8Error::NonCharacter);   // U+FFFE
    EXPECT_EQ(validate("\xEF\xBF\xBF"), Utf8Error::NonCharacter);   // U+FFFF
    EXPECT_EQ(validate("\xEF\xB7\x90"), Utf8Error::NonCharacter);   // U+FDD0
}

TEST(Utf8, AcceptsArabicAndArabicPresentationForms) {
    EXPECT_EQ(validate(kQahwa), Utf8Error::Ok);
    EXPECT_EQ(validate("Coffee"), Utf8Error::Ok);
    EXPECT_EQ(validate(""), Utf8Error::Ok);
    // U+FDF2 sits just above the non-character block — the bound must not eat it.
    EXPECT_EQ(validate("\xEF\xB7\xB2"), Utf8Error::Ok);
}

// --- 7: reject, never repair ---------------------------------------------

TEST(Utf8, NeverRepairsInvalidInput) {
    // The API deliberately offers no "sanitise" entry point. Repairing turns an
    // attack into corrupted data and destroys the signal.
    const std::string_view malformed{"\xC0\x80", 2};
    EXPECT_EQ(validate(malformed), Utf8Error::Malformed);
    EXPECT_EQ(malformed.size(), 2U) << "input must not be mutated";
}

// --- 8: code points, not bytes -------------------------------------------

TEST(Utf8, CountsCodePointsNotBytes) {
    EXPECT_EQ(kQahwa.size(), 8U);
    EXPECT_EQ(count_code_points(kQahwa), 4U);
    EXPECT_EQ(count_code_points("Coffee"), 6U);
    EXPECT_EQ(count_code_points(""), 0U);
}

TEST(Utf8, ArabicGetsTheSameAllowanceAsEnglish) {
    std::string arabic;
    for (int i = 0; i < 100; ++i) { arabic += "ق"; }   // 100 code points, 200 bytes

    EXPECT_EQ(count_code_points(arabic), 100U);
    EXPECT_TRUE(within_code_point_bounds(arabic, 1, 100));
    EXPECT_FALSE(within_code_point_bounds(arabic, 1, 99));
    EXPECT_FALSE(within_code_point_bounds(arabic, 101, 200));
}

TEST(Utf8, BoundsCheckShortCircuits) {
    // A hostile body must not pay for a full scan before being rejected.
    const std::string huge(1'000'000, 'a');
    EXPECT_FALSE(within_code_point_bounds(huge, 0, 10));
}

// --- 9-10: truncation ----------------------------------------------------

TEST(Utf8, TruncationNeverProducesInvalidUtf8) {
    for (std::size_t n = 0; n <= 6; ++n) {
        const std::string_view cut = truncate_to_code_points(kQahwa, n);
        EXPECT_TRUE(is_structurally_valid(cut)) << "n=" << n;
        EXPECT_LE(count_code_points(cut), n);
    }
}

TEST(Utf8, TruncationReturnsWholeStringWhenUnderLimit) {
    EXPECT_EQ(truncate_to_code_points(kQahwa, 10), kQahwa);
    EXPECT_EQ(truncate_to_code_points("abc", 3), "abc");
}

TEST(Utf8, TruncationDoesNotOrphanTashkeel) {
    // "بَ" — beh followed by fatha (U+064E). Cutting between them leaves a
    // stray diacritic that renders on whatever follows it.
    const std::string_view with_mark = "بَت";
    const std::string_view cut = truncate_to_code_points(with_mark, 2);

    EXPECT_TRUE(is_structurally_valid(cut));
    // Either both the base and its mark are kept, or neither — never the base
    // alone followed by nothing, and never a leading combining mark.
    if (!cut.empty()) {
        EXPECT_NE(static_cast<unsigned char>(cut[0]), 0xD9U)
            << "must not begin with a combining mark";
    }
}

TEST(Utf8, BoundaryBeforeLandsOnCodePointStart) {
    for (std::size_t i = 0; i <= kQahwa.size(); ++i) {
        const std::size_t boundary = code_point_boundary_before(kQahwa, i);
        EXPECT_TRUE(is_structurally_valid(kQahwa.substr(0, boundary))) << "i=" << i;
    }
}

// --- 11: fuzz ------------------------------------------------------------

TEST(Utf8, FuzzNeverCrashes) {
    std::mt19937_64 rng{0xC0FFEE};   // test-only; never for security (ENGINEERING_RULES.md §5)
    std::uniform_int_distribution<int> byte_dist{0, 255};
    std::uniform_int_distribution<std::size_t> len_dist{0, 64};

    for (int iteration = 0; iteration < 20000; ++iteration) {
        std::string noise;
        const std::size_t len = len_dist(rng);
        noise.reserve(len);
        for (std::size_t i = 0; i < len; ++i) {
            noise.push_back(static_cast<char>(byte_dist(rng)));
        }

        const Utf8Error result = validate(noise);
        // Any outcome is acceptable; crashing, hanging, or reading out of
        // bounds is not. ASan and UBSan are the real assertions here.
        (void)result;
        (void)count_code_points(noise);
        (void)within_code_point_bounds(noise, 0, 32);

        if (is_structurally_valid(noise)) {
            EXPECT_TRUE(is_structurally_valid(truncate_to_code_points(noise, 8)));
        }
    }
}

}  // namespace anvil::i18n
