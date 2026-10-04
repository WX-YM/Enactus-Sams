#pragma once

// The reference application's message kinds (docs/22-chat.md §4.4): one, a
// poll, as the worked example of a card binder. The binder is ordinary input
// binding, the same ObjectBinder every request body goes through, and it
// answers the canonical JSON anvil stores and hands back.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "anvil/chat/card_spec.h"
#include "anvil/http/json_writer.h"
#include "anvil/input/fields.h"
#include "anvil/input/schema.h"

namespace testapp {

inline constexpr std::string_view kPollQuestion = "question";
inline constexpr std::string_view kPollOptions = "options";

inline constexpr anvil::input::TextRules kPollText{1, 200, anvil::i18n::TextClass::Prose, false};

[[nodiscard]] inline anvil::Result<std::string> bind_poll(const anvil::input::JsonValue& body) {
    namespace input = anvil::input;
    const auto refuse = [](std::string_view field, input::Reason reason) {
        return anvil::Failure{anvil::ErrorCode::ValidationFailed, field,
                              static_cast<std::uint16_t>(reason)};
    };
    input::ObjectBinder binder{body};
    std::string_view question;
    if (const input::Reason r = binder.text(kPollQuestion, kPollText, question); !input::is_ok(r)) {
        return refuse(kPollQuestion, r);
    }
    input::Reason reason = input::Reason::Ok;
    const input::JsonValue* options = binder.array(kPollOptions, 12, reason);
    if (options == nullptr || !input::is_ok(reason)) { return refuse(kPollOptions, reason); }
    if (options->elements().size() < 2) { return refuse(kPollOptions, input::Reason::TooShort); }
    if (const auto unknown = binder.finish()) { return refuse(unknown->field, unknown->reason); }

    std::string out;
    out.reserve(512);
    out += '{';
    anvil::http::append_json_key(out, kPollQuestion);
    anvil::http::append_json_string(out, question);
    out += ',';
    anvil::http::append_json_key(out, kPollOptions);
    out += '[';
    bool first = true;
    for (const input::JsonValue& option : options->elements()) {
        const std::optional<std::string_view> text = option.as_string();
        if (!text.has_value() ||
            !input::is_ok(input::check_text(*text, kPollText))) {
            return refuse(kPollOptions, input::Reason::BadFormat);
        }
        if (!first) { out += ','; }
        first = false;
        anvil::http::append_json_string(out, *text);
    }
    out += "]}";
    return out;
}

inline constexpr std::array<anvil::chat::CardSpec, 1> kChatCards{{
    {"poll", &bind_poll, 0},
}};

static_assert(anvil::chat::cards_are_well_formed(kChatCards));

}  // namespace testapp
