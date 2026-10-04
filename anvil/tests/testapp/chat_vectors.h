#pragma once

// The golden vectors for every byte the chat server checks on behalf of an
// encrypted client (docs/22-chat.md §7.3.1, §7.6, §7.8).
//
// The device bundle, the signed prekeys and the link signature are a byte-exact
// contract between anvil's checks here and hammer's key store there, and a
// check written against its own output is two copies of one belief. So these
// were produced by a third implementation: a throwaway script that took the
// primitives from Python's `cryptography` package and concatenated each signed
// message by hand from the table in §7.3.1, without reading either side's code.
// tests/chat_vectors_test.cc asserts anvil reproduces every byte and accepts
// every bundle; `testapp_emit_chat_vectors` prints them as the JSON fixture
// hammer commits, with the private halves, so hammer's suite can assert that
// its own derivations and signatures land on the same bytes.
//
// Every value is lowercase hex, except where a field says otherwise.

#include <array>
#include <cstdint>
#include <string_view>

namespace testapp {

// One device as its key store holds it. The private halves are fixed patterns
// (0x10, 0x11, … and so on) so they can be read off the page; they are test
// vectors and nothing else.
struct ChatDeviceVector final {
    std::string_view name;
    std::string_view device_id;                // canonical UUID text
    std::string_view signing_seed;             // Ed25519 seed (RFC 8032 private key)
    std::string_view signing_key;              // its public key
    std::string_view agreement_private;        // X25519 private
    std::string_view agreement_key;            // X25519(agreement_private, 9)
    std::string_view signed_prekey_private;
    std::string_view signed_prekey;
    std::string_view last_resort_private;
    std::string_view last_resort_key;
    // "anvil-chat-spk" ‖ device id ‖ signed prekey, and its signature by the
    // device's own signing key.
    std::string_view signed_prekey_message;
    std::string_view signed_prekey_signature;
    // "anvil-chat-lrk" ‖ device id ‖ last-resort key, likewise.
    std::string_view last_resort_message;
    std::string_view last_resort_signature;
};

inline constexpr std::array<ChatDeviceVector, 2> kChatDeviceVectors{{
    {.name = "a",
     .device_id = "6f1c2d3e-4b5a-4c6d-8e7f-a0b1c2d3e4f5",
     .signing_seed = "101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f",
     .signing_key = "7776e870b93354f2a0b24c23f2a36cc4e80e223218c1b97926fdd018396a2b9b",
     .agreement_private = "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f",
     .agreement_key = "34e42d4af5ef94a07a3a84201b889d4cd1a743cb27b11b6a10438a8feb8e5847",
     .signed_prekey_private =
         "505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f",
     .signed_prekey = "392d174a38b3b1beafaf1fe824870841c5fa531bc6eafdb6402c124664488c1c",
     .last_resort_private = "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f",
     .last_resort_key = "23b7bb8c91ae008711fb12846780bcdf1e065f821bdfec49f57e7c7dcd4c4823",
     .signed_prekey_message =
         "616e76696c2d636861742d73706b6f1c2d3e4b5a4c6d8e7fa0b1c2d3e4f5392d174a38b3b1beafaf1f"
         "e824870841c5fa531bc6eafdb6402c124664488c1c",
     .signed_prekey_signature =
         "c5063e75482447daecbeb9b2e9fa2a7e81960736a21964011a9efc1df3583ba0c586032b0f9fdaf7d5"
         "c73a56a5aec2048cc1634f7cdef29693fb500371110e08",
     .last_resort_message =
         "616e76696c2d636861742d6c726b6f1c2d3e4b5a4c6d8e7fa0b1c2d3e4f523b7bb8c91ae008711fb12"
         "846780bcdf1e065f821bdfec49f57e7c7dcd4c4823",
     .last_resort_signature =
         "83f1a6eb034ffa73d2286a361c516b48c3224800cf21cc70581e24d359fc33f96b8f378f80bc6edeb9"
         "18dcbec3749ab41440171025fa6cc8cdcea18188453001"},
    {.name = "b",
     .device_id = "7a8b9cad-becf-4d0e-9f10-213243546576",
     .signing_seed = "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
     .signing_key = "cd14b37f956e953194ff7fb73b3d81dcc561d61a7538094b7c3e1a643ee5f3aa",
     .agreement_private = "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf",
     .agreement_key = "605a725d2a4adfeeb1a29e17edd621c1b7593ee8cdbc44ac6c4ab6e2f805d23c",
     .signed_prekey_private =
         "c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf",
     .signed_prekey = "dc2cca31e8e43bbd91dff7e475cca3347eb478107d5bd765aba4ae4a30c35d44",
     .last_resort_private = "e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
     .last_resort_key = "736845d54e87de09d6bb114aa7042c50a4a015bd9901d1a0026f5956533a1519",
     .signed_prekey_message =
         "616e76696c2d636861742d73706b7a8b9cadbecf4d0e9f10213243546576dc2cca31e8e43bbd91dff7"
         "e475cca3347eb478107d5bd765aba4ae4a30c35d44",
     .signed_prekey_signature =
         "9ec717c0babfee5f20ccdd875232c00f508646095da2f2fd2ee813062630a018306b07df2f035ed2ba"
         "6dde31f031c9e223da8e264fbdafdde690242e9b0a9507",
     .last_resort_message =
         "616e76696c2d636861742d6c726b7a8b9cadbecf4d0e9f10213243546576736845d54e87de09d6bb11"
         "4aa7042c50a4a015bd9901d1a0026f5956533a1519",
     .last_resort_signature =
         "ab4211febcc4ecc072d305b62e14964f8e85b6797401f330ec5ba7856bc10f877b1e3548ef532d5cac"
         "822eff532806375ac760f27bd13ede58074c89207cb400"},
}};

// Device "a" admits device "b" to the account.
struct ChatLinkVector final {
    std::string_view account;     // canonical UUID text
    std::uint64_t    timestamp_s;
    // "anvil-chat-link" ‖ account ‖ b's id ‖ b's agreement key ‖ b's signing key
    // ‖ timestamp as u64 big-endian: 119 bytes.
    std::string_view message;
    // By a's signing key.
    std::string_view signature;
};

inline constexpr ChatLinkVector kChatLinkVector{
    .account = "0190a4e2-7c1d-7b3a-9f00-112233445566",
    .timestamp_s = 1'767'225'600,
    .message =
        "616e76696c2d636861742d6c696e6b0190a4e27c1d7b3a9f001122334455667a8b9cadbecf4d0e9f1021"
        "3243546576605a725d2a4adfeeb1a29e17edd621c1b7593ee8cdbc44ac6c4ab6e2f805d23ccd14b37f95"
        "6e953194ff7fb73b3d81dcc561d61a7538094b7c3e1a643ee5f3aa000000006955b900",
    .signature =
        "e68534fcc4d46831c556f314d1e590aaac3697a9a535563c63a8dfdcf5c6f8caec55d3028903d72e86cf"
        "dc0b66fbbabeedf2ecd46f7366ec05f0de59f3b2a801",
};

// Keys the server refuses, and the field its refusal names. A client that
// produces one has a bug, and the fixture lets hammer assert it never does.
struct ChatRejectionVector final {
    std::string_view name;
    std::string_view field;   // the device_inputs name
    std::string_view key;     // 32 bytes
};

inline constexpr std::array<ChatRejectionVector, 5> kChatRejectionVectors{{
    // Low order: the shared secret with it is all zero.
    {"x25519 zero", "agreement_key",
     "0000000000000000000000000000000000000000000000000000000000000000"},
    {"x25519 one", "agreement_key",
     "0100000000000000000000000000000000000000000000000000000000000000"},
    // u = p, a second spelling of zero: WebCrypto never exports one, and a key
    // with two spellings breaks byte comparison and safety numbers.
    {"x25519 non-canonical", "agreement_key",
     "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"},
    // The identity point, under which OpenSSL accepts a forgery.
    {"ed25519 identity", "signing_key",
     "0100000000000000000000000000000000000000000000000000000000000000"},
    // y = p, non-canonical.
    {"ed25519 non-canonical", "signing_key",
     "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"},
}};

// An encrypted conversation's push payload for conversation `push_conversation`
// at seq 42, exactly (§7.8).
inline constexpr std::string_view kChatPushConversation = "5b9e8f7a-6c5d-4e3f-a2b1-c0d9e8f7a6b5";
inline constexpr std::int64_t kChatPushSeq = 42;
inline constexpr std::string_view kChatPushPayload =
    R"({"c":"5b9e8f7a-6c5d-4e3f-a2b1-c0d9e8f7a6b5","seq":42})";

}  // namespace testapp
