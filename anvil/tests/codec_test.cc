// BSON codecs.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/json.hpp>

#include <bsoncxx/types.hpp>

#include "anvil/core/perm_set.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

constexpr Uuid kId{0x01, 0x8f, 0x3a, 0x2b, 0x00, 0x11, 0x22, 0x33,
                   0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};

// --- 1: identifiers are 16-byte BinData(4), never a string ----------------

TEST(Codec, UuidEncodesAsBinarySubtypeFourOfSixteenBytes) {
    bsoncxx::builder::basic::document doc;
    codec::append_uuid(doc, "_id", kId);

    const bsoncxx::document::element stored = doc.view()["_id"];
    ASSERT_TRUE(stored);
    ASSERT_EQ(stored.type(), bsoncxx::type::k_binary);

    const bsoncxx::types::b_binary binary = stored.get_binary();
    EXPECT_EQ(binary.sub_type, bsoncxx::binary_sub_type::k_uuid);
    EXPECT_EQ(binary.size, 16U) << "16 raw bytes, not 36 characters";
    EXPECT_EQ(std::memcmp(binary.bytes, kId.data(), kId.size()), 0);

    const Result<Uuid> back = codec::read_uuid(doc.view(), "_id");
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value(), kId);
}

TEST(Codec, UuidStoredAsAStringDoesNotDecode) {
    // A 36-character id must fail, not be parsed back into 16 bytes: silently
    // accepting it would let the expensive representation survive forever, and
    // would let a writer outside this codec choose the storage format.
    const auto doc = make_document(kvp("_id", "018f3a2b-0011-2233-4455-66778899aabb"));
    const Result<Uuid> decoded = codec::read_uuid(doc.view(), "_id");
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.code(), ErrorCode::Internal);
}

TEST(Codec, UuidRejectsTheLegacyBinarySubtypeAndWrongLengths) {
    // Subtype 3 is the old driver-specific UUID whose byte order differs
    // between drivers. Accepting it would mean two documents with the same
    // logical id comparing unequal.
    const std::array<std::uint8_t, 16> raw = kId;
    const auto legacy = make_document(
        kvp("id", bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_uuid_deprecated, 16,
                                           raw.data()}));
    EXPECT_FALSE(codec::read_uuid(legacy.view(), "id").ok());

    const auto short_id = make_document(
        kvp("id", bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_uuid, 8, raw.data()}));
    EXPECT_FALSE(codec::read_uuid(short_id.view(), "id").ok());
}

TEST(Codec, OptionalUuidRoundTripsNull) {
    bsoncxx::builder::basic::document doc;
    codec::append_optional_uuid(doc, "sub", std::optional<Uuid>{});
    codec::append_optional_uuid(doc, "owner", std::optional<Uuid>{kId});

    const Result<std::optional<Uuid>> absent = codec::read_optional_uuid(doc.view(), "sub");
    ASSERT_TRUE(absent.ok());
    EXPECT_FALSE(absent.value().has_value());

    const Result<std::optional<Uuid>> present = codec::read_optional_uuid(doc.view(), "owner");
    ASSERT_TRUE(present.ok());
    ASSERT_TRUE(present.value().has_value());
    EXPECT_EQ(*present.value(), kId);
}

// --- 2: permission sets ---------------------------------------------------

TEST(Codec, PermSetRoundTripsAllOneHundredAndTwentyEightBits) {
    for (std::size_t bit = 0; bit < PermSet::kBits; ++bit) {
        PermSet perms;
        perms.set(bit);

        bsoncxx::builder::basic::document doc;
        codec::append_perm_set(doc, "eff", perms);

        const bsoncxx::types::b_binary binary = doc.view()["eff"].get_binary();
        ASSERT_EQ(binary.sub_type, bsoncxx::binary_sub_type::k_binary);
        ASSERT_EQ(binary.size, 16U) << "128 bits in 16 bytes, never an array of names";

        const Result<PermSet> back = codec::read_perm_set(doc.view(), "eff");
        ASSERT_TRUE(back.ok()) << "bit " << bit;
        EXPECT_EQ(back.value(), perms) << "bit " << bit;
        EXPECT_TRUE(back.value().test(bit));
        EXPECT_EQ(back.value().count(), 1U);
    }
}

TEST(Codec, PermSetRejectsAWrongWidthBinary) {
    const std::array<std::uint8_t, 8> half{};
    const auto doc = make_document(
        kvp("eff",
            bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary, 8, half.data()}));
    EXPECT_FALSE(codec::read_perm_set(doc.view(), "eff").ok());
}

// --- 4: timestamps --------------------------------------------------------

TEST(Codec, TimestampsEncodeAsBsonDateInUtc) {
    const TimeMs at{std::chrono::milliseconds{1'770'000'123'456}};

    bsoncxx::builder::basic::document doc;
    codec::append_time(doc, "created_at", at);

    const bsoncxx::document::element stored = doc.view()["created_at"];
    ASSERT_EQ(stored.type(), bsoncxx::type::k_date)
        << "never a string: a string timestamp carries a zone, or implies a local one";
    EXPECT_EQ(stored.get_date().to_int64(), 1'770'000'123'456);

    const Result<TimeMs> back = codec::read_time(doc.view(), "created_at");
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value(), at);
}

TEST(Codec, TimestampStoredAsAStringOrIntegerDoesNotDecode) {
    const auto as_string = make_document(kvp("at", "2026-08-04T00:00:00Z"));
    EXPECT_FALSE(codec::read_time(as_string.view(), "at").ok());

    const auto as_int = make_document(kvp("at", bsoncxx::types::b_int64{1'770'000'123'456}));
    EXPECT_FALSE(codec::read_time(as_int.view(), "at").ok());
}

// --- 5, 6: localized text -------------------------------------------------

TEST(Codec, LocalizedTextRequiresBothLanguages) {
    bsoncxx::builder::basic::document doc;
    const Status missing_ar =
        codec::append_localized(doc, "t", LocalizedView{{"Welcome", ""}});
    EXPECT_FALSE(missing_ar.ok()) << "a missing ar is a validation error, never a fallback";

    const auto stored_without_ar = make_document(kvp("t", [](sub_document sub) {
        sub.append(kvp("en", "Welcome"));
    }));
    EXPECT_FALSE(codec::read_localized(stored_without_ar.view(), "t").ok());

    const auto stored_with_empty_ar = make_document(kvp("t", [](sub_document sub) {
        sub.append(kvp("en", "Welcome"));
        sub.append(kvp("ar", ""));
    }));
    EXPECT_FALSE(codec::read_localized(stored_with_empty_ar.view(), "t").ok());
}

TEST(Codec, ArabicRoundTripsAsRawUtf8) {
    constexpr std::string_view kArabic = "أهلاً بك في يارد كلوب";

    bsoncxx::builder::basic::document doc;
    ASSERT_TRUE(codec::append_localized(doc, "t", LocalizedView{{"Welcome", kArabic}}).ok());

    const Result<LocalizedView> back = codec::read_localized(doc.view(), "t");
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value().values[1], kArabic);
    EXPECT_EQ(back.value().get(*Locale::from_tag("ar")), kArabic);

    // Stored as the same bytes, not as \uXXXX escapes.
    const std::string json = bsoncxx::to_json(doc.view());
    EXPECT_NE(json.find(kArabic), std::string::npos);
    EXPECT_EQ(json.find("\\u"), std::string::npos);
}

TEST(Codec, MalformedStoredTextIsRejectedNeverRepaired) {
    // Rejected, never substituted with U+FFFD: repair destroys the signal that
    // something wrote invalid data.
    const std::string_view lone_surrogate{"\xED\xA0\x80", 3};
    const auto doc = make_document(kvp("t", lone_surrogate));
    EXPECT_FALSE(codec::read_text(doc.view(), "t").ok());
}

// --- 7: nothing is ever coerced -------------------------------------------

TEST(Codec, UnexpectedTypesErrorRatherThanCoerce) {
    const auto doc = make_document(kvp("n", "1"), kvp("flag", bsoncxx::types::b_int32{1}),
                                   kvp("count", bsoncxx::types::b_int32{7}),
                                   kvp("text", bsoncxx::types::b_int32{5}));

    // "1" is not 1, int32 1 is not true, and an int32 version is not an int64
    // one: every one of these would be a plausible coercion, and every one of
    // them would launder a document written by something other than this codec
    // into a valid-looking value.
    EXPECT_FALSE(codec::read_int32(doc.view(), "n").ok());
    EXPECT_FALSE(codec::read_bool(doc.view(), "flag").ok());
    EXPECT_FALSE(codec::read_int64(doc.view(), "count").ok());
    EXPECT_FALSE(codec::read_text(doc.view(), "text").ok());
    EXPECT_FALSE(codec::read_uuid(doc.view(), "count").ok());
}

TEST(Codec, MissingFieldsFailTheSameWayAsWrongTypes) {
    const auto doc = make_document(kvp("present", bsoncxx::types::b_int32{1}));
    EXPECT_FALSE(codec::read_int32(doc.view(), "absent").ok());
    EXPECT_EQ(codec::read_int32(doc.view(), "absent").error().field, "absent")
        << "the failure names the field and never the value";
}

TEST(Codec, EnumsAreInt32AndRangeChecked) {
    bsoncxx::builder::basic::document doc;
    codec::append_enum(doc, "utype", UserType::FullControl);
    codec::append_enum(doc, "bad", static_cast<UserType>(9));

    ASSERT_EQ(doc.view()["utype"].type(), bsoncxx::type::k_int32)
        << "never the display string";

    const Result<UserType> good = codec::read_enum(doc.view(), "utype", UserType::SuperAdmin);
    ASSERT_TRUE(good.ok());
    EXPECT_EQ(good.value(), UserType::FullControl);

    EXPECT_FALSE(codec::read_enum(doc.view(), "bad", UserType::SuperAdmin).ok())
        << "an out-of-range enum is corruption, not a new variant";
}

TEST(Codec, DigestsAreThirtyTwoByteBinarySubtypeZero) {
    crypto::Digest256 digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        digest[i] = static_cast<std::uint8_t>(i);
    }

    bsoncxx::builder::basic::document doc;
    codec::append_digest(doc, "rt_hash", digest);

    const bsoncxx::types::b_binary binary = doc.view()["rt_hash"].get_binary();
    EXPECT_EQ(binary.sub_type, bsoncxx::binary_sub_type::k_binary);
    EXPECT_EQ(binary.size, 32U);

    const Result<crypto::Digest256> back = codec::read_digest(doc.view(), "rt_hash");
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value(), digest);
}

TEST(Codec, FixedWidthBytesRoundTripForPackedIpsAndUaHashes) {
    // 16 bytes for a v4-mapped v6 address and 8 for the truncated UA hash, not
    // the ~200-byte strings they replace.
    const std::array<std::uint8_t, 8> ua_hash{1, 2, 3, 4, 5, 6, 7, 8};

    bsoncxx::builder::basic::document doc;
    doc.append(kvp("ua_hash", codec::bytes_bin(ua_hash)));

    std::array<std::uint8_t, 8> back{};
    ASSERT_TRUE(codec::read_bytes(doc.view(), "ua_hash", back).ok());
    EXPECT_EQ(back, ua_hash);

    std::array<std::uint8_t, 16> wrong_width{};
    EXPECT_FALSE(codec::read_bytes(doc.view(), "ua_hash", wrong_width).ok());
}

// --- an application's stored enums ------------------------------------------
//
// The original asserted, exhaustively, that none of 63 audit-action values had
// ever moved: they are written to disk, retained for 400 days, and read back by
// dashboards that render historical rows, so renumbering one relabels every row
// already written. That is the worst failure a log can have, because the log
// exists to be believed.
//
// It is not anvil's assertion to make. The values belong to whoever declares the
// actions, exactly as permission bit indices belong to whoever declares the
// permissions — and it belongs in that application's own tests, where an
// exhaustive table is maintainable because the person adding an action is the
// person editing the file.
//
// What anvil owes is the mechanism, and read_enum is tested above: an out-of-range
// value is rejected rather than becoming a valid-looking enum, which is what makes
// an append-only table safe to read back years later.

}  // namespace
}  // namespace anvil::db

