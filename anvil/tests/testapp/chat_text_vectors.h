#pragma once

// The chat text validators' cases as vectors (docs/22-chat.md §4.3, §4.6): a
// message body, mention spans, a link preview and a reaction, each with what
// anvil answers — accepted, or the field and the reason.
//
// tests/chat_text_test.cc holds anvil's validators to every case here, and
// testapp_emit_chat_text prints them as the fixture hammer's composer
// validator is held to, so a client refuses exactly what the server refuses
// and accepts exactly what it accepts. Texts are built in C++ rather than
// spelled out, because a bound of 4 096 Arabic letters is 8 KiB a reader
// should not have to count.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "anvil/chat/text.h"
#include "anvil/core/types.h"
#include "anvil/input/fields.h"

namespace testapp::text_vectors {

namespace chat = anvil::chat;
using anvil::input::Reason;

// The fields the routes name these refusals by (kBodyField, kMentionsField and
// kReactionField in anvil/chat/service.h, which is past the foundation this
// header stays inside; chat_routes_listener_test.cc sees them on the wire). A
// preview names its own, from chat/text.h.
inline constexpr std::string_view kBodyFieldName = "body";
inline constexpr std::string_view kMentionsFieldName = "mentions";
inline constexpr std::string_view kReactionFieldName = "reaction";

enum class Validator : std::uint8_t { Message, Mentions, Preview, Reaction };

[[nodiscard]] constexpr std::string_view validator_name(Validator v) noexcept {
    switch (v) {
        case Validator::Message: return "message";
        case Validator::Mentions: return "mentions";
        case Validator::Preview: return "preview";
        case Validator::Reaction: return "reaction";
    }
    return "message";
}

struct TextCase final {
    std::string                    name;
    // The body, the mentions' text, the reaction, or the preview's URL.
    std::string                    text;
    std::string                    title;        // preview only
    std::string                    description;  // preview only
    std::vector<chat::MentionSpan> mentions;     // mentions only
    std::string_view               field;        // what a refusal names; empty when accepted
    std::uint32_t                  max;          // message only: the kind's bound
    Reason                         reason;       // Ok when accepted
    Validator                      validator;
};

[[nodiscard]] inline std::string repeat(std::string_view unit, std::size_t times) {
    std::string out;
    out.reserve(unit.size() * times);
    for (std::size_t i = 0; i < times; ++i) { out.append(unit); }
    return out;
}

// A user id whose last byte is `tag`, the rest zero: a fixture's ids are
// readable, and only nil is special to a validator.
[[nodiscard]] inline anvil::Uuid user(std::uint8_t tag) noexcept {
    anvil::Uuid id{};
    id[15] = tag;
    return id;
}

// What validate() would have the client answer, field and reason.
[[nodiscard]] inline std::vector<TextCase> cases() {
    std::vector<TextCase> out;
    constexpr std::uint32_t kMax = chat::kMaxMessageCodePoints;
    constexpr std::string_view kBeh = "ب";  // one code point, two bytes
    const auto message = [&](std::string name, std::string text, Reason reason,
                             std::uint32_t max = chat::kMaxMessageCodePoints) {
        out.push_back(TextCase{std::move(name), std::move(text), {}, {}, {},
                               reason == Reason::Ok ? std::string_view{} : kBodyFieldName,
                               max, reason, Validator::Message});
    };
    const auto mentions = [&](std::string name, std::string text,
                              std::vector<chat::MentionSpan> spans, Reason reason) {
        out.push_back(TextCase{std::move(name), std::move(text), {}, {}, std::move(spans),
                               reason == Reason::Ok ? std::string_view{} : kMentionsFieldName,
                               0, reason, Validator::Mentions});
    };
    const auto preview = [&](std::string name, std::string url, std::string title,
                             std::string description, std::string_view field, Reason reason) {
        out.push_back(TextCase{std::move(name), std::move(url), std::move(title),
                               std::move(description), {}, field, 0, reason, Validator::Preview});
    };
    const auto reaction = [&](std::string name, std::string text, Reason reason) {
        out.push_back(TextCase{std::move(name), std::move(text), {}, {}, {},
                               reason == Reason::Ok ? std::string_view{} : kReactionFieldName,
                               0, reason, Validator::Reaction});
    };

    // --- message body ---
    message("latin", "hello", Reason::Ok);
    message("arabic", "مرحبا", Reason::Ok);
    message("newlines_and_tab", "line one\nline two\n\tindented", Reason::Ok);
    message("emoji", "\U0001F44D great", Reason::Ok);
    message("arabic_at_bound", repeat(kBeh, kMax), Reason::Ok);
    message("arabic_over_bound", repeat(kBeh, kMax + 1U), Reason::TooLong);
    message("kind_bound_at", repeat("a", 10), Reason::Ok, 10);
    message("kind_bound_over", repeat("a", 11), Reason::TooLong, 10);
    message("kind_bound_clamped_to_anvils", repeat("a", kMax + 1U), Reason::TooLong, 1'000'000);
    message("kind_bound_zero", "a", Reason::TooLong, 0);
    message("empty", "", Reason::Required);
    message("blank_whitespace", " \n\t ", Reason::Required);
    message("blank_nbsp_ideographic", " 　", Reason::Required);
    message("blank_zwsp_hangul_filler", "​ㅤ", Reason::Required);
    message("padded_letter", " x ", Reason::Ok);
    message("c0_control", "a\x01" "b", Reason::BadCharset);
    message("escape_sequence", "a\x1B[2Jb", Reason::BadCharset);
    message("delete", "a\x7F", Reason::BadCharset);
    message("c1_next_line", "a\u0085b", Reason::BadCharset);
    message("c1_csi", "a\u009Bb", Reason::BadCharset);
    message("carriage_return", "a\rb", Reason::BadCharset);
    message("crlf", "a\r\nb", Reason::BadCharset);
    message("line_separator", "a b", Reason::BadCharset);
    message("paragraph_separator", "a b", Reason::BadCharset);
    message("truncated_utf8", "\xC3", Reason::BadCharset);
    message("overlong_utf8", "\xC0\xAF", Reason::BadCharset);
    message("surrogate_utf8", "\xED\xA0\x80", Reason::BadCharset);
    message("embedded_nul", std::string{"a\0b", 3}, Reason::BadCharset);
    message("noncharacter", "￾", Reason::BadCharset);
    message("rlo_override", "invoice\xE2\x80\xAE" "fdp.exe", Reason::BadCharset);
    message("lre_embedding", "a\xE2\x80\xAA" "b", Reason::BadCharset);
    message("isolates_allowed",
            "the word \xE2\x81\xA7مرحبا\xE2\x81\xA9 here", Reason::Ok);
    message("nfc_composed", "café", Reason::Ok);
    message("nfd_decomposed", "café", Reason::BadFormat);
    message("composition_exclusion", "क़", Reason::BadFormat);
    message("arabic_tashkeel_nfc", "بَاب", Reason::Ok);

    // --- mentions: code points into the text, sorted, non-overlapping ---
    const std::string greeting =
        "مرحبا أحمد و سارة";
    mentions("arabic_spans", greeting, {{user(1), 6, 4}, {user(2), 13, 4}}, Reason::Ok);
    mentions("none", greeting, {}, Reason::Ok);
    mentions("same_person_twice", "ab cd", {{user(1), 0, 2}, {user(1), 3, 2}}, Reason::Ok);
    mentions("adjacent", "abcd", {{user(1), 0, 2}, {user(2), 2, 2}}, Reason::Ok);
    mentions("at_end", "abcde", {{user(1), 3, 2}}, Reason::Ok);
    mentions("past_end", "abcde", {{user(1), 4, 2}}, Reason::OutOfRange);
    mentions("past_end_in_code_points", "بب", {{user(1), 0, 3}}, Reason::OutOfRange);
    mentions("wrapping_end", "abc", {{user(1), 0xFFFF'FFFFU, 2}}, Reason::OutOfRange);
    mentions("overlapping", "abcdef", {{user(1), 0, 3}, {user(2), 2, 2}}, Reason::BadFormat);
    mentions("unsorted", "abcdef", {{user(1), 3, 2}, {user(2), 0, 2}}, Reason::BadFormat);
    mentions("empty_span", "abc", {{user(1), 1, 0}}, Reason::BadFormat);
    mentions("nil_user", "abc", {{anvil::kNilUuid, 0, 1}}, Reason::Required);
    {
        std::vector<chat::MentionSpan> most;
        for (std::uint32_t i = 0; i < chat::kMaxMentions; ++i) { most.push_back({user(1), i, 1}); }
        mentions("thirty_two", repeat("a", 64), most, Reason::Ok);
        most.push_back({user(1), 40, 1});
        mentions("thirty_three", repeat("a", 64), most, Reason::TooLong);
    }

    // --- link preview ---
    const std::string_view url = chat::kPreviewUrlField;
    const std::string_view title = chat::kPreviewTitleField;
    const std::string_view description = chat::kPreviewDescriptionField;
    preview("https_with_title_and_description", "https://example.com/a?b=c#d", "A title",
            "First line.\nSecond line.", {}, Reason::Ok);
    preview("scheme_case_insensitive", "HTTPS://Example.COM/", "", "", {}, Reason::Ok);
    preview("punycode_host", "https://xn--mgbh0fb.example/", "", "", {}, Reason::Ok);
    preview("empty_url", "", "", "", url, Reason::Required);
    preview("javascript", "javascript:alert(1)", "", "", url, Reason::NotAllowed);
    preview("plain_http", "http://example.com/", "", "", url, Reason::NotAllowed);
    preview("relative", "/relative/page", "", "", url, Reason::NotAllowed);
    preview("fragment_only", "#fragment", "", "", url, Reason::NotAllowed);
    preview("mailto", "mailto:a@example.com", "", "", url, Reason::NotAllowed);
    preview("data", "data:text/html,x", "", "", url, Reason::NotAllowed);
    preview("userinfo", "https://bank.example@evil.example/", "", "", url, Reason::NotAllowed);
    preview("ip_literal", "https://192.0.2.1/", "", "", url, Reason::NotAllowed);
    preview("single_label_host", "https://intranet/", "", "", url, Reason::BadFormat);
    preview("cyrillic_homoglyph", "https://pаypal.example/", "", "", url, Reason::BadCharset);
    preview("space_in_path", "https://example.com/a b", "", "", url, Reason::BadCharset);
    preview("header_injection", "https://example.com/\r\nSet-Cookie:x", "", "", url,
            Reason::BadCharset);
    {
        const std::string at_bound =
            "https://example.com/" + repeat("a", chat::kMaxPreviewUrlBytes - 20U);
        preview("url_at_bound", at_bound, "", "", {}, Reason::Ok);
        preview("url_over_bound", at_bound + "a", "", "", url, Reason::TooLong);
    }
    constexpr std::string_view kUrl = "https://example.com/";
    preview("title_at_bound", std::string{kUrl}, repeat(kBeh, chat::kMaxPreviewTitleCodePoints),
            "", {}, Reason::Ok);
    preview("title_over_bound", std::string{kUrl},
            repeat(kBeh, chat::kMaxPreviewTitleCodePoints + 1U), "", title, Reason::TooLong);
    preview("title_two_lines", std::string{kUrl}, "two\nlines", "", title, Reason::BadCharset);
    preview("title_tab", std::string{kUrl}, "a\ttab", "", title, Reason::BadCharset);
    preview("title_override", std::string{kUrl}, "gnp.\xE2\x80\xAE" "exe", "", title,
            Reason::BadCharset);
    preview("title_blank", std::string{kUrl}, "   ", "", title, Reason::Required);
    preview("description_at_bound", std::string{kUrl}, "",
            repeat(kBeh, chat::kMaxPreviewDescriptionCodePoints), {}, Reason::Ok);
    preview("description_over_bound", std::string{kUrl}, "",
            repeat(kBeh, chat::kMaxPreviewDescriptionCodePoints + 1U), description,
            Reason::TooLong);
    preview("description_control", std::string{kUrl}, "", "a\x01", description,
            Reason::BadCharset);
    preview("description_nfd", std::string{kUrl}, "", "café", description,
            Reason::BadFormat);

    // --- reactions: one grapheme cluster, at most eight code points ---
    reaction("thumbs_up", "\U0001F44D", Reason::Ok);
    reaction("skin_tone", "\U0001F44D\U0001F3FD", Reason::Ok);
    reaction("emoji_presentation", "❤️", Reason::Ok);
    reaction("keycap", "1️⃣", Reason::Ok);
    reaction("flag", "\U0001F1EA\U0001F1EC", Reason::Ok);
    reaction("letter", "a", Reason::Ok);
    reaction("letter_with_tashkeel", "بَ", Reason::Ok);
    reaction("zwj_family", "\U0001F468‍\U0001F469‍\U0001F467‍\U0001F466",
             Reason::Ok);
    reaction("two_clusters", "\U0001F44D\U0001F44D", Reason::BadFormat);
    reaction("two_letters", "ab", Reason::BadFormat);
    reaction("flag_and_a_stray", "\U0001F1EA\U0001F1EC\U0001F1EA", Reason::BadFormat);
    reaction("zwj_between_letters", "a‍b", Reason::BadFormat);
    reaction("nine_code_points", "a" + repeat("́", 8), Reason::TooLong);
    reaction("over_byte_bound", repeat("a", chat::kMaxReactionBytes + 1U), Reason::TooLong);
    reaction("empty", "", Reason::Required);
    reaction("space", " ", Reason::Required);
    reaction("hangul_filler", "ㅤ", Reason::Required);
    reaction("control", "\x01", Reason::BadCharset);
    reaction("newline", "\n", Reason::BadCharset);
    reaction("override", "\xE2\x80\xAE\U0001F44D", Reason::BadCharset);
    reaction("isolate", "\xE2\x81\xA7\U0001F44D", Reason::BadCharset);
    reaction("lrm", "‎", Reason::BadCharset);
    reaction("zwsp", "​", Reason::BadCharset);
    reaction("trailing_zwnj", "a‌", Reason::BadCharset);
    reaction("lone_zwj", "‍", Reason::BadCharset);
    reaction("trailing_zwj", "\U0001F44D‍", Reason::BadCharset);
    reaction("leading_zwj", "‍\U0001F44D", Reason::BadCharset);
    reaction("truncated_utf8", "\xF0\x9F\x91", Reason::BadCharset);
    reaction("surrogate_utf8", "\xED\xA0\x80", Reason::BadCharset);
    reaction("nfd", "é", Reason::BadFormat);
    reaction("nfc", "é", Reason::Ok);
    return out;
}

// What anvil answers for one case: the field and reason, or Ok.
[[nodiscard]] inline std::pair<std::string_view, Reason> validate(const TextCase& c) {
    switch (c.validator) {
        case Validator::Message: {
            const Reason r = chat::validate_message_text(c.text, c.max);
            return {r == Reason::Ok ? std::string_view{} : kBodyFieldName, r};
        }
        case Validator::Mentions: {
            const Reason r = chat::validate_mentions(c.text, c.mentions);
            return {r == Reason::Ok ? std::string_view{} : kMentionsFieldName, r};
        }
        case Validator::Preview: {
            const auto refused =
                chat::validate_link_preview(chat::LinkPreview{c.text, c.title, c.description});
            if (!refused.has_value()) { return {std::string_view{}, Reason::Ok}; }
            return {refused->field, refused->reason};
        }
        case Validator::Reaction: {
            const Reason r = chat::validate_reaction(c.text);
            return {r == Reason::Ok ? std::string_view{} : kReactionFieldName, r};
        }
    }
    return {std::string_view{}, Reason::Ok};
}

}  // namespace testapp::text_vectors
