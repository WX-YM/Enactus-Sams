// Prints the chat text validators' cases as the JSON fixture hammer's
// composer validator is held to (docs/22-chat.md §4.3, §4.6).
//
// The cases are tests/testapp/chat_text_vectors.h, which tests/chat_text_test.cc
// holds anvil's validators to. Every text is lowercase hex of its UTF-8 bytes,
// because some cases are not UTF-8 at all and JSON cannot carry those as
// strings. Mention offsets and lengths are code points. `expect` is
// `{"accepted":true}` or `{"field","reason"}`, the reason as the wire spells it.
//
//     build/<preset>/tests/testapp_emit_chat_text > <hammer>/tests/chat/text_vectors.json

#include <cstdio>
#include <string>
#include <string_view>

#include "anvil/http/errors.h"
#include "anvil/http/json_writer.h"

#include "chat_text_vectors.h"

namespace {

namespace json = anvil::http;
namespace tv = testapp::text_vectors;

void append_hex(std::string& out, std::string_view key, std::string_view bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    json::append_json_key(out, key);
    out += '"';
    for (const char c : bytes) {
        const auto b = static_cast<std::uint8_t>(c);
        out += digits[b >> 4U];
        out += digits[b & 0xFU];
    }
    out += '"';
}

}  // namespace

int main() {
    std::string out;
    out.reserve(1U << 20U);
    out += '{';
    json::append_json_key(out, "format");
    json::append_json_int(out, 1);
    out += ',';
    json::append_json_key(out, "max_message_code_points");
    json::append_json_int(out, anvil::chat::kMaxMessageCodePoints);
    out += ',';
    json::append_json_key(out, "cases");
    out += '[';
    bool first = true;
    for (const tv::TextCase& c : tv::cases()) {
        // Printed only as anvil answers it: a case the validators disagree with
        // is a fixture that would teach the client something false.
        const auto [field, reason] = tv::validate(c);
        if (reason != c.reason || field != c.field) {
            std::fprintf(stderr, "case %s is not what anvil answers; nothing printed\n",
                         c.name.c_str());
            return 1;
        }
        if (!first) { out += ','; }
        first = false;
        out += '{';
        json::append_json_key(out, "name");
        json::append_json_string(out, c.name);
        out += ',';
        json::append_json_key(out, "validator");
        json::append_json_string(out, tv::validator_name(c.validator));
        out += ',';
        append_hex(out, c.validator == tv::Validator::Preview ? "url_hex" : "text_hex", c.text);
        if (c.validator == tv::Validator::Message) {
            out += ',';
            json::append_json_key(out, "max_code_points");
            json::append_json_int(out, c.max);
        }
        if (c.validator == tv::Validator::Preview) {
            out += ',';
            append_hex(out, "title_hex", c.title);
            out += ',';
            append_hex(out, "description_hex", c.description);
        }
        if (c.validator == tv::Validator::Mentions) {
            out += ',';
            json::append_json_key(out, "mentions");
            out += '[';
            for (std::size_t i = 0; i < c.mentions.size(); ++i) {
                if (i != 0) { out += ','; }
                out += '{';
                json::append_json_key(out, "user");
                json::append_json_uuid(out, c.mentions[i].user);
                out += ',';
                json::append_json_key(out, "offset");
                json::append_json_int(out, c.mentions[i].offset);
                out += ',';
                json::append_json_key(out, "length");
                json::append_json_int(out, c.mentions[i].length);
                out += '}';
            }
            out += ']';
        }
        out += ',';
        json::append_json_key(out, "expect");
        if (c.reason == anvil::input::Reason::Ok) {
            out += R"({"accepted":true})";
        } else {
            out += '{';
            json::append_json_key(out, "field");
            json::append_json_string(out, c.field);
            out += ',';
            json::append_json_key(out, "reason");
            json::append_json_string(out, json::wire_name(c.reason));
            out += '}';
        }
        out += '}';
    }
    out += "]}";
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
