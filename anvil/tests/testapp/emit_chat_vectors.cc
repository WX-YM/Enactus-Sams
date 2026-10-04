// Prints the chat golden vectors as the JSON fixture hammer's key store commits.
//
// The vectors are tests/testapp/chat_vectors.h, which a third implementation
// produced and tests/chat_vectors_test.cc holds anvil to. This program writes
// them as a client sends them: every key and signature in unpadded base64url,
// a device's bundle under the field names the device routes bind, the private
// halves in hex beside them so hammer can assert its own derivations and
// signatures land on the same bytes, and an encrypted send's body as the send
// route takes it.
//
//     build/release/tests/testapp_emit_chat_vectors > <hammer>/tests/chat/server_vectors.json

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/crypto/base64url.h"
#include "anvil/http/json_writer.h"

#include "chat_vectors.h"

namespace {

namespace json = anvil::http;

[[nodiscard]] std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out(hex.size() / 2);
    const auto nibble = [](char c) -> std::uint8_t {
        return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4U) | nibble(hex[2 * i + 1]));
    }
    return out;
}

void key(std::string& out, std::string_view name, std::string_view text) {
    json::append_json_key(out, name);
    json::append_json_string(out, text);
}

// A field as it travels: base64url of the hex vector.
void wire(std::string& out, std::string_view name, std::string_view hex) {
    key(out, name, anvil::crypto::base64url_encode(from_hex(hex)));
}

void append_device(std::string& out, const testapp::ChatDeviceVector& v) {
    out += '{';
    key(out, "name", v.name);
    out += ',';
    // What the device routes bind, under the names they bind it by.
    json::append_json_key(out, "bundle");
    out += '{';
    key(out, "device_id", v.device_id);
    out += ",\"suite\":1,";
    wire(out, "agreement_key", v.agreement_key);
    out += ',';
    wire(out, "signing_key", v.signing_key);
    out += ',';
    wire(out, "signed_prekey", v.signed_prekey);
    out += ',';
    wire(out, "signed_prekey_signature", v.signed_prekey_signature);
    out += ',';
    wire(out, "last_resort_key", v.last_resort_key);
    out += ',';
    wire(out, "last_resort_signature", v.last_resort_signature);
    out += "},";
    // The key store's side, and the exact bytes each signature is over.
    json::append_json_key(out, "private");
    out += '{';
    key(out, "signing_seed", v.signing_seed);
    out += ',';
    key(out, "agreement", v.agreement_private);
    out += ',';
    key(out, "signed_prekey", v.signed_prekey_private);
    out += ',';
    key(out, "last_resort", v.last_resort_private);
    out += "},";
    json::append_json_key(out, "signed_messages_hex");
    out += '{';
    key(out, "signed_prekey", v.signed_prekey_message);
    out += ',';
    key(out, "last_resort", v.last_resort_message);
    out += "}}";
}

}  // namespace

int main() {
    std::string out;
    out.reserve(8192);
    out += '{';
    json::append_json_key(out, "devices");
    out += '[';
    for (std::size_t i = 0; i < testapp::kChatDeviceVectors.size(); ++i) {
        if (i != 0) { out += ','; }
        append_device(out, testapp::kChatDeviceVectors[i]);
    }
    out += "],";

    // Device a admits device b: the link route's body beside b's bundle.
    const testapp::ChatDeviceVector& a = testapp::kChatDeviceVectors[0];
    json::append_json_key(out, "link");
    out += '{';
    key(out, "account", testapp::kChatLinkVector.account);
    out += ',';
    key(out, "approver", a.device_id);
    out += ',';
    json::append_json_key(out, "timestamp");
    json::append_json_int(out, static_cast<std::int64_t>(testapp::kChatLinkVector.timestamp_s));
    out += ',';
    key(out, "message_hex", testapp::kChatLinkVector.message);
    out += ',';
    wire(out, "link_signature", testapp::kChatLinkVector.signature);
    out += "},";

    json::append_json_key(out, "rejections");
    out += '[';
    for (std::size_t i = 0; i < testapp::kChatRejectionVectors.size(); ++i) {
        const testapp::ChatRejectionVector& bad = testapp::kChatRejectionVectors[i];
        if (i != 0) { out += ','; }
        out += '{';
        key(out, "name", bad.name);
        out += ',';
        key(out, "field", bad.field);
        out += ',';
        wire(out, "key", bad.key);
        out += '}';
    }
    out += "],";

    // An encrypted send from a to b, as the send route takes it: a common
    // ciphertext and one per-device ciphertext, both opaque to the server.
    json::append_json_key(out, "send");
    out += '{';
    key(out, "cid", anvil::crypto::base64url_encode(from_hex("000102030405060708090a0b0c0d0e0f")));
    out += ',';
    key(out, "device", a.device_id);
    out += ",\"dsv\":1767225600000,";
    key(out, "ciphertext", anvil::crypto::base64url_encode(from_hex("c0ffee")));
    out += ',';
    json::append_json_key(out, "devices");
    out += "[{";
    key(out, "device", testapp::kChatDeviceVectors[1].device_id);
    out += ',';
    key(out, "ciphertext", anvil::crypto::base64url_encode(from_hex("decafbad")));
    out += "}]},";

    json::append_json_key(out, "push");
    out += '{';
    key(out, "conversation", testapp::kChatPushConversation);
    out += ',';
    json::append_json_key(out, "seq");
    json::append_json_int(out, testapp::kChatPushSeq);
    out += ',';
    key(out, "payload", testapp::kChatPushPayload);
    out += "}}\n";

    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
