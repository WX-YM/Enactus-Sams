#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>

#include "anvil/core/uuid.h"

namespace anvil::uuid {

TEST(Uuid, V4HasCorrectVersionAndVariant) {
    const Uuid id = generate_v4();
    EXPECT_EQ(id[6] & 0xF0U, 0x40U);
    EXPECT_EQ(id[8] & 0xC0U, 0x80U);
}

TEST(Uuid, V7HasCorrectVersionAndVariant) {
    const Uuid id = generate_v7();
    EXPECT_EQ(id[6] & 0xF0U, 0x70U);
    EXPECT_EQ(id[8] & 0xC0U, 0x80U);
}

TEST(Uuid, V7IsTimeOrdered) {
    // The whole point of v7 for internal _id fields: ids sort by creation time,
    // so B-tree inserts append instead of splitting random pages.
    const Uuid first = generate_v7();
    std::this_thread::sleep_for(std::chrono::milliseconds{3});
    const Uuid second = generate_v7();
    EXPECT_LT(first, second);
}

TEST(Uuid, V7EncodesItsTimestamp) {
    const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const Uuid id = generate_v7();
    const std::int64_t encoded = v7_timestamp_ms(id);

    EXPECT_GE(encoded, before - 1000);
    EXPECT_LE(encoded, before + 1000);
}

TEST(Uuid, V4DoesNotCollide) {
    // Not a randomness test — a smoke test that the CSPRNG is actually wired up.
    // A generator returning a constant is the failure this catches.
    std::set<Uuid> seen;
    for (int i = 0; i < 10000; ++i) { seen.insert(generate_v4()); }
    EXPECT_EQ(seen.size(), 10000U);
}

TEST(Uuid, StringRoundTrip) {
    const Uuid id = generate_v4();
    const std::string text = to_string(id);

    ASSERT_EQ(text.size(), 36U);
    EXPECT_EQ(text[8], '-');
    EXPECT_EQ(text[13], '-');
    EXPECT_EQ(text[18], '-');
    EXPECT_EQ(text[23], '-');

    const auto parsed = parse(text);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, id);
}

TEST(Uuid, Base64UrlRoundTrip) {
    const Uuid id = generate_v7();
    const std::array<char, 22> encoded = to_base64url(id);

    const auto parsed = from_base64url(std::string_view{encoded.data(), encoded.size()});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, id);
}

TEST(Uuid, RejectsMalformedInput) {
    // Cursors and token subjects arrive as client input; parse must never throw
    // and never accept a near-miss.
    EXPECT_FALSE(parse("").has_value());
    EXPECT_FALSE(parse("not-a-uuid").has_value());
    EXPECT_FALSE(parse("0123456789abcdef0123456789abcdef").has_value());       // no hyphens
    EXPECT_FALSE(parse("zzzzzzzz-0000-0000-0000-000000000000").has_value());   // non-hex
    EXPECT_FALSE(parse("00000000_0000-0000-0000-000000000000").has_value());   // wrong sep
    EXPECT_FALSE(from_base64url("short").has_value());
    EXPECT_FALSE(from_base64url("!!!!!!!!!!!!!!!!!!!!!!").has_value());
}

TEST(Uuid, NilParsesButIsRecognisable) {
    const auto parsed = parse("00000000-0000-0000-0000-000000000000");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(is_nil(*parsed));
}

}  // namespace anvil::uuid
