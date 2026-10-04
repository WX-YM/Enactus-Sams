// Prints the chat socket's golden frames and every refusal as the JSON fixture
// hammer's frame codec is held to (docs/22-chat.md §8.2).
//
// The vectors are tests/testapp/chat_frame_vectors.h, which
// tests/chat_frames_test.cc holds anvil's codec to. Bytes are lowercase hex.
// Each golden carries what it decodes to, field by field, ids in their
// hyphenated form, so a client asserts its decoder rather than only its
// round trip. Each refusal carries the fault name and the error code the
// decoder refuses it with; a client that refuses with another name is a
// client that counts its closes differently.
//
//     build/<preset>/tests/testapp_emit_chat_frames > <hammer>/tests/chat/frame_vectors.json

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "anvil/chat/frames.h"
#include "anvil/http/errors.h"
#include "anvil/http/json_writer.h"

#include "chat_frame_vectors.h"

namespace {

namespace json = anvil::http;
namespace frames = anvil::chat::frames;
namespace fv = testapp::frame_vectors;

void append_hex(std::string& out, std::string_view key, std::span<const std::uint8_t> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    json::append_json_key(out, key);
    out += '"';
    for (const std::uint8_t b : bytes) {
        out += digits[b >> 4U];
        out += digits[b & 0xFU];
    }
    out += '"';
}

void field(std::string& out, std::string_view key, std::int64_t value) {
    out += ',';
    json::append_json_key(out, key);
    json::append_json_int(out, value);
}

void field(std::string& out, std::string_view key, const anvil::Uuid& value) {
    out += ',';
    json::append_json_key(out, key);
    json::append_json_uuid(out, value);
}

void type(std::string& out, std::string_view name) {
    json::append_json_key(out, "type");
    json::append_json_string(out, name);
}

void append_frame(std::string& out, const frames::DownstreamFrame& frame) {
    out += '{';
    std::visit(
        [&out](const auto& f) {
            using F = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<F, frames::Wake>) {
                type(out, "wake");
                field(out, "conversation", f.conversation);
                field(out, "seq", f.seq);
                out += ',';
                append_hex(out, "inline_hex", f.inline_message);
            } else if constexpr (std::is_same_v<F, frames::Typing>) {
                type(out, "typing");
                field(out, "conversation", f.conversation);
                field(out, "user", f.user);
            } else if constexpr (std::is_same_v<F, frames::Presence>) {
                type(out, "presence");
                field(out, "user", f.user);
                out += ',';
                json::append_json_key(out, "state");
                json::append_json_string(
                    out, f.state == frames::PresenceState::Online ? "online" : "offline");
                field(out, "last_seen_unix_ms", f.last_seen_unix_ms);
            } else if constexpr (std::is_same_v<F, frames::Receipt>) {
                type(out, "receipt");
                field(out, "conversation", f.conversation);
                field(out, "user", f.user);
                field(out, "delivered_seq", f.delivered_seq);
                field(out, "read_seq", f.read_seq);
            } else if constexpr (std::is_same_v<F, frames::Membership>) {
                type(out, "membership");
                field(out, "conversation", f.conversation);
                field(out, "membership_version", f.membership_version);
            } else if constexpr (std::is_same_v<F, frames::Mutation>) {
                type(out, "mutation");
                field(out, "conversation", f.conversation);
                field(out, "mutation", f.mutation);
            } else if constexpr (std::is_same_v<F, frames::Ping>) {
                type(out, "ping");
            } else if constexpr (std::is_same_v<F, frames::Pong>) {
                type(out, "pong");
            } else {
                type(out, "sync");
            }
        },
        frame);
    out += '}';
}

void append_frame(std::string& out, const frames::UpstreamFrame& frame) {
    out += '{';
    std::visit(
        [&out](const auto& f) {
            using F = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<F, frames::ClientTyping>) {
                type(out, "client_typing");
                field(out, "conversation", f.conversation);
            } else if constexpr (std::is_same_v<F, frames::ClientPong>) {
                type(out, "client_pong");
            } else {
                type(out, "client_ping");
            }
        },
        frame);
    out += '}';
}

// Answers false, having printed nothing useful, when the codec refuses its own
// golden: the fixture is then not printed at all.
[[nodiscard]] bool append_golden(std::string& out, std::string_view name,
                                 std::span<const std::uint8_t> bytes, fv::Direction direction) {
    out += '{';
    json::append_json_key(out, "name");
    json::append_json_string(out, name);
    out += ',';
    json::append_json_key(out, "direction");
    json::append_json_string(out, fv::direction_name(direction));
    out += ',';
    append_hex(out, "hex", bytes);
    out += ',';
    json::append_json_key(out, "frame");
    if (direction == fv::Direction::Down) {
        const auto decoded = frames::decode_downstream(bytes);
        if (!decoded) { return false; }
        append_frame(out, decoded.value());
    } else {
        const auto decoded = frames::decode_upstream(bytes);
        if (!decoded) { return false; }
        append_frame(out, decoded.value());
    }
    out += '}';
    return true;
}

}  // namespace

int main() {
    std::string out;
    out.reserve(1U << 20U);
    out += '{';
    json::append_json_key(out, "format");
    json::append_json_int(out, 1);
    field(out, "frame_version", frames::kFrameVersion);
    field(out, "inline_wake_bytes", static_cast<std::int64_t>(frames::kInlineWakeBytes));
    field(out, "max_downstream_bytes", static_cast<std::int64_t>(frames::kMaxDownstreamFrameBytes));
    field(out, "max_upstream_bytes", static_cast<std::int64_t>(frames::kMaxUpstreamFrameBytes));
    out += ',';
    json::append_json_key(out, "goldens");
    out += '[';
    bool first = true;
    for (const fv::Golden& golden : fv::kGoldens) {
        if (!first) { out += ','; }
        first = false;
        if (!append_golden(out, golden.name, golden.bytes, golden.direction)) {
            std::fputs("a golden frame does not decode; nothing printed\n", stderr);
            return 1;
        }
    }
    out += ',';
    const std::vector<std::uint8_t> largest = fv::largest_wake();
    if (!append_golden(out, "wake_largest", largest, fv::Direction::Down)) { return 1; }
    out += "],";
    json::append_json_key(out, "refusals");
    out += '[';
    first = true;
    for (const fv::Refusal& refusal : fv::refusals()) {
        if (!first) { out += ','; }
        first = false;
        out += '{';
        json::append_json_key(out, "name");
        json::append_json_string(out, refusal.name);
        out += ',';
        json::append_json_key(out, "direction");
        json::append_json_string(out, fv::direction_name(refusal.direction));
        out += ',';
        append_hex(out, "hex", refusal.bytes);
        out += ',';
        json::append_json_key(out, "fault");
        json::append_json_string(out, refusal.fault);
        out += ',';
        json::append_json_key(out, "code");
        json::append_json_string(out, json::wire_name(refusal.code));
        out += '}';
    }
    out += "]}";
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
