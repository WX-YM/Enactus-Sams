#include "anvil/i18n/normalize.h"

#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uspoof.h>
#include <unicode/utypes.h>

#include <cstdint>
#include <memory>

namespace anvil::i18n {
namespace {

// ICU's Normalizer2 instances are singletons owned by ICU, immutable, and
// thread-safe. Fetching one per call is a lookup, not a construction, but
// caching the pointer avoids even that on the hot path.
const icu::Normalizer2* normalizer_for(NormalizeMode mode) {
    UErrorCode status = U_ZERO_ERROR;
    switch (mode) {
        case NormalizeMode::Nfc:
            return icu::Normalizer2::getNFCInstance(status);
        case NormalizeMode::Nfkc:
            return icu::Normalizer2::getNFKCInstance(status);
        case NormalizeMode::NfkcCaseFold:
            return icu::Normalizer2::getNFKCCasefoldInstance(status);
    }
    return nullptr;
}

[[nodiscard]] constexpr bool is_tashkeel(UChar32 cp) noexcept {
    // Fathatan through sukun: the vowel and gemination marks. Meaningful for
    // display and for Quranic text, noise for matching.
    // UChar32 is a signed int, so the bounds are signed literals — an unsigned
    // comparison here is a -Wsign-compare warning and, with a negative cp,
    // silently wrong.
    return cp >= 0x064B && cp <= 0x0652;
}

[[nodiscard]] constexpr UChar32 fold_arabic_letter(UChar32 cp) noexcept {
    switch (cp) {
        // Alef variants: users type the bare form regardless of the hamza.
        case 0x0623:  // أ  alef with hamza above
        case 0x0625:  // إ  alef with hamza below
        case 0x0622:  // آ  alef with madda
        case 0x0671:  // ٱ  alef wasla
            return 0x0627;  // ا
        case 0x0629:        // ة  teh marbuta -> ه  heh
            return 0x0647;
        case 0x0649:  // ى  alef maksura -> ي  yeh
            return 0x064A;
        case 0x0624:  // ؤ  waw with hamza -> و  waw
            return 0x0648;
        case 0x0626:  // ئ  yeh with hamza -> ي  yeh
            return 0x064A;
        default:
            return cp;
    }
}

std::string to_utf8(const icu::UnicodeString& text) {
    std::string out;
    text.toUTF8String(out);
    return out;
}

}  // namespace

std::optional<std::string> normalize(std::string_view text, NormalizeMode mode) {
    if (text.empty()) { return std::string{}; }

    UErrorCode status = U_ZERO_ERROR;
    const icu::Normalizer2* normalizer = normalizer_for(mode);
    if (normalizer == nullptr || U_FAILURE(status)) { return std::nullopt; }

    const icu::UnicodeString input =
        icu::UnicodeString::fromUTF8(icu::StringPiece{text.data(), static_cast<std::int32_t>(text.size())});

    const icu::UnicodeString output = normalizer->normalize(input, status);
    if (U_FAILURE(status)) { return std::nullopt; }

    return to_utf8(output);
}

bool is_normalized(std::string_view text, NormalizeMode mode) {
    if (text.empty()) { return true; }

    UErrorCode status = U_ZERO_ERROR;
    const icu::Normalizer2* normalizer = normalizer_for(mode);
    if (normalizer == nullptr || U_FAILURE(status)) { return false; }

    const icu::UnicodeString input =
        icu::UnicodeString::fromUTF8(icu::StringPiece{text.data(), static_cast<std::int32_t>(text.size())});

    const UBool result = normalizer->isNormalized(input, status);
    return U_SUCCESS(status) && result != 0;
}

std::string fold_for_search(std::string_view text) {
    const std::optional<std::string> nfc = normalize(text, NormalizeMode::Nfc);
    if (!nfc.has_value()) { return std::string{text}; }

    const icu::UnicodeString input = icu::UnicodeString::fromUTF8(*nfc);

    icu::UnicodeString folded;
    // Folding only ever removes or replaces one-for-one, so the input length is
    // an exact upper bound.
    folded.getBuffer(input.length());
    folded.releaseBuffer(0);

    for (std::int32_t i = 0; i < input.length();) {
        const UChar32 cp = input.char32At(i);
        i += U16_LENGTH(cp);

        // Tatweel is a pure presentation stretch with no semantic content;
        // tashkeel are vowel marks the user may or may not have typed.
        if (cp == 0x0640 || is_tashkeel(cp)) { continue; }

        folded.append(fold_arabic_letter(cp));
    }

    // Case-fold last so Latin text in a bilingual field matches too.
    return to_utf8(folded.foldCase());
}

std::string confusable_skeleton(std::string_view text) {
    UErrorCode status = U_ZERO_ERROR;

    // USpoofChecker owns the confusable mapping tables. Constructing one is
    // relatively expensive, so this is deliberately not on a request hot path —
    // it runs at signup and at username change only.
    const std::unique_ptr<USpoofChecker, decltype(&uspoof_close)> checker{
        uspoof_open(&status), &uspoof_close};

    if (U_FAILURE(status) || !checker) { return std::string{text}; }

    const icu::UnicodeString input =
        icu::UnicodeString::fromUTF8(icu::StringPiece{text.data(), static_cast<std::int32_t>(text.size())});

    icu::UnicodeString skeleton;
    uspoof_getSkeletonUnicodeString(checker.get(), 0, input, skeleton, &status);
    if (U_FAILURE(status)) { return std::string{text}; }

    return to_utf8(skeleton);
}

}  // namespace anvil::i18n
