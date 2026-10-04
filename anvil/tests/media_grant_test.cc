// Grants: the server's agreement to serve one private object, sealed so the
// media origin can check it without a session (docs/22-chat.md §6.1).
//
// Every case here is a property a grant has to keep while being a URL segment
// anyone can edit: it opens only as itself, only until it expires, only under a
// key the server holds, and it never shows the client the object id it names.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <drogon/HttpTypes.h>

#include "anvil/core/uuid.h"
#include "anvil/crypto/base64url.h"
#include "anvil/fs/paths.h"
#include "anvil/media/grant.h"
#include "anvil/media/serving.h"
#include "namespaces.h"

namespace {

namespace media = anvil::media;
namespace fs = anvil::fs;
using anvil::Uuid;

constexpr std::int64_t kNow = 1'800'000'123;

[[nodiscard]] std::array<std::uint8_t, media::GrantKeys::kKeyBytes> key_of(std::uint8_t fill) {
    std::array<std::uint8_t, media::GrantKeys::kKeyBytes> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(fill + i);
    }
    return key;
}

[[nodiscard]] Uuid object_id() {
    Uuid id{};
    for (std::size_t i = 0; i < id.size(); ++i) { id[i] = static_cast<std::uint8_t>(0xA0 + i); }
    return id;
}

TEST(MediaGrant, OpensAsTheObjectItWasMintedFor) {
    const media::GrantKeys keys{1, key_of(0x10)};
    const std::string grant = media::mint_grant(keys, testapp::kChat, object_id(), kNow);
    EXPECT_EQ(grant.size(), media::kGrantChars);

    const auto opened = media::open_grant(keys, grant, kNow);
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(opened->id(), object_id());
    EXPECT_EQ(opened->ns(), testapp::kChat);
}

TEST(MediaGrant, IsTheSameUrlWithinABucketSoTheBrowserCacheHits) {
    const media::GrantKeys keys{1, key_of(0x10)};
    const std::int64_t bucket_start = (kNow / media::kGrantBucketSeconds) * media::kGrantBucketSeconds;
    const std::string first = media::mint_grant(keys, testapp::kChat, object_id(), bucket_start);
    const std::string later = media::mint_grant(keys, testapp::kChat, object_id(),
                                                bucket_start + media::kGrantBucketSeconds - 1);
    EXPECT_EQ(first, later);

    // The next bucket is a new URL, which is what bounds how long one lives.
    const std::string next = media::mint_grant(keys, testapp::kChat, object_id(),
                                               bucket_start + media::kGrantBucketSeconds);
    EXPECT_NE(first, next);

    // And another object is never the same URL.
    Uuid other = object_id();
    other[15] ^= 0x01U;
    EXPECT_NE(first, media::mint_grant(keys, testapp::kChat, other, bucket_start));
}

TEST(MediaGrant, LivesBetweenOneAndTwoBucketsAndNotASecondLonger) {
    const media::GrantKeys keys{1, key_of(0x10)};
    const std::string grant = media::mint_grant(keys, testapp::kChat, object_id(), kNow);
    const std::int64_t expires = media::grant_expiry(kNow);
    EXPECT_GT(expires - kNow, media::kGrantBucketSeconds - 1);
    EXPECT_LE(expires - kNow, 2 * media::kGrantBucketSeconds);

    EXPECT_TRUE(media::open_grant(keys, grant, expires - 1).has_value());
    // That upper bound is the revocation latency for a removed member, so the
    // boundary is exact rather than tolerant.
    EXPECT_FALSE(media::open_grant(keys, grant, expires).has_value());
}

TEST(MediaGrant, NeverShowsTheClientTheObjectId) {
    // A client is told it can pull this and nothing taken from storage. The id
    // sealed inside must not be readable in any alignment of the decoded bytes.
    const media::GrantKeys keys{1, key_of(0x10)};
    const std::string grant = media::mint_grant(keys, testapp::kChat, object_id(), kNow);
    const auto raw = anvil::crypto::base64url_decode(grant);
    ASSERT_TRUE(raw.has_value());
    const Uuid id = object_id();
    EXPECT_EQ(std::search(raw->begin(), raw->end(), id.begin(), id.end()), raw->end());
    EXPECT_EQ(grant.find(anvil::uuid::to_string(id)), std::string::npos);
}

TEST(MediaGrant, AnyEditedCharacterIsRefused) {
    const media::GrantKeys keys{1, key_of(0x10)};
    const std::string grant = media::mint_grant(keys, testapp::kChat, object_id(), kNow);
    for (std::size_t i = 0; i < grant.size(); ++i) {
        std::string edited = grant;
        edited[i] = edited[i] == 'A' ? 'B' : 'A';
        // The last character carries only some of a byte's bits; an edit there
        // that leaves those bits alone decodes to the same bytes, and is the
        // same grant rather than a forgery.
        const auto same = anvil::crypto::base64url_decode(edited);
        const auto original = anvil::crypto::base64url_decode(grant);
        if (same.has_value() && original.has_value() && *same == *original) { continue; }
        EXPECT_FALSE(media::open_grant(keys, edited, kNow).has_value()) << "position " << i;
    }
}

TEST(MediaGrant, AWrongLengthOrAlphabetIsRefusedBeforeAnythingDecodes) {
    const media::GrantKeys keys{1, key_of(0x10)};
    const std::string grant = media::mint_grant(keys, testapp::kChat, object_id(), kNow);
    EXPECT_FALSE(media::open_grant(keys, "", kNow).has_value());
    EXPECT_FALSE(media::open_grant(keys, grant.substr(1), kNow).has_value());
    EXPECT_FALSE(media::open_grant(keys, grant + "A", kNow).has_value());
    std::string slashed = grant;
    slashed[10] = '/';
    EXPECT_FALSE(media::open_grant(keys, slashed, kNow).has_value());
    EXPECT_FALSE(media::open_grant(keys, std::string(media::kGrantChars, '.'), kNow).has_value());
}

TEST(MediaGrant, AnotherKeyOpensNothing) {
    const media::GrantKeys minted_under{1, key_of(0x10)};
    const media::GrantKeys other_key_same_id{1, key_of(0x20)};
    const media::GrantKeys unknown_id{2, key_of(0x10)};
    const std::string grant = media::mint_grant(minted_under, testapp::kChat, object_id(), kNow);
    EXPECT_FALSE(media::open_grant(other_key_same_id, grant, kNow).has_value());
    EXPECT_FALSE(media::open_grant(unknown_id, grant, kNow).has_value());
}

TEST(MediaGrant, RotationKeepsAGrantFromThePreviousKeyWorking) {
    const media::GrantKeys before{1, key_of(0x10)};
    const std::string grant = media::mint_grant(before, testapp::kChat, object_id(), kNow);

    const auto next = key_of(0x30);
    const auto previous = key_of(0x10);
    const media::GrantKeys after{2, next, 1, previous};
    EXPECT_TRUE(media::open_grant(after, grant, kNow).has_value());
    // New grants carry the new id.
    const std::string minted = media::mint_grant(after, testapp::kChat, object_id(), kNow);
    EXPECT_NE(minted, grant);
    EXPECT_FALSE(media::open_grant(before, minted, kNow).has_value());
}

TEST(MediaGrant, KeysOfTheWrongSizeOrSharingAnIdAreRefusedAtConstruction) {
    const std::array<std::uint8_t, 32> short_key{};
    EXPECT_THROW((media::GrantKeys{1, short_key}), std::invalid_argument);
    const auto key = key_of(0x10);
    EXPECT_THROW((media::GrantKeys{1, key, 1, key}), std::invalid_argument);
}

TEST(MediaGrant, APrivateNamespaceIsNeverServedOnAnIdAlone) {
    const auto by_id = media::accel_redirect_response(testapp::kChat, object_id(),
                                                      fs::kMasterVariant, fs::Mime::Jpeg);
    EXPECT_EQ(by_id->statusCode(), drogon::k404NotFound);
    EXPECT_TRUE(by_id->getHeader("X-Accel-Redirect").empty());

    const media::GrantKeys keys{1, key_of(0x10)};
    const auto grant =
        media::open_grant(keys, media::mint_grant(keys, testapp::kChat, object_id(), kNow), kNow);
    ASSERT_TRUE(grant.has_value());
    const auto by_grant =
        media::accel_redirect_response(*grant, fs::kMasterVariant, fs::Mime::Jpeg);
    EXPECT_EQ(by_grant->statusCode(), drogon::k200OK);
    EXPECT_FALSE(by_grant->getHeader("X-Accel-Redirect").empty());

    // A public namespace is served exactly as it always was.
    const auto public_ns = media::accel_redirect_response(testapp::kMedia, object_id(),
                                                          fs::kMasterVariant, fs::Mime::Jpeg);
    EXPECT_EQ(public_ns->statusCode(), drogon::k200OK);
}

TEST(MediaGrant, ASealedBlobIsServedOnlyOnAGrantAndOnlyAsAnOpaqueAttachment) {
    // Private, so an id alone gets the stealth 404.
    const auto by_id = media::accel_redirect_response(testapp::kSealed, object_id(),
                                                      fs::kMasterVariant, fs::Mime::Sealed);
    EXPECT_EQ(by_id->statusCode(), drogon::k404NotFound);
    EXPECT_TRUE(by_id->getHeader("X-Accel-Redirect").empty());

    const media::GrantKeys keys{1, key_of(0x10)};
    const auto grant = media::open_grant(
        keys, media::mint_grant(keys, testapp::kSealed, object_id(), kNow), kNow);
    ASSERT_TRUE(grant.has_value());
    const auto served =
        media::accel_redirect_response(*grant, fs::kMasterVariant, fs::Mime::Sealed);
    EXPECT_EQ(served->statusCode(), drogon::k200OK);
    EXPECT_EQ(served->contentTypeString(), "application/octet-stream");
    EXPECT_EQ(served->getHeader("Content-Disposition"), "attachment");
    EXPECT_EQ(served->getHeader("Content-Security-Policy"),
              std::string{media::kSealedContentSecurityPolicy});
    EXPECT_EQ(served->getHeader("Content-Security-Policy").find("media-src"), std::string::npos);
    EXPECT_EQ(served->getHeader("X-Content-Type-Options"), "nosniff");
    EXPECT_TRUE(served->getHeader("Vary").empty());

    // A row that says otherwise cannot make ciphertext renderable: the
    // namespace decides the type, the disposition and the file.
    const auto lying = media::accel_redirect_response(
        *grant, fs::VariantKey{640, fs::Format::Webp}, fs::Mime::Pdf);
    EXPECT_EQ(lying->contentTypeString(), "application/octet-stream");
    EXPECT_EQ(lying->getHeader("Content-Disposition"), "attachment");
    EXPECT_EQ(lying->getHeader("X-Accel-Redirect"), served->getHeader("X-Accel-Redirect"));
}

// --- upload handles -----------------------------------------------------------

TEST(UploadHandle, OpensOnlyForTheAccountThatUploaded) {
    const media::GrantKeys keys{1, key_of(0x10)};
    const Uuid alice = anvil::uuid::generate_v4();
    const Uuid bob = anvil::uuid::generate_v4();
    const std::string handle =
        media::mint_upload_handle(keys, testapp::kChat, object_id(), alice, kNow);
    EXPECT_EQ(handle.size(), media::kUploadHandleChars);

    const auto claimed = media::open_upload_handle(keys, handle, alice, kNow);
    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->id, object_id());
    EXPECT_EQ(claimed->ns, testapp::kChat);

    // Passed to another account, it is refused exactly like a forgery: an
    // object id learned anywhere cannot become someone else's attachment.
    EXPECT_FALSE(media::open_upload_handle(keys, handle, bob, kNow).has_value());
}

TEST(UploadHandle, ExpiresAfterAnHour) {
    const media::GrantKeys keys{1, key_of(0x10)};
    const Uuid alice = anvil::uuid::generate_v4();
    const std::string handle =
        media::mint_upload_handle(keys, testapp::kChat, object_id(), alice, kNow);
    EXPECT_TRUE(
        media::open_upload_handle(keys, handle, alice, kNow + media::kUploadHandleSeconds - 1)
            .has_value());
    EXPECT_FALSE(
        media::open_upload_handle(keys, handle, alice, kNow + media::kUploadHandleSeconds)
            .has_value());
}

TEST(UploadHandle, NeverShowsTheObjectIdAndNeverOpensAsAGrant) {
    const media::GrantKeys keys{1, key_of(0x10)};
    const Uuid alice = anvil::uuid::generate_v4();
    const std::string handle =
        media::mint_upload_handle(keys, testapp::kChat, object_id(), alice, kNow);
    const auto raw = anvil::crypto::base64url_decode(handle);
    ASSERT_TRUE(raw.has_value());
    const Uuid id = object_id();
    EXPECT_EQ(std::search(raw->begin(), raw->end(), id.begin(), id.end()), raw->end());

    // The purposes are separate associated data, so a handle is never a way to
    // be served the object, and a grant is never a way to attach one. Their
    // lengths differ too, so the purpose is what is being tested: the same
    // body sealed for the other purpose would still fail.
    EXPECT_FALSE(media::open_grant(keys, handle.substr(0, media::kGrantChars), kNow).has_value());
    const std::string grant = media::mint_grant(keys, testapp::kChat, object_id(), kNow);
    EXPECT_FALSE(media::open_upload_handle(keys, grant + std::string(21, 'A'), alice, kNow)
                     .has_value());
}

TEST(UploadHandle, AnyEditedCharacterIsRefused) {
    const media::GrantKeys keys{1, key_of(0x10)};
    const Uuid alice = anvil::uuid::generate_v4();
    const std::string handle =
        media::mint_upload_handle(keys, testapp::kChat, object_id(), alice, kNow);
    const auto original = anvil::crypto::base64url_decode(handle);
    for (std::size_t i = 0; i < handle.size(); ++i) {
        std::string edited = handle;
        edited[i] = edited[i] == 'A' ? 'B' : 'A';
        const auto same = anvil::crypto::base64url_decode(edited);
        if (same.has_value() && original.has_value() && *same == *original) { continue; }
        EXPECT_FALSE(media::open_upload_handle(keys, edited, alice, kNow).has_value())
            << "position " << i;
    }
}

}  // namespace
