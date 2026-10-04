// anvil against the chat golden vectors (tests/testapp/chat_vectors.h), which a
// third implementation produced from docs/22-chat.md §7.3.1 alone.
//
// Each case is one byte the server checks and hammer has to produce: a key
// derived from its private half, a signed message laid out field by field, a
// signature, and a key the server must refuse. A drift in any of them is a
// client whose every device the server turns away.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/chat/devices.h"
#include "anvil/chat/push.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/ed25519.h"
#include "anvil/crypto/x25519.h"
#include "testapp/chat_vectors.h"

namespace {

namespace chat = anvil::chat;
namespace crypto = anvil::crypto;
using anvil::Uuid;

[[nodiscard]] std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(std::stoul(std::string{hex.substr(2 * i, 2)}, nullptr, 16));
    }
    return out;
}

template <std::size_t N>
[[nodiscard]] std::array<std::uint8_t, N> array_from_hex(std::string_view hex) {
    const std::vector<std::uint8_t> bytes = from_hex(hex);
    std::array<std::uint8_t, N> out{};
    EXPECT_EQ(bytes.size(), N) << hex;
    std::copy_n(bytes.begin(), std::min(N, bytes.size()), out.begin());
    return out;
}

template <typename Secret>
[[nodiscard]] Secret secret_from_hex(std::string_view hex) {
    const std::vector<std::uint8_t> bytes = from_hex(hex);
    Secret out;
    std::copy_n(bytes.begin(), std::min(out.size(), bytes.size()), out.data());
    return out;
}

// X25519's base point, u = 9: a public key is X25519(private, 9).
[[nodiscard]] crypto::X25519PublicKey public_of(std::string_view private_hex) {
    crypto::X25519PublicKey base{};
    base[0] = 9;
    const auto shared =
        crypto::x25519_derive(secret_from_hex<crypto::X25519PrivateKey>(private_hex), base);
    crypto::X25519PublicKey out{};
    if (shared.has_value()) { std::copy_n(shared->data(), out.size(), out.begin()); }
    return out;
}

[[nodiscard]] Uuid uuid_of(std::string_view text) { return *anvil::uuid::parse(text); }

// The bundle as the vectors' client would upload it.
[[nodiscard]] chat::DeviceKeys keys_of(const testapp::ChatDeviceVector& v) {
    chat::DeviceKeys keys{};
    keys.suite = chat::Suite::SignalX25519Ed25519;
    keys.agreement = array_from_hex<32>(v.agreement_key);
    keys.signing = array_from_hex<32>(v.signing_key);
    keys.signed_prekey = array_from_hex<32>(v.signed_prekey);
    keys.signed_prekey_signature = array_from_hex<64>(v.signed_prekey_signature);
    keys.last_resort = array_from_hex<32>(v.last_resort_key);
    keys.last_resort_signature = array_from_hex<64>(v.last_resort_signature);
    return keys;
}

TEST(ChatVectors, EveryPublicKeyIsDerivedFromItsPrivateHalf) {
    for (const testapp::ChatDeviceVector& v : testapp::kChatDeviceVectors) {
        EXPECT_EQ(crypto::ed25519_public_key_from_seed(
                      secret_from_hex<crypto::Ed25519Seed>(v.signing_seed)),
                  array_from_hex<32>(v.signing_key))
            << v.name;
        EXPECT_EQ(public_of(v.agreement_private), array_from_hex<32>(v.agreement_key)) << v.name;
        EXPECT_EQ(public_of(v.signed_prekey_private), array_from_hex<32>(v.signed_prekey))
            << v.name;
        EXPECT_EQ(public_of(v.last_resort_private), array_from_hex<32>(v.last_resort_key))
            << v.name;
    }
}

TEST(ChatVectors, ThePrekeyMessagesAreLaidOutAsDocumentedAndSignedByTheDevice) {
    for (const testapp::ChatDeviceVector& v : testapp::kChatDeviceVectors) {
        const Uuid device = uuid_of(v.device_id);
        const chat::PrekeyMessage spk = chat::prekey_message(
            chat::kSignedPrekeyDomain, device, array_from_hex<32>(v.signed_prekey));
        const chat::PrekeyMessage lrk = chat::prekey_message(
            chat::kLastResortDomain, device, array_from_hex<32>(v.last_resort_key));
        EXPECT_EQ(std::vector<std::uint8_t>(spk.begin(), spk.end()), from_hex(v.signed_prekey_message))
            << v.name;
        EXPECT_EQ(std::vector<std::uint8_t>(lrk.begin(), lrk.end()), from_hex(v.last_resort_message))
            << v.name;
        // Ed25519 is deterministic, so the signature itself is a vector.
        const crypto::Ed25519Seed seed = secret_from_hex<crypto::Ed25519Seed>(v.signing_seed);
        EXPECT_EQ(crypto::ed25519_sign(seed, spk), array_from_hex<64>(v.signed_prekey_signature))
            << v.name;
        EXPECT_EQ(crypto::ed25519_sign(seed, lrk), array_from_hex<64>(v.last_resort_signature))
            << v.name;
        // And the whole bundle is one the server accepts.
        EXPECT_TRUE(chat::validate_device(device, keys_of(v)).ok()) << v.name;
    }
}

TEST(ChatVectors, TheLinkMessageIsLaidOutAsDocumentedAndVerifiesUnderTheApprover) {
    const testapp::ChatDeviceVector& approver = testapp::kChatDeviceVectors[0];
    const testapp::ChatDeviceVector& joining = testapp::kChatDeviceVectors[1];
    const chat::LinkMessage message = chat::link_message(
        uuid_of(testapp::kChatLinkVector.account), uuid_of(joining.device_id),
        array_from_hex<32>(joining.agreement_key), array_from_hex<32>(joining.signing_key),
        testapp::kChatLinkVector.timestamp_s);
    EXPECT_EQ(std::vector<std::uint8_t>(message.begin(), message.end()),
              from_hex(testapp::kChatLinkVector.message));
    EXPECT_EQ(crypto::ed25519_sign(secret_from_hex<crypto::Ed25519Seed>(approver.signing_seed),
                                   message),
              array_from_hex<64>(testapp::kChatLinkVector.signature));
    EXPECT_TRUE(crypto::ed25519_verify(array_from_hex<32>(approver.signing_key), message,
                                       array_from_hex<64>(testapp::kChatLinkVector.signature)));
    // Signed for one account, it is not a link into another.
    chat::LinkMessage elsewhere = message;
    elsewhere[15] ^= 0x01U;
    EXPECT_FALSE(crypto::ed25519_verify(array_from_hex<32>(approver.signing_key), elsewhere,
                                        array_from_hex<64>(testapp::kChatLinkVector.signature)));
}

TEST(ChatVectors, EveryRejectionIsRefusedNamingItsField) {
    const testapp::ChatDeviceVector& v = testapp::kChatDeviceVectors[0];
    for (const testapp::ChatRejectionVector& bad : testapp::kChatRejectionVectors) {
        chat::DeviceKeys keys = keys_of(v);
        if (bad.field == "agreement_key") {
            keys.agreement = array_from_hex<32>(bad.key);
        } else {
            keys.signing = array_from_hex<32>(bad.key);
        }
        const anvil::Status refused = chat::validate_device(uuid_of(v.device_id), keys);
        ASSERT_FALSE(refused.ok()) << bad.name;
        EXPECT_EQ(refused.error().field, bad.field) << bad.name;
    }
}

TEST(ChatVectors, AnEncryptedPushIsExactlyTheConversationAndTheSeq) {
    EXPECT_EQ(chat::encrypted_push_payload(uuid_of(testapp::kChatPushConversation),
                                           testapp::kChatPushSeq),
              testapp::kChatPushPayload);
}

}  // namespace
