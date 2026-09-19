// Normalisation, digit folding, bidi policy.

#include <gtest/gtest.h>

#include <array>
#include <string>

#include "anvil/i18n/bidi.h"
#include "anvil/i18n/digits.h"
#include "anvil/i18n/normalize.h"

namespace anvil::i18n {

// --- 1-3: normalisation modes --------------------------------------------

TEST(Normalize, NfcMakesDecomposedAndComposedEqual) {
    // U+0623 (alef with hamza above) vs U+0627 U+0654 (alef + combining hamza).
    const std::string composed = "أ";
    const std::string decomposed = "أ";
    ASSERT_NE(composed, decomposed);

    const auto a = normalize(composed, NormalizeMode::Nfc);
    const auto b = normalize(decomposed, NormalizeMode::Nfc);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*a, *b);
}

TEST(Normalize, NfkcCollapsesArabicPresentationForms) {
    // Presentation forms are legacy compatibility code points. NFC leaves them
    // alone, so without NFKC these register as two distinct usernames.
    const std::string presentation = "ﻙﻤﻠ";
    const auto folded = normalize(presentation, NormalizeMode::Nfkc);
    ASSERT_TRUE(folded.has_value());
    EXPECT_NE(*folded, presentation) << "NFKC must map presentation forms to base letters";

    const auto nfc_only = normalize(presentation, NormalizeMode::Nfc);
    ASSERT_TRUE(nfc_only.has_value());
    EXPECT_EQ(*nfc_only, presentation) << "NFC alone is not sufficient — this is why";
}

TEST(Normalize, PasswordPathIsNfcAndNothingElse) {
    // Never trimmed, never case-folded: a password is bytes the user chose.
    const std::string password = "  Mot De Passe مرحبا  ";
    const auto result = normalize(password, NormalizeMode::Nfc);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->front(), ' ') << "leading whitespace must survive";
    EXPECT_EQ(result->back(), ' ') << "trailing whitespace must survive";
    EXPECT_NE(result->find('M'), std::string::npos) << "case must survive";
}

TEST(Normalize, IsIdempotent) {
    for (const NormalizeMode mode :
         {NormalizeMode::Nfc, NormalizeMode::Nfkc, NormalizeMode::NfkcCaseFold}) {
        for (const std::string_view input : {"قهوة", "Coffee", "أ", "", "  a  "}) {
            const auto once = normalize(input, mode);
            ASSERT_TRUE(once.has_value());
            const auto twice = normalize(*once, mode);
            ASSERT_TRUE(twice.has_value());
            EXPECT_EQ(*once, *twice) << "input=" << input;
        }
    }
}

TEST(Normalize, IsNormalizedAgreesWithNormalize) {
    const std::string decomposed = "أ";
    EXPECT_FALSE(is_normalized(decomposed, NormalizeMode::Nfc));

    const auto composed = normalize(decomposed, NormalizeMode::Nfc);
    ASSERT_TRUE(composed.has_value());
    EXPECT_TRUE(is_normalized(*composed, NormalizeMode::Nfc));
}

// --- 4-5: digit folding ---------------------------------------------------

TEST(Digits, FoldsArabicIndicToAscii) {
    // The exact input an Egyptian user produces on an Arabic keyboard.
    EXPECT_EQ(fold_digits("٢٩٨٠١٠١٢٣٤٥٦٧"), "2980101234567");
    EXPECT_EQ(fold_digits("٠١٢٣٤٥٦٧٨٩"), "0123456789");
}

TEST(Digits, FoldsExtendedArabicIndicToAscii) {
    // Persian and Urdu keyboards produce U+06F0..U+06F9 instead.
    EXPECT_EQ(fold_digits("۰۱۲۳۴۵۶۷۸۹"), "0123456789");
}

TEST(Digits, LeavesNonDigitArabicUntouched) {
    EXPECT_EQ(fold_digits("قهوة"), "قهوة");
    EXPECT_EQ(fold_digits("قهوة ٣"), "قهوة 3");
}

TEST(Digits, AsciiInputIsUnchanged) {
    EXPECT_EQ(fold_digits("2980101234567"), "2980101234567");
    EXPECT_FALSE(has_non_ascii_digits("2980101234567"));
    EXPECT_TRUE(has_non_ascii_digits("٢980101234567"));
}

TEST(Digits, FoldIntoFixedBufferIsAllocationFree) {
    std::array<char, 14> buffer{};
    const auto written = fold_digits_into("٢٩٨٠١٠١٢٣٤٥٦٧", buffer);
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, 13U);
    EXPECT_EQ(std::string_view(buffer.data(), *written), "2980101234567");
}

TEST(Digits, FoldIntoRejectsOverflowRatherThanTruncating) {
    std::array<char, 4> tiny{};
    EXPECT_FALSE(fold_digits_into("٠١٢٣٤٥٦٧٨٩", tiny).has_value());
}

// --- 6-8: bidi policy -----------------------------------------------------

TEST(Bidi, RejectsRightToLeftOverrideEverywhere) {
    // exe.<U+202E>gnp.evil renders as evil.png in a listing.
    //
    // Written as explicit bytes rather than a literal override character: a
    // real U+202E in this file would reverse how the source itself renders in
    // an editor, which is precisely the attack under test.
    const std::string spoofed = "exe.\xE2\x80\xAE" "gnp.evil";
    EXPECT_EQ(check(spoofed, TextClass::Identifier), BidiIssue::Override);
    EXPECT_EQ(check(spoofed, TextClass::Prose), BidiIssue::Override);
}

TEST(Bidi, AllowsIsolatesInProseButNotIdentifiers) {
    // Isolates are genuinely needed to render mixed EN/AR sentences correctly.
    const std::string mixed = "The item ⁦Yard Club⁩ قهوة";
    EXPECT_EQ(check(mixed, TextClass::Prose), BidiIssue::Ok);
    EXPECT_EQ(check(mixed, TextClass::Identifier), BidiIssue::Override);
}

TEST(Bidi, RejectsZeroWidthInIdentifiers) {
    // Two visually identical usernames that compare unequal.
    EXPECT_EQ(check("ahmed​youssef", TextClass::Identifier), BidiIssue::ZeroWidth);
    EXPECT_EQ(check("ahmed﻿", TextClass::Identifier), BidiIssue::ZeroWidth);
    EXPECT_EQ(check("قهوة‍", TextClass::Prose), BidiIssue::Ok);
}

TEST(Bidi, RejectsDirectionalMarksInIdentifiers) {
    EXPECT_EQ(check("ahmed‏", TextClass::Identifier), BidiIssue::Mark);
    EXPECT_EQ(check("ahmed؜", TextClass::Identifier), BidiIssue::Mark);
}

TEST(Bidi, AcceptsOrdinaryArabicAndEnglish) {
    EXPECT_TRUE(is_acceptable("قهوة", TextClass::Identifier));
    EXPECT_TRUE(is_acceptable("ahmed-youssef", TextClass::Identifier));
    EXPECT_TRUE(is_acceptable("مرحباً بكم في يارد كلوب", TextClass::Prose));
    EXPECT_TRUE(is_acceptable("", TextClass::Identifier));
}

// --- 9-10: search folding -------------------------------------------------

TEST(Normalize, SearchFoldingCollapsesArabicLetterVariants) {
    // Users type these interchangeably; searching for one must find the other.
    EXPECT_EQ(fold_for_search("قهوة"), fold_for_search("قهوه"));
    EXPECT_EQ(fold_for_search("أحمد"), fold_for_search("احمد"));
    EXPECT_EQ(fold_for_search("إبراهيم"), fold_for_search("ابراهيم"));
    EXPECT_EQ(fold_for_search("مصطفى"), fold_for_search("مصطفي"));
}

TEST(Normalize, SearchFoldingStripsTashkeelAndTatweel) {
    EXPECT_EQ(fold_for_search("مَرْحَبًا"), fold_for_search("مرحبا"));
    EXPECT_EQ(fold_for_search("قهــــوة"), fold_for_search("قهوة"));
}

TEST(Normalize, SearchFoldingDoesNotMutateDisplayText) {
    // The folded form goes in its own field; users must see their own tashkeel.
    const std::string display = "مَرْحَبًا";
    const std::string folded = fold_for_search(display);
    EXPECT_NE(folded, display);
    EXPECT_EQ(display, "مَرْحَبًا") << "input must be untouched";
}

TEST(Normalize, SearchFoldingIsCaseInsensitiveForLatin) {
    EXPECT_EQ(fold_for_search("Coffee"), fold_for_search("COFFEE"));
}

// --- 11: confusables ------------------------------------------------------

TEST(Normalize, ConfusableSkeletonCollidesHomoglyphs) {
    // Cyrillic 'а' (U+0430) against Latin 'a'. Without this check a homoglyph
    // username can be registered alongside a real one.
    const std::string latin = "admin";
    const std::string cyrillic = "аdmin";
    ASSERT_NE(latin, cyrillic);
    EXPECT_EQ(confusable_skeleton(latin), confusable_skeleton(cyrillic));
}

TEST(Normalize, ConfusableSkeletonKeepsDistinctNamesDistinct) {
    EXPECT_NE(confusable_skeleton("ahmed"), confusable_skeleton("youssef"));
}

}  // namespace anvil::i18n
