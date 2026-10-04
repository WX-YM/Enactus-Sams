// The device directory and the key directory against a live replica set
// (docs/22-chat.md §7.3, §7.5).
//
// One case per security property, and each is one a plausible implementation
// gets wrong: a session admitted where a signature is required, a read before
// the write that two racers both pass, a signature checked against the wrong
// account's key, a replay inside the clock window.

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "anvil/chat/devices.h"
#include "anvil/chat/prekeys.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/ed25519.h"
#include "anvil/crypto/x25519.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/chat_collections.h"

namespace {

namespace chat = anvil::chat;
namespace crypto = anvil::crypto;
using anvil::ErrorCode;
using anvil::Uuid;
using anvil::db::TimeMs;
using anvil::testfixture::scratch_names;

// A device as its client holds it: the public bundle and the signing seed the
// server never sees.
struct Client final {
    crypto::Ed25519Seed seed;
    chat::NewDevice device;
};

[[nodiscard]] Client make_client() {
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

[[nodiscard]] std::uint64_t unix_s(TimeMs at) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count());
}

// What the approving device signs to admit `device` to `account`.
[[nodiscard]] crypto::Ed25519Signature approve(const Client& approver,
                                               const Uuid& account,
                                               const chat::NewDevice& device,
                                               std::uint64_t timestamp_s) {
    return crypto::ed25519_sign(
        approver.seed,
        chat::link_message(
            account, device.id, device.keys.agreement, device.keys.signing, timestamp_s));
}

class ChatDevicesDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ =
            std::make_unique<mongocxx::pool::entry>(anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, testapp::kDeviceCollections.identities);
        anvil::testfixture::clear_collection(**client_, testapp::kDeviceCollections.prekeys);
        devices_ = std::make_unique<chat::DeviceDirectory>(
            scratch_names(), testapp::kDeviceCollections, testapp::kDeviceConfig);
        prekeys_ = std::make_unique<chat::PrekeyDirectory>(
            scratch_names(), testapp::kDeviceCollections, testapp::kDeviceConfig);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] const chat::DeviceDirectory& devices() const { return *devices_; }

    [[nodiscard]] const chat::PrekeyDirectory& prekeys() const { return *prekeys_; }

    [[nodiscard]] anvil::Status first(const Uuid& user,
                                      const Client& client,
                                      TimeMs now = anvil::db::now_ms()) {
        return devices().register_first_device(db(), user, client.device, now, now);
    }

    [[nodiscard]] anvil::Status link(const Uuid& user,
                                     const Client& approver,
                                     const Client& joining,
                                     TimeMs now = anvil::db::now_ms()) {
        const std::uint64_t at = unix_s(now);
        return devices().link_device(db(),
                                     user,
                                     approver.device.id,
                                     joining.device,
                                     at,
                                     approve(approver, user, joining.device, at),
                                     now);
    }

    [[nodiscard]] chat::IdentityRecord identity(const Uuid& user) {
        const auto found = devices().identity(db(), user);
        EXPECT_TRUE(found.ok());
        EXPECT_TRUE(found.ok() && found.value().has_value());
        return found.ok() && found.value().has_value() ? *found.value()
                                                       : chat::IdentityRecord{};
    }

    [[nodiscard]] std::size_t count(const Uuid& user) { return identity(user).devices.size(); }

    std::unique_ptr<mongocxx::pool::entry> client_;
    std::unique_ptr<chat::DeviceDirectory> devices_;
    std::unique_ptr<chat::PrekeyDirectory> prekeys_;
};

// `count` fresh one-time keys with ids from `first_id`.
[[nodiscard]] std::vector<chat::OneTimePrekey> batch(std::size_t count,
                                                     std::uint32_t first_id) {
    std::vector<chat::OneTimePrekey> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(chat::OneTimePrekey{crypto::x25519_generate_keypair().public_key,
                                          first_id + static_cast<std::uint32_t>(i)});
    }
    return out;
}

// --- the first device -------------------------------------------------------------

TEST_F(ChatDevicesDb, AFirstDeviceNeedsAFreshPrimaryAuthenticationNotJustASession) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    const TimeMs now = anvil::db::now_ms();

    // A session that authenticated six minutes ago is a cookie, not a password.
    const anvil::Status stale = devices().register_first_device(
        db(), user, phone.device, now - std::chrono::minutes{6}, now);
    // Not Unauthenticated: the session is valid, and a client answers a 401 by
    // refreshing its token, which cannot help and ends with it signed out.
    EXPECT_EQ(stale.code(), ErrorCode::CapabilityRequired);
    EXPECT_EQ(stale.error().field, chat::device_inputs::kAuthenticatedAt);
    // Nor is a time from the future, which is what a forged or confused
    // timestamp looks like.
    EXPECT_EQ(
        devices()
            .register_first_device(db(), user, phone.device, now + std::chrono::minutes{5}, now)
            .code(),
        ErrorCode::CapabilityRequired);
    EXPECT_FALSE(devices().identity(db(), user).value().has_value());

    EXPECT_TRUE(
        devices()
            .register_first_device(db(), user, phone.device, now - std::chrono::minutes{4}, now)
            .ok());
    const chat::IdentityRecord row = identity(user);
    ASSERT_EQ(row.devices.size(), 1U);
    EXPECT_EQ(row.device_set_version, 1);
    // Nothing signed the first device, and it does not pretend otherwise.
    EXPECT_FALSE(row.devices[0].published.link.has_value());
    EXPECT_EQ(row.devices[0].session, phone.device.session);
    // The change is marked for propagation in the same write (§7.4).
    EXPECT_EQ(row.pending_since, now);
}

// --- rotating the signed keys (docs/22-chat.md §7.3.1) ---------------------------

[[nodiscard]] chat::SignedPrekey signed_by(const Client& owner, std::string_view domain) {
    const crypto::X25519PublicKey key = crypto::x25519_generate_keypair().public_key;
    return chat::SignedPrekey{
        crypto::ed25519_sign(owner.seed, chat::prekey_message(domain, owner.device.id, key)), key};
}

TEST_F(ChatDevicesDb, ASignedPrekeyIsRotatedUnderTheDevicesOwnSigningKey) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    const std::int64_t version = identity(user).device_set_version;

    const chat::SignedPrekey spk = signed_by(phone, chat::kSignedPrekeyDomain);
    const chat::SignedPrekey lrk = signed_by(phone, chat::kLastResortDomain);
    ASSERT_TRUE(devices().rotate_prekeys(db(), user, phone.device.id, spk, lrk).ok());
    const chat::IdentityRecord row = identity(user);
    EXPECT_EQ(row.devices[0].published.keys.signed_prekey, spk.key);
    EXPECT_EQ(row.devices[0].published.keys.signed_prekey_signature, spk.signature);
    EXPECT_EQ(row.devices[0].published.keys.last_resort, lrk.key);
    EXPECT_EQ(row.devices[0].published.keys.last_resort_signature, lrk.signature);
    // The set of devices did not change, and no fence moves for it.
    EXPECT_EQ(row.device_set_version, version);
    // A claim hands out the new one at once.
    const auto claimed = prekeys().claim(db(), user);
    ASSERT_TRUE(claimed.ok());
    EXPECT_EQ(claimed.value()[0].keys.signed_prekey, spk.key);

    // One alone is a rotation too.
    const chat::SignedPrekey next = signed_by(phone, chat::kSignedPrekeyDomain);
    ASSERT_TRUE(devices().rotate_prekeys(db(), user, phone.device.id, next, std::nullopt).ok());
    EXPECT_EQ(identity(user).devices[0].published.keys.last_resort, lrk.key);
}

TEST_F(ChatDevicesDb, ARotationSignedByAnythingButTheDeviceIsRefused) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    const Client laptop = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    ASSERT_TRUE(link(user, phone, laptop).ok());
    const chat::IdentityRecord before = identity(user);

    // The laptop's signature over the phone's key: another device of the same
    // account is not the device.
    const crypto::X25519PublicKey key = crypto::x25519_generate_keypair().public_key;
    const chat::SignedPrekey forged{
        crypto::ed25519_sign(laptop.seed,
                             chat::prekey_message(chat::kSignedPrekeyDomain, phone.device.id, key)),
        key};
    const anvil::Status refused =
        devices().rotate_prekeys(db(), user, phone.device.id, forged, std::nullopt);
    EXPECT_EQ(refused.code(), ErrorCode::ValidationFailed);
    EXPECT_EQ(refused.error().field, chat::device_inputs::kSignedPrekeySignature);
    // A signed prekey offered as a last-resort key: the domain is in the message.
    const anvil::Status crossed = devices().rotate_prekeys(
        db(), user, phone.device.id, std::nullopt, signed_by(phone, chat::kSignedPrekeyDomain));
    EXPECT_EQ(crossed.error().field, chat::device_inputs::kLastResortSignature);
    // Another account's device, and nothing at all.
    EXPECT_EQ(devices()
                  .rotate_prekeys(db(), anvil::uuid::generate_v4(), phone.device.id,
                                  signed_by(phone, chat::kSignedPrekeyDomain), std::nullopt)
                  .code(),
              ErrorCode::Forbidden);
    EXPECT_EQ(devices().rotate_prekeys(db(), user, phone.device.id, std::nullopt, std::nullopt)
                  .code(),
              ErrorCode::ValidationFailed);
    EXPECT_EQ(identity(user).devices[0].published.keys.signed_prekey,
              before.devices[0].published.keys.signed_prekey);
}

TEST_F(ChatDevicesDb, ASecondFirstDeviceIsRefused) {
    const Uuid user = anvil::uuid::generate_v4();
    ASSERT_TRUE(first(user, make_client()).ok());

    // A fresh authentication does not make a second device "first": that would
    // be a stolen password adding a device with no signature from the first.
    const anvil::Status second = first(user, make_client());
    EXPECT_EQ(second.code(), ErrorCode::Conflict);
    EXPECT_EQ(count(user), 1U);
    EXPECT_EQ(identity(user).device_set_version, 1);
}

TEST_F(ChatDevicesDb, TwoConcurrentFirstRegistrationsLeaveExactlyOneDevice) {
    const Uuid user = anvil::uuid::generate_v4();
    constexpr int kRacers = 8;
    std::vector<Client> racers;
    racers.reserve(kRacers);
    for (int i = 0; i < kRacers; ++i) {
        racers.push_back(make_client());
    }

    // Separate pool clients, because a client is not thread-safe and because
    // the race has to reach the server as concurrent operations.
    std::vector<std::future<anvil::Status>> outcomes;
    outcomes.reserve(kRacers);
    for (const Client& racer : racers) {
        outcomes.push_back(std::async(std::launch::async, [this, &racer, user] {
            auto client = anvil::db::MongoPool::instance().acquire();
            const TimeMs now = anvil::db::now_ms();
            return devices().register_first_device(*client, user, racer.device, now, now);
        }));
    }
    int won = 0;
    for (auto& outcome : outcomes) {
        const anvil::Status status = outcome.get();
        if (status.ok()) {
            ++won;
        } else {
            EXPECT_EQ(status.code(), ErrorCode::Conflict);
        }
    }
    EXPECT_EQ(won, 1);
    EXPECT_EQ(count(user), 1U);
    EXPECT_EQ(identity(user).device_set_version, 1);
}

TEST_F(ChatDevicesDb, AnAccountWhoseDevicesWereAllUnlinkedRegistersAFirstDeviceAgain) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    ASSERT_TRUE(
        devices().unlink_device(db(), user, phone.device.id, anvil::db::now_ms()).value());

    // The reset of §7.3: fresh authentication, a new first device, and dv moves
    // so every conversation learns the key changed.
    EXPECT_TRUE(first(user, make_client()).ok());
    EXPECT_EQ(count(user), 1U);
    EXPECT_EQ(identity(user).device_set_version, 3);
}

TEST_F(ChatDevicesDb, ADeviceIdNamesOneDeviceAcrossEveryAccount) {
    const Uuid alice = anvil::uuid::generate_v4();
    const Uuid mallory = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(alice, phone).ok());

    // Mallory re-signs Alice's id with keys of her own: the id is what other
    // clients address ciphertext to, so it cannot be held twice.
    Client copy = make_client();
    copy.device.id = phone.device.id;
    copy.device.keys.signed_prekey_signature = crypto::ed25519_sign(
        copy.seed,
        chat::prekey_message(
            chat::kSignedPrekeyDomain, copy.device.id, copy.device.keys.signed_prekey));
    copy.device.keys.last_resort_signature = crypto::ed25519_sign(
        copy.seed,
        chat::prekey_message(
            chat::kLastResortDomain, copy.device.id, copy.device.keys.last_resort));
    EXPECT_EQ(first(mallory, copy).code(), ErrorCode::Conflict);
    EXPECT_FALSE(devices().identity(db(), mallory).value().has_value());
}

// --- keys as bytes ------------------------------------------------------------------

TEST_F(ChatDevicesDb, MalformedKeysAreRefusedNamingTheKeyAndNothingIsWritten) {
    const Uuid user = anvil::uuid::generate_v4();
    const auto refused = [&](const Client& client, std::string_view field) {
        const anvil::Status status = first(user, client);
        EXPECT_EQ(status.code(), ErrorCode::ValidationFailed) << field;
        if (!status.ok()) {
            EXPECT_EQ(status.error().field, field);
        }
    };

    {
        // u = 0 is low order: every shared secret with it is all zeros.
        Client client = make_client();
        client.device.keys.agreement.fill(0);
        refused(client, chat::device_inputs::kAgreementKey);
    }
    {
        // The identity point (y = 1): a "key" whose signatures verify for
        // messages its holder never signed.
        Client client = make_client();
        client.device.keys.signing.fill(0);
        client.device.keys.signing[0] = 1;
        refused(client, chat::device_inputs::kSigningKey);
    }
    {
        Client client = make_client();
        client.device.keys.signed_prekey_signature[5] ^= 0x01U;
        refused(client, chat::device_inputs::kSignedPrekeySignature);
    }
    {
        // A signed prekey signature presented as the last-resort one: the
        // domains differ, so one never verifies as the other.
        Client client = make_client();
        client.device.keys.last_resort = client.device.keys.signed_prekey;
        client.device.keys.last_resort_signature = client.device.keys.signed_prekey_signature;
        refused(client, chat::device_inputs::kLastResortSignature);
    }
    {
        Client client = make_client();
        client.device.keys.signed_prekey.fill(0);
        refused(client, chat::device_inputs::kSignedPrekey);
    }
    {
        Client client = make_client();
        client.device.keys.suite = chat::Suite::Reserved;
        refused(client, chat::device_inputs::kSuite);
    }
    {
        Client client = make_client();
        client.device.id = anvil::kNilUuid;
        refused(client, chat::device_inputs::kDeviceId);
    }
    EXPECT_FALSE(devices().identity(db(), user).value().has_value());
}

// --- linking ------------------------------------------------------------------------

TEST_F(ChatDevicesDb, AValidSessionWithNoApprovingSignatureCannotAddADevice) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    const Client laptop = make_client();
    const TimeMs now = anvil::db::now_ms();

    // Everything a stolen cookie can supply: the account, a real approver id
    // read off the device list, fresh keys and a current timestamp. No
    // signature, because no cookie carries a device's signing key.
    const anvil::Status refused = devices().link_device(db(),
                                                        user,
                                                        phone.device.id,
                                                        laptop.device,
                                                        unix_s(now),
                                                        crypto::Ed25519Signature{},
                                                        now);
    EXPECT_EQ(refused.code(), ErrorCode::Forbidden);
    EXPECT_EQ(refused.error().field, chat::device_inputs::kLinkSignature);
    EXPECT_EQ(count(user), 1U);
    EXPECT_EQ(identity(user).device_set_version, 1);

    EXPECT_TRUE(link(user, phone, laptop).ok());
    const chat::IdentityRecord row = identity(user);
    ASSERT_EQ(row.devices.size(), 2U);
    EXPECT_EQ(row.device_set_version, 2);
    ASSERT_TRUE(row.devices[1].published.link.has_value());
    EXPECT_EQ(row.devices[1].published.link->approver, phone.device.id);
}

TEST_F(ChatDevicesDb, ASignatureByAnUnlinkedDeviceIsRefused) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    const Client tablet = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    ASSERT_TRUE(link(user, phone, tablet).ok());
    ASSERT_TRUE(
        devices().unlink_device(db(), user, tablet.device.id, anvil::db::now_ms()).value());

    // The tablet still holds its signing key; an unlinked device's key admits
    // nobody, which is the point of unlinking a lost one.
    const anvil::Status refused = link(user, tablet, make_client());
    EXPECT_EQ(refused.code(), ErrorCode::Forbidden);
    EXPECT_EQ(refused.error().field, chat::device_inputs::kApprover);
    EXPECT_EQ(count(user), 1U);
}

TEST_F(ChatDevicesDb, ASignatureByAnotherAccountsDeviceIsRefused) {
    const Uuid alice = anvil::uuid::generate_v4();
    const Uuid mallory = anvil::uuid::generate_v4();
    const Client alice_phone = make_client();
    const Client mallory_phone = make_client();
    ASSERT_TRUE(first(alice, alice_phone).ok());
    ASSERT_TRUE(first(mallory, mallory_phone).ok());
    const Client ghost = make_client();
    const TimeMs now = anvil::db::now_ms();
    const std::uint64_t at = unix_s(now);

    // Mallory's own device as the approver of a device on Alice's account.
    EXPECT_EQ(devices()
                  .link_device(db(),
                               alice,
                               mallory_phone.device.id,
                               ghost.device,
                               at,
                               approve(mallory_phone, alice, ghost.device, at),
                               now)
                  .code(),
              ErrorCode::Forbidden);
    // Alice's device named as the approver, Mallory's key behind the signature.
    EXPECT_EQ(devices()
                  .link_device(db(),
                               alice,
                               alice_phone.device.id,
                               ghost.device,
                               at,
                               approve(mallory_phone, alice, ghost.device, at),
                               now)
                  .code(),
              ErrorCode::Forbidden);
    // A signature Alice's phone really made, but for Mallory's account: the
    // account is inside the signed bytes, so it does not transfer.
    EXPECT_EQ(devices()
                  .link_device(db(),
                               alice,
                               alice_phone.device.id,
                               ghost.device,
                               at,
                               approve(alice_phone, mallory, ghost.device, at),
                               now)
                  .code(),
              ErrorCode::Forbidden);
    EXPECT_EQ(count(alice), 1U);
}

TEST_F(ChatDevicesDb, TheSignatureCoversTheNewDevicesKeys) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    const Client laptop = make_client();
    const TimeMs now = anvil::db::now_ms();
    const std::uint64_t at = unix_s(now);
    const crypto::Ed25519Signature signature = approve(phone, user, laptop.device, at);

    // The approval intercepted and the keys swapped for an attacker's: the
    // signature was over the original keys, so it no longer verifies.
    const Client attacker = make_client();
    chat::NewDevice swapped = attacker.device;
    swapped.id = laptop.device.id;
    swapped.keys.signed_prekey_signature = crypto::ed25519_sign(
        attacker.seed,
        chat::prekey_message(
            chat::kSignedPrekeyDomain, swapped.id, swapped.keys.signed_prekey));
    swapped.keys.last_resort_signature = crypto::ed25519_sign(
        attacker.seed,
        chat::prekey_message(chat::kLastResortDomain, swapped.id, swapped.keys.last_resort));
    EXPECT_EQ(
        devices().link_device(db(), user, phone.device.id, swapped, at, signature, now).code(),
        ErrorCode::Forbidden);
    // Nor is the timestamp outside the signature.
    EXPECT_EQ(
        devices()
            .link_device(db(), user, phone.device.id, laptop.device, at - 1, signature, now)
            .code(),
        ErrorCode::Forbidden);
    EXPECT_TRUE(devices()
                    .link_device(db(), user, phone.device.id, laptop.device, at, signature, now)
                    .ok());
}

TEST_F(ChatDevicesDb, ALinkOutsideTheClockWindowIsRefused) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    const TimeMs now = anvil::db::now_ms();

    for (const std::int64_t offset_s : {-301, 301}) {
        const Client laptop = make_client();
        const std::uint64_t at = unix_s(now) + static_cast<std::uint64_t>(offset_s);
        const anvil::Status refused =
            devices().link_device(db(),
                                  user,
                                  phone.device.id,
                                  laptop.device,
                                  at,
                                  approve(phone, user, laptop.device, at),
                                  now);
        EXPECT_EQ(refused.code(), ErrorCode::ValidationFailed) << offset_s;
        if (!refused.ok()) {
            EXPECT_EQ(refused.error().field, chat::device_inputs::kTimestamp);
        }
    }
    // A value no clock produces, which signed arithmetic must not wrap into
    // the window.
    const Client far = make_client();
    EXPECT_EQ(devices()
                  .link_device(db(),
                               user,
                               phone.device.id,
                               far.device,
                               UINT64_MAX,
                               approve(phone, user, far.device, UINT64_MAX),
                               now)
                  .code(),
              ErrorCode::ValidationFailed);
    EXPECT_EQ(count(user), 1U);

    const Client laptop = make_client();
    const std::uint64_t at = unix_s(now) - 299;
    EXPECT_TRUE(devices()
                    .link_device(db(),
                                 user,
                                 phone.device.id,
                                 laptop.device,
                                 at,
                                 approve(phone, user, laptop.device, at),
                                 now)
                    .ok());
}

TEST_F(ChatDevicesDb, AReplayedLinkIsRefusedEvenAfterItsDeviceWasUnlinked) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    const Client laptop = make_client();
    const TimeMs now = anvil::db::now_ms();
    const std::uint64_t at = unix_s(now);
    const crypto::Ed25519Signature signature = approve(phone, user, laptop.device, at);

    ASSERT_TRUE(devices()
                    .link_device(db(), user, phone.device.id, laptop.device, at, signature, now)
                    .ok());
    const anvil::Status again =
        devices().link_device(db(), user, phone.device.id, laptop.device, at, signature, now);
    EXPECT_EQ(again.code(), ErrorCode::Conflict);
    EXPECT_EQ(again.error().field, chat::device_inputs::kDeviceId);

    // The laptop is stolen and unlinked. Its own link request is still inside
    // the clock window and still carries a valid signature; replaying it must
    // not bring the laptop back.
    ASSERT_TRUE(devices().unlink_device(db(), user, laptop.device.id, now).value());
    const anvil::Status replayed =
        devices().link_device(db(), user, phone.device.id, laptop.device, at, signature, now);
    EXPECT_EQ(replayed.code(), ErrorCode::Conflict);
    EXPECT_EQ(count(user), 1U);
}

TEST_F(ChatDevicesDb, AnAccountHoldsAtMostMaxDevices) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    for (std::uint32_t i = 1; i < testapp::kDeviceConfig.max_devices; ++i) {
        ASSERT_TRUE(link(user, phone, make_client()).ok()) << i;
    }
    const anvil::Status full = link(user, phone, make_client());
    EXPECT_EQ(full.code(), ErrorCode::Conflict);
    EXPECT_EQ(full.error().field, chat::device_inputs::kDevices);
    EXPECT_EQ(count(user), testapp::kDeviceConfig.max_devices);
}

TEST_F(ChatDevicesDb, ConcurrentLinksCannotOverfillTheAccount) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    // Every racer passes the read (four of five slots are free); the filter of
    // the write is what holds the bound.
    constexpr int kRacers = 8;
    std::vector<Client> racers;
    racers.reserve(kRacers);
    for (int i = 0; i < kRacers; ++i) {
        racers.push_back(make_client());
    }
    std::vector<std::future<anvil::Status>> outcomes;
    outcomes.reserve(kRacers);
    for (const Client& racer : racers) {
        outcomes.push_back(std::async(std::launch::async, [this, &racer, &phone, user] {
            auto client = anvil::db::MongoPool::instance().acquire();
            const TimeMs now = anvil::db::now_ms();
            const std::uint64_t at = unix_s(now);
            return devices().link_device(*client,
                                         user,
                                         phone.device.id,
                                         racer.device,
                                         at,
                                         approve(phone, user, racer.device, at),
                                         now);
        }));
    }
    int linked = 0;
    for (auto& outcome : outcomes) {
        linked += outcome.get().ok() ? 1 : 0;
    }
    EXPECT_EQ(linked, static_cast<int>(testapp::kDeviceConfig.max_devices) - 1);
    EXPECT_EQ(count(user), testapp::kDeviceConfig.max_devices);
}

// --- unlinking ----------------------------------------------------------------------

TEST_F(ChatDevicesDb, UnlinkBumpsTheVersionMarksTheChangeAndIsIdempotent) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    const Client laptop = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    ASSERT_TRUE(link(user, phone, laptop).ok());
    const TimeMs later = anvil::db::now_ms() + std::chrono::seconds{3};

    EXPECT_TRUE(devices().unlink_device(db(), user, laptop.device.id, later).value());
    chat::IdentityRecord row = identity(user);
    EXPECT_EQ(row.device_set_version, 3);
    EXPECT_EQ(row.pending_since, later);
    ASSERT_EQ(row.devices.size(), 1U);
    EXPECT_EQ(row.devices[0].published.id, phone.device.id);

    // A repeat changes nothing: no version for a change that did not happen.
    EXPECT_FALSE(devices().unlink_device(db(), user, laptop.device.id, later).value());
    EXPECT_EQ(identity(user).device_set_version, 3);
}

TEST_F(ChatDevicesDb, RevokingASessionUnlinksTheDeviceItRegistered) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    const Client laptop = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    ASSERT_TRUE(link(user, phone, laptop).ok());

    const auto unlinked =
        devices().unlink_session(db(), user, laptop.device.session, anvil::db::now_ms());
    ASSERT_TRUE(unlinked.ok());
    ASSERT_TRUE(unlinked.value().has_value());
    EXPECT_EQ(*unlinked.value(), laptop.device.id);
    const chat::IdentityRecord row = identity(user);
    ASSERT_EQ(row.devices.size(), 1U);
    EXPECT_EQ(row.devices[0].published.id, phone.device.id);

    // Another account's session, or one that registered nothing, unlinks nothing.
    EXPECT_FALSE(
        devices()
            .unlink_session(
                db(), anvil::uuid::generate_v4(), phone.device.session, anvil::db::now_ms())
            .value()
            .has_value());
    EXPECT_FALSE(devices()
                     .unlink_session(db(), user, laptop.device.session, anvil::db::now_ms())
                     .value()
                     .has_value());
    EXPECT_EQ(count(user), 1U);
}

// --- reading ------------------------------------------------------------------------

TEST_F(ChatDevicesDb, DevicesOfPublishesAVerifiableChainAndIsBounded) {
    const Uuid alice = anvil::uuid::generate_v4();
    const Uuid bob = anvil::uuid::generate_v4();
    const Client phone = make_client();
    const Client laptop = make_client();
    ASSERT_TRUE(first(alice, phone).ok());
    ASSERT_TRUE(link(alice, phone, laptop).ok());
    ASSERT_TRUE(first(bob, make_client()).ok());

    const std::array<Uuid, 3> users{alice, bob, anvil::uuid::generate_v4()};
    const auto listed = devices().devices_of(db(), users, 8);
    ASSERT_TRUE(listed.ok());
    ASSERT_EQ(listed.value().size(), 2U);
    const chat::AccountDevices& account =
        listed.value()[0].user == alice ? listed.value()[0] : listed.value()[1];
    ASSERT_EQ(account.devices.size(), 2U);
    EXPECT_EQ(account.device_set_version, 2);

    // What a client does with the answer: verify the laptop was admitted by
    // the phone, under the phone's key from this same list.
    const chat::PublishedDevice& joined = account.devices[1];
    ASSERT_TRUE(joined.link.has_value());
    EXPECT_TRUE(crypto::ed25519_verify(account.devices[0].keys.signing,
                                       chat::link_message(alice,
                                                          joined.id,
                                                          joined.keys.agreement,
                                                          joined.keys.signing,
                                                          joined.link->timestamp_s),
                                       joined.link->signature));
    EXPECT_EQ(chat::validate_device(joined.id, joined.keys).code(), ErrorCode::Ok);

    // More accounts than the caller's bound is refused, not truncated.
    EXPECT_EQ(devices().devices_of(db(), users, 2).code(), ErrorCode::ValidationFailed);
}

// --- last seen ----------------------------------------------------------------------

TEST_F(ChatDevicesDb, TouchIsCoalesced) {
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    const TimeMs linked = anvil::db::now_ms();
    ASSERT_TRUE(first(user, phone, linked).ok());

    // Inside the interval: no write at all.
    EXPECT_FALSE(devices()
                     .touch(db(), user, phone.device.id, linked + std::chrono::minutes{10})
                     .value());
    EXPECT_EQ(identity(user).devices[0].last_seen, linked);

    const TimeMs later = linked + std::chrono::minutes{61};
    EXPECT_TRUE(devices().touch(db(), user, phone.device.id, later).value());
    EXPECT_EQ(identity(user).devices[0].last_seen, later);
    EXPECT_FALSE(
        devices().touch(db(), user, phone.device.id, later + std::chrono::minutes{1}).value());
    // Not another account's device, and not the version: last_seen is not a
    // change to the set.
    EXPECT_FALSE(devices()
                     .touch(db(),
                            anvil::uuid::generate_v4(),
                            phone.device.id,
                            later + std::chrono::hours{5})
                     .value());
    EXPECT_EQ(identity(user).device_set_version, 1);
}

TEST_F(ChatDevicesDb, TheIdleSweeperUnlinksOnlyTheIdle) {
    const Uuid user = anvil::uuid::generate_v4();
    const Uuid other = anvil::uuid::generate_v4();
    const Client phone = make_client();
    const Client laptop = make_client();
    const Client fresh = make_client();
    const TimeMs now = anvil::db::now_ms();
    const TimeMs long_ago = now - std::chrono::hours{24 * 40};
    ASSERT_TRUE(first(user, phone, long_ago).ok());
    ASSERT_TRUE(link(user, phone, laptop, long_ago).ok());
    ASSERT_TRUE(first(other, fresh, now).ok());
    // The laptop is in use; the phone has not been seen for forty days.
    ASSERT_TRUE(devices().touch(db(), user, laptop.device.id, now).value());

    const auto swept = devices().unlink_idle(db(), now, chat::kDefaultIdleDays, 16);
    ASSERT_TRUE(swept.ok());
    ASSERT_EQ(swept.value().size(), 1U);
    EXPECT_EQ(swept.value()[0].user, user);
    EXPECT_EQ(swept.value()[0].device, phone.device.id);

    const chat::IdentityRecord row = identity(user);
    ASSERT_EQ(row.devices.size(), 1U);
    EXPECT_EQ(row.devices[0].published.id, laptop.device.id);
    EXPECT_EQ(row.device_set_version, 3);
    EXPECT_EQ(row.pending_since, now);
    EXPECT_EQ(count(other), 1U);
    // A second pass finds nothing.
    EXPECT_TRUE(devices().unlink_idle(db(), now, chat::kDefaultIdleDays, 16).value().empty());
}

// --- the key directory --------------------------------------------------------------

TEST_F(ChatDevicesDb, TwoConcurrentClaimsNeverReturnTheSameKey) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    constexpr std::size_t kKeys = 50;
    ASSERT_TRUE(prekeys().upload(db(), user, phone.device.id, batch(kKeys, 1)).ok());

    // More claims than keys, from separate clients at once: every key is
    // handed out exactly once and the rest fall back to the last-resort key.
    constexpr int kClaimers = 8;
    constexpr int kClaimsEach = 10;
    std::vector<std::future<std::vector<std::uint32_t>>> claimers;
    claimers.reserve(kClaimers);
    for (int c = 0; c < kClaimers; ++c) {
        claimers.push_back(std::async(std::launch::async, [this, user] {
            auto client = anvil::db::MongoPool::instance().acquire();
            std::vector<std::uint32_t> taken;
            for (int i = 0; i < kClaimsEach; ++i) {
                const auto bundles = prekeys().claim(*client, user);
                EXPECT_TRUE(bundles.ok());
                if (!bundles.ok() || bundles.value().size() != 1) {
                    continue;
                }
                if (bundles.value()[0].one_time.has_value()) {
                    taken.push_back(bundles.value()[0].one_time->id);
                }
            }
            return taken;
        }));
    }
    std::multiset<std::uint32_t> handed;
    for (auto& claimer : claimers) {
        for (const std::uint32_t id : claimer.get()) {
            handed.insert(id);
        }
    }
    EXPECT_EQ(handed.size(), kKeys);
    EXPECT_EQ(std::set<std::uint32_t>(handed.begin(), handed.end()).size(), kKeys)
        << "a one-time key was handed to two senders";
    EXPECT_EQ(prekeys().remaining(db(), user, phone.device.id).value(), 0);
}

TEST_F(ChatDevicesDb, AnEmptyPoolAnswersTheLastResortKeyAndAnUploadClearsLow) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());

    const auto empty = prekeys().claim(db(), user);
    ASSERT_TRUE(empty.ok());
    ASSERT_EQ(empty.value().size(), 1U);
    const chat::ClaimedBundle& bundle = empty.value()[0];
    EXPECT_EQ(bundle.device, phone.device.id);
    EXPECT_FALSE(bundle.one_time.has_value());
    // The sender gets a key it can verify before trusting it.
    EXPECT_EQ(bundle.keys.last_resort, phone.device.keys.last_resort);
    EXPECT_TRUE(crypto::ed25519_verify(
        bundle.keys.signing,
        chat::prekey_message(chat::kLastResortDomain, bundle.device, bundle.keys.last_resort),
        bundle.keys.last_resort_signature));
    EXPECT_TRUE(identity(user).devices[0].low);

    ASSERT_TRUE(prekeys().upload(db(), user, phone.device.id, batch(3, 7)).ok());
    EXPECT_FALSE(identity(user).devices[0].low);
    const auto claimed = prekeys().claim(db(), user);
    ASSERT_TRUE(claimed.ok());
    ASSERT_TRUE(claimed.value()[0].one_time.has_value());
    EXPECT_GE(claimed.value()[0].one_time->id, 7U);
    EXPECT_EQ(prekeys().remaining(db(), user, phone.device.id).value(), 2);
    EXPECT_FALSE(identity(user).devices[0].low);
}

TEST_F(ChatDevicesDb, AClaimAnswersEveryCurrentDeviceAndNoUnlinkedOne) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    const Client laptop = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    ASSERT_TRUE(link(user, phone, laptop).ok());
    ASSERT_TRUE(prekeys().upload(db(), user, phone.device.id, batch(5, 1)).ok());
    ASSERT_TRUE(prekeys().upload(db(), user, laptop.device.id, batch(5, 1)).ok());

    const auto both = prekeys().claim(db(), user);
    ASSERT_TRUE(both.ok());
    ASSERT_EQ(both.value().size(), 2U);
    EXPECT_TRUE(both.value()[0].one_time.has_value());
    EXPECT_TRUE(both.value()[1].one_time.has_value());

    // The laptop is unlinked and its rows are still there: the claim walks the
    // identity document, so they are unreachable before anything deletes them.
    ASSERT_TRUE(
        devices().unlink_device(db(), user, laptop.device.id, anvil::db::now_ms()).value());
    EXPECT_EQ(prekeys().remaining(db(), user, laptop.device.id).value(), 4);
    const auto one = prekeys().claim(db(), user);
    ASSERT_TRUE(one.ok());
    ASSERT_EQ(one.value().size(), 1U);
    EXPECT_EQ(one.value()[0].device, phone.device.id);
    EXPECT_EQ(prekeys().remaining(db(), user, laptop.device.id).value(), 4);

    // The follow-up, idempotently.
    EXPECT_TRUE(prekeys().discard(db(), user, laptop.device.id).ok());
    EXPECT_TRUE(prekeys().discard(db(), user, laptop.device.id).ok());
    EXPECT_EQ(prekeys().remaining(db(), user, laptop.device.id).value(), 0);
    EXPECT_EQ(prekeys().remaining(db(), user, phone.device.id).value(), 3);

    // An account with no device has nothing to claim.
    const auto nobody = prekeys().claim(db(), anvil::uuid::generate_v4());
    ASSERT_TRUE(nobody.ok());
    EXPECT_TRUE(nobody.value().empty());
}

TEST_F(ChatDevicesDb, UploadsAreBatchedBoundedValidatedAndAllOrNothing) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    const Uuid device = phone.device.id;
    const auto refused =
        [&](std::span<const chat::OneTimePrekey> keys, ErrorCode code, std::string_view field) {
            const anvil::Status status = prekeys().upload(db(), user, device, keys);
            EXPECT_EQ(status.code(), code) << field;
            if (!status.ok()) {
                EXPECT_EQ(status.error().field, field);
            }
        };

    refused({}, ErrorCode::ValidationFailed, chat::prekey_inputs::kPrekeys);
    refused(batch(chat::kMaxPrekeyBatch + 1, 1),
            ErrorCode::ValidationFailed,
            chat::prekey_inputs::kPrekeys);
    {
        std::vector<chat::OneTimePrekey> keys = batch(4, 1);
        keys[2].key.fill(0);
        refused(keys, ErrorCode::ValidationFailed, chat::prekey_inputs::kPrekey);
    }
    {
        std::vector<chat::OneTimePrekey> keys = batch(4, 1);
        keys[3].id = keys[0].id;
        refused(keys, ErrorCode::ValidationFailed, chat::prekey_inputs::kKeyId);
    }
    {
        // Another account's device, through this account's session.
        const Uuid other = anvil::uuid::generate_v4();
        const Client theirs = make_client();
        ASSERT_TRUE(first(other, theirs).ok());
        const anvil::Status status =
            prekeys().upload(db(), user, theirs.device.id, batch(1, 1));
        EXPECT_EQ(status.code(), ErrorCode::Forbidden);
        EXPECT_EQ(prekeys().remaining(db(), other, theirs.device.id).value(), 0);
    }
    EXPECT_EQ(prekeys().remaining(db(), user, device).value(), 0);

    // Nine full batches, then one that repeats a stored id: refused whole, so
    // neither its other ninety-nine keys nor its reservation survive.
    for (std::uint32_t b = 0; b < 9; ++b) {
        ASSERT_TRUE(prekeys().upload(db(), user, device, batch(100, b * 100)).ok()) << b;
    }
    refused(batch(100, 850), ErrorCode::Conflict, chat::prekey_inputs::kKeyId);
    EXPECT_EQ(prekeys().remaining(db(), user, device).value(), 900);

    // The bound is exact: the last hundred fit, one more does not.
    ASSERT_TRUE(prekeys().upload(db(), user, device, batch(100, 900)).ok());
    refused(batch(1, 5000), ErrorCode::Conflict, chat::prekey_inputs::kPrekeys);
    EXPECT_EQ(prekeys().remaining(db(), user, device).value(), chat::kMaxPrekeysPerDevice);

    // A claim gives one slot back, and exactly one.
    ASSERT_TRUE(prekeys().claim(db(), user).value()[0].one_time.has_value());
    EXPECT_TRUE(prekeys().upload(db(), user, device, batch(1, 5000)).ok());
    refused(batch(1, 5001), ErrorCode::Conflict, chat::prekey_inputs::kPrekeys);
}

TEST_F(ChatDevicesDb, ConcurrentUploadsCannotOverfillADevice) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid user = anvil::uuid::generate_v4();
    const Client phone = make_client();
    ASSERT_TRUE(first(user, phone).ok());
    for (std::uint32_t b = 0; b < 8; ++b) {
        ASSERT_TRUE(prekeys().upload(db(), user, phone.device.id, batch(100, b * 100)).ok());
    }
    // Room for two more batches and six racing for it. A count followed by an
    // insert lets every one of them through.
    constexpr int kRacers = 6;
    std::vector<std::future<anvil::Status>> outcomes;
    outcomes.reserve(kRacers);
    for (int r = 0; r < kRacers; ++r) {
        outcomes.push_back(std::async(std::launch::async, [this, user, &phone, r] {
            auto client = anvil::db::MongoPool::instance().acquire();
            return prekeys().upload(*client,
                                    user,
                                    phone.device.id,
                                    batch(100, 10000 + static_cast<std::uint32_t>(r) * 100));
        }));
    }
    int stored = 0;
    for (auto& outcome : outcomes) {
        stored += outcome.get().ok() ? 1 : 0;
    }
    EXPECT_EQ(stored, 2);
    EXPECT_EQ(prekeys().remaining(db(), user, phone.device.id).value(),
              chat::kMaxPrekeysPerDevice);
}

}  // namespace
