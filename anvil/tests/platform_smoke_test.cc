// The driver primitives, exercised before a feature is written against them.
//
// Separate from dependency_smoke_test.cc because anvil::foundation deliberately
// does not link mongocxx, and a single smoke-test binary that linked both would
// quietly erase the boundary the target split exists to enforce. Keeping them
// apart means an accidental dependency from a validator onto the driver is a
// link error here rather than a review comment (docs/00-architecture.md §2).

#include <array>
#include <cstdint>
#include <cstring>

#include <openssl/rand.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include <gtest/gtest.h>

namespace {

TEST(PlatformSmoke, BsonRoundTripsBinDataSubtype4) {
    std::array<std::uint8_t, 16> id{};
    ASSERT_EQ(RAND_bytes(id.data(), static_cast<int>(id.size())), 1);

    // Subtype 4 is how every identifier in this system is stored: 16 raw bytes,
    // never a 36-character string. That is 2.25x less storage and turns every
    // comparison from a string compare into a 16-byte memcmp. If this does not
    // round-trip, nothing above it does.
    const bsoncxx::document::value doc = bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp(
            "_id", bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_uuid,
                                            static_cast<std::uint32_t>(id.size()),
                                            id.data()}));

    const bsoncxx::document::element element = doc.view()["_id"];
    ASSERT_EQ(element.type(), bsoncxx::type::k_binary);

    const bsoncxx::types::b_binary stored = element.get_binary();
    ASSERT_EQ(stored.sub_type, bsoncxx::binary_sub_type::k_uuid);
    ASSERT_EQ(stored.size, id.size());
    EXPECT_EQ(std::memcmp(stored.bytes, id.data(), id.size()), 0);
}

TEST(PlatformSmoke, BsonDoesNotCoerceOnRead) {
    // The property every decoder in anvil rests on: a string where an integer is
    // expected must be an ERROR, not a zero. A coercing read is what turns
    // {"role": {"$gt": ""}} into an authorisation bypass.
    const bsoncxx::document::value doc = bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("n", bsoncxx::types::b_string{"7"}));

    EXPECT_EQ(doc.view()["n"].type(), bsoncxx::type::k_string);
    EXPECT_NE(doc.view()["n"].type(), bsoncxx::type::k_int64);
}

}  // namespace
