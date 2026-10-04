#include "anvil/chat/text.h"

#include <unicode/normalizer2.h>
#include <unicode/stringpiece.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/utext.h>
#include <unicode/utypes.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "anvil/i18n/bidi.h"
#include "anvil/i18n/utf8.h"
#include "anvil/input/html.h"

namespace anvil::chat {
namespace {

using input::Reason;

[[nodiscard]] constexpr std::uint32_t lead(char c, std::uint32_t mask) noexcept {
    return static_cast<unsigned char>(c) & mask;
}
[[nodiscard]] constexpr std::uint32_t cont(char c) noexcept {
    return static_cast<unsigned char>(c) & 0x3FU;
}

// Only ever called after i18n::validate has accepted the whole text, so every
// sequence is complete and well formed.
[[nodiscard]] std::uint32_t decode_valid(std::string_view text, std::size_t& i) noexcept {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte < 0x80U) {
        i += 1;
        return byte;
    }
    if ((byte & 0xE0U) == 0xC0U) {
        const std::uint32_t cp = (lead(text[i], 0x1FU) << 6) | cont(text[i + 1]);
        i += 2;
        return cp;
    }
    if ((byte & 0xF0U) == 0xE0U) {
        const std::uint32_t cp = (lead(text[i], 0x0FU) << 12) | (cont(text[i + 1]) << 6) |
                                 cont(text[i + 2]);
        i += 3;
        return cp;
    }
    const std::uint32_t cp = (lead(text[i], 0x07U) << 18) | (cont(text[i + 1]) << 12) |
                             (cont(text[i + 2]) << 6) | cont(text[i + 3]);
    i += 4;
    return cp;
}

// C0, DEL and C1. C1 is policed by code point, not by byte: U+0085 (NEL) is a
// line break to some renderers and invisible to others, and the rest have no
// meaning in text at all.
[[nodiscard]] constexpr bool is_control(std::uint32_t cp) noexcept {
    return cp < 0x20U || (cp >= 0x7FU && cp <= 0x9FU);
}

[[nodiscard]] constexpr bool is_line_or_paragraph_separator(std::uint32_t cp) noexcept {
    return cp == 0x2028U || cp == 0x2029U;
}

// "Renders as nothing": every White_Space code point, and every
// Default_Ignorable one — zero-width characters, bidi marks, variation
// selectors and the Hangul fillers, which are the usual way to post a message
// that looks empty past a naive whitespace check.
[[nodiscard]] bool is_blank(std::uint32_t cp) noexcept {
    if (cp < 0x80U) { return cp == 0x20U || cp == 0x09U || cp == 0x0AU; }
    const auto c = static_cast<UChar32>(cp);
    return u_isUWhiteSpace(c) != 0 ||
           u_hasBinaryProperty(c, UCHAR_DEFAULT_IGNORABLE_CODE_POINT) != 0;
}

[[nodiscard]] bool is_ascii(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return static_cast<unsigned char>(c) < 0x80U; });
}

// ICU's UTF-8 entry point, not i18n::is_normalized: that one converts to a
// UTF-16 UnicodeString first, which is a heap allocation per call on a path an
// unauthenticated sender drives at their own rate. The NFC instance is an ICU
// singleton, immutable and shared by every thread.
[[nodiscard]] bool is_nfc(std::string_view text) noexcept {
    if (is_ascii(text)) { return true; }
    UErrorCode status = U_ZERO_ERROR;
    const icu::Normalizer2* const nfc = icu::Normalizer2::getNFCInstance(status);
    if (U_FAILURE(status) || nfc == nullptr) { return false; }
    const bool normalized = nfc->isNormalizedUTF8(
        icu::StringPiece{text.data(), static_cast<std::int32_t>(text.size())}, status);
    return U_SUCCESS(status) && normalized;
}

// The character rules shared by every free-text part of a message. The caller
// has already established that `text` is non-empty.
[[nodiscard]] Reason check_prose(std::string_view text, std::uint32_t max_code_points,
                                 bool allow_line_breaks) noexcept {
    // A code point is at most four bytes, so anything longer is over the bound
    // without decoding it; a hostile megabyte costs one comparison.
    if (text.size() > std::size_t{max_code_points} * 4U) { return Reason::TooLong; }
    if (i18n::validate(text) != i18n::Utf8Error::Ok) { return Reason::BadCharset; }
    if (!i18n::within_code_point_bounds(text, 0, max_code_points)) { return Reason::TooLong; }

    bool visible = false;
    for (std::size_t i = 0; i < text.size();) {
        const std::uint32_t cp = decode_valid(text, i);
        if (is_control(cp)) {
            const bool permitted = allow_line_breaks && (cp == 0x0AU || cp == 0x09U);
            if (!permitted) { return Reason::BadCharset; }
        }
        if (is_line_or_paragraph_separator(cp)) { return Reason::BadCharset; }
        if (!visible && !is_blank(cp)) { visible = true; }
    }
    if (!visible) { return Reason::Required; }

    // The house Prose policy, not a second one: overrides and embeddings are
    // refused, isolates are allowed because a mixed Arabic/English sentence
    // needs them.
    if (i18n::check(text, i18n::TextClass::Prose) != i18n::BidiIssue::Ok) {
        return Reason::BadCharset;
    }
    if (!is_nfc(text)) { return Reason::BadFormat; }
    return Reason::Ok;
}

// --- link preview -----------------------------------------------------------

[[nodiscard]] constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool has_https_scheme(std::string_view url) noexcept {
    constexpr std::string_view kScheme = "https://";
    if (url.size() < kScheme.size()) { return false; }
    for (std::size_t i = 0; i < kScheme.size(); ++i) {
        if (lower(url[i]) != kScheme[i]) { return false; }
    }
    return true;
}

[[nodiscard]] Reason check_preview_url(std::string_view url) noexcept {
    if (url.empty()) { return Reason::Required; }
    if (url.size() > kMaxPreviewUrlBytes) { return Reason::TooLong; }
    for (const char c : url) {
        // Space and controls as well as non-ASCII: an encoded URL contains
        // none of them, and a raw one is how a value survives one parser and
        // means something else to the next.
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20U || byte >= 0x7FU) { return Reason::BadCharset; }
    }
    if (!has_https_scheme(url)) { return Reason::NotAllowed; }
    if (!input::is_safe_link_target(url)) { return Reason::NotAllowed; }
    return input::check_url(url, input::UrlUse::Link);
}

// --- reactions --------------------------------------------------------------

// Identifier's refusals, minus ZWJ, which a reaction needs (see text.h).
[[nodiscard]] constexpr bool is_refused_in_reaction(std::uint32_t cp) noexcept {
    return cp == 0x200BU || cp == 0x200CU || cp == 0xFEFFU ||   // zero-width other than ZWJ
           cp == 0x200EU || cp == 0x200FU || cp == 0x061CU ||   // bidi marks
           (cp >= 0x2066U && cp <= 0x2069U);                    // isolates
}

constexpr std::uint32_t kZeroWidthJoiner = 0x200DU;

struct BreakIteratorCloser final {
    void operator()(UBreakIterator* iterator) const noexcept { ubrk_close(iterator); }
};
using BreakIteratorPtr = std::unique_ptr<UBreakIterator, BreakIteratorCloser>;

[[nodiscard]] UText closed_text() noexcept {
    UText text{};
    text.magic = static_cast<std::uint32_t>(UTEXT_MAGIC);
    text.sizeOfStruct = static_cast<std::int32_t>(sizeof(UText));
    return text;
}

// A UText reopened over each reaction in turn. ICU's UTF-8 provider gives a
// UText a side buffer of a few hundred bytes the first time it is opened and
// keeps it for as long as the struct is reused; a fresh UText on the stack
// paid for that buffer on every call, which the allocation test caught.
class ReusableText final {
public:
    ReusableText() noexcept : text_{closed_text()} {}
    ReusableText(const ReusableText&) = delete;
    ReusableText& operator=(const ReusableText&) = delete;
    ReusableText(ReusableText&&) = delete;
    ReusableText& operator=(ReusableText&&) = delete;
    ~ReusableText() { utext_close(&text_); }

    [[nodiscard]] UText* open(std::string_view bytes) noexcept {
        UErrorCode status = U_ZERO_ERROR;
        UText* const opened = utext_openUTF8(
            &text_, bytes.data(), static_cast<std::int64_t>(bytes.size()), &status);
        return U_SUCCESS(status) ? opened : nullptr;
    }

private:
    UText text_;
};

// One character segmenter per thread, opened on first use and reused. Opening
// a break iterator loads and compiles rule data and allocates several
// kilobytes; doing that per reaction would put ICU's allocator on a path anyone
// in a conversation can drive. Neither half is thread-safe, so they cannot be
// shared, and the number of threads that validate is bounded by the pools that
// run them: the standing cost is one iterator and two small UText buffers per
// such thread. If ICU cannot open the iterator, the next call tries again
// rather than leaving the thread unable to accept a reaction for its lifetime.
struct Segmenter final {
    BreakIteratorPtr iterator;
    ReusableText     text;
};

[[nodiscard]] Segmenter* thread_segmenter() noexcept {
    thread_local Segmenter segmenter;
    if (!segmenter.iterator) {
        UErrorCode status = U_ZERO_ERROR;
        BreakIteratorPtr opened{ubrk_open(UBRK_CHARACTER, "", nullptr, 0, &status)};
        if (U_FAILURE(status)) { return nullptr; }
        segmenter.iterator = std::move(opened);
    }
    return &segmenter;
}

// True when `text` is exactly one extended grapheme cluster. `text` is at most
// kMaxReactionBytes long by the time it reaches here. The iterator keeps a
// pointer into `text` after this returns; nothing reads through it before the
// next call on this thread replaces it.
[[nodiscard]] bool is_single_cluster(std::string_view text) noexcept {
    Segmenter* const segmenter = thread_segmenter();
    if (segmenter == nullptr) { return false; }
    UText* const opened = segmenter->text.open(text);
    if (opened == nullptr) { return false; }

    UErrorCode status = U_ZERO_ERROR;
    UBreakIterator* const iterator = segmenter->iterator.get();
    ubrk_setUText(iterator, opened, &status);
    if (U_FAILURE(status)) { return false; }
    const std::int32_t first = ubrk_first(iterator);
    const std::int32_t second = ubrk_next(iterator);
    return first == 0 && second == static_cast<std::int32_t>(text.size());
}

}  // namespace

// --- message body -----------------------------------------------------------

input::Reason validate_message_text(std::string_view text,
                                    std::uint32_t max_code_points) noexcept {
    if (text.empty()) { return Reason::Required; }
    return check_prose(text, std::min(max_code_points, kMaxMessageCodePoints), true);
}

input::Reason validate_line(std::string_view text, std::uint32_t max_code_points) noexcept {
    if (text.empty()) { return Reason::Required; }
    return check_prose(text, max_code_points, false);
}

input::Reason validate_prose(std::string_view text, std::uint32_t max_code_points) noexcept {
    if (text.empty()) { return Reason::Required; }
    return check_prose(text, max_code_points, true);
}

// --- mentions ---------------------------------------------------------------

input::Reason validate_mentions(std::string_view text,
                                std::span<const MentionSpan> mentions) noexcept {
    if (mentions.size() > kMaxMentions) { return Reason::TooLong; }
    if (mentions.empty()) { return Reason::Ok; }

    // 64-bit throughout: offset + length of two attacker-chosen uint32 values
    // wraps in 32 bits, and a wrapped end is a span that passes the bound.
    const std::uint64_t text_code_points = i18n::count_code_points(text);
    std::uint64_t previous_end = 0;
    for (const MentionSpan& mention : mentions) {
        if (is_nil(mention.user)) { return Reason::Required; }
        if (mention.length == 0U) { return Reason::BadFormat; }
        if (mention.offset < previous_end) { return Reason::BadFormat; }
        const std::uint64_t end = std::uint64_t{mention.offset} + mention.length;
        if (end > text_code_points) { return Reason::OutOfRange; }
        previous_end = end;
    }
    return Reason::Ok;
}

// --- link preview -----------------------------------------------------------

std::optional<input::FieldError> validate_link_preview(const LinkPreview& preview) noexcept {
    if (const Reason reason = check_preview_url(preview.url); reason != Reason::Ok) {
        return input::FieldError{kPreviewUrlField, reason};
    }
    if (!preview.title.empty()) {
        const Reason reason = check_prose(preview.title, kMaxPreviewTitleCodePoints, false);
        if (reason != Reason::Ok) { return input::FieldError{kPreviewTitleField, reason}; }
    }
    if (!preview.description.empty()) {
        const Reason reason =
            check_prose(preview.description, kMaxPreviewDescriptionCodePoints, true);
        if (reason != Reason::Ok) { return input::FieldError{kPreviewDescriptionField, reason}; }
    }
    return std::nullopt;
}

// --- reactions --------------------------------------------------------------

input::Reason validate_reaction(std::string_view reaction) noexcept {
    if (reaction.empty()) { return Reason::Required; }
    // Before anything decodes a byte or touches ICU: the cost of an oversized
    // value is this comparison and nothing else.
    if (reaction.size() > kMaxReactionBytes) { return Reason::TooLong; }
    if (i18n::validate(reaction) != i18n::Utf8Error::Ok) { return Reason::BadCharset; }
    if (i18n::count_code_points(reaction) > kMaxReactionCodePoints) { return Reason::TooLong; }

    bool visible = false;
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < reaction.size();) {
        const bool first = i == 0;
        const std::uint32_t cp = decode_valid(reaction, i);
        if (is_control(cp) || is_line_or_paragraph_separator(cp) || is_refused_in_reaction(cp)) {
            return Reason::BadCharset;
        }
        if (cp == kZeroWidthJoiner && first) { return Reason::BadCharset; }
        if (!visible && !is_blank(cp)) { visible = true; }
        previous = cp;
    }
    if (previous == kZeroWidthJoiner) { return Reason::BadCharset; }
    if (!visible) { return Reason::Required; }

    if (i18n::check(reaction, i18n::TextClass::Prose) != i18n::BidiIssue::Ok) {
        return Reason::BadCharset;
    }
    if (!is_nfc(reaction)) { return Reason::BadFormat; }
    if (!is_single_cluster(reaction)) { return Reason::BadFormat; }
    return Reason::Ok;
}

}  // namespace anvil::chat
