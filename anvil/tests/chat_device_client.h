#pragma once

// A chat device as its client holds it, for the suites that need one: the
// public bundle the server is sent, and the signing seed it never sees. Made
// the way a client makes one, so a case that passes is one a real client's
// bundle passes.

#include <utility>

#include "anvil/chat/devices.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/ed25519.h"
#include "anvil/crypto/x25519.h"

namespace anvil::chattest {

namespace chat = anvil::chat;
namespace crypto = anvil::crypto;

struct Client final {
    crypto::Ed25519Seed seed;
    chat::NewDevice device;
};

[[nodiscard]] inline Client make_client() {
    crypto::Ed25519Keypair signing = crypto::ed25519_generate_keypair();
    Client out{.seed = std::move(signing.seed), .device = {}};
    out.device.id = anvil::uuid::generate_v4();
    out.device.session = anvil::uuid::generate_v4();
    chat::DeviceKeys& keys = out.device.keys;
    keys.suite = chat::Suite::SignalX25519Ed25519;
    keys.signing = signing.public_key;
    keys.agreement = crypto::x25519_generate_keypair().public_key;
    keys.signed_prekey = crypto::x25519_generate_keypair().public_key;
    keys.last_resort = crypto::x25519_generate_keypair().public_key;
    keys.signed_prekey_signature = crypto::ed25519_sign(
        out.seed,
        chat::prekey_message(chat::kSignedPrekeyDomain, out.device.id, keys.signed_prekey));
    keys.last_resort_signature = crypto::ed25519_sign(
        out.seed,
        chat::prekey_message(chat::kLastResortDomain, out.device.id, keys.last_resort));
    return out;
}

}  // namespace anvil::chattest
