// The seams, exercised from OUTSIDE anvil.
//
// This file plays the part of an application: it includes only public headers and
// the reference application's own tables. A seam that cannot be satisfied from
// out here fails at this boundary, which is the only place it can fail cheaply —
// the alternative is finding out in the first project that tries to use anvil for
// something other than what it was extracted from.
//
// Most of what matters below is a static_assert and has therefore already passed
// by the time this binary runs. The runtime cases cover what a constant
// expression cannot reach: visitor ordering, and the behaviour of the lookups on
// inputs an application does not control.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "anvil/core/locale.h"
#include "anvil/core/perm_catalogue.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/core/user_context.h"

#include "perms.h"

namespace {

using testapp::Perm;
using testapp::kPerms;

// --- Seam 1: permissions ---------------------------------------------------

TEST(PermCatalogue, NamesEveryDeclaredBit) {
    EXPECT_EQ(kPerms.name_for_bit(static_cast<std::size_t>(Perm::ContentWrite)), "ContentWrite");
    EXPECT_EQ(kPerms.name_for_bit(static_cast<std::size_t>(Perm::SystemAnnounce)),
              "SystemAnnounce");
}

TEST(PermCatalogue, ReservedGapsHaveNoName) {
    // 5 sits in the gap between the content block and the media block. It is not
    // a permission the server understands, and a plausible-sounding name for it
    // would put a control on a staff screen that authorises nothing.
    EXPECT_TRUE(kPerms.name_for_bit(5).empty());
    EXPECT_TRUE(kPerms.name_for_bit(127).empty());
    // Out of range entirely: not a crash, not a guess.
    EXPECT_TRUE(kPerms.name_for_bit(999).empty());
}

TEST(PermCatalogue, UnknownNameResolvesToNothing) {
    EXPECT_FALSE(kPerms.bit_for_name("NoSuchPermission").has_value());
    EXPECT_FALSE(kPerms.bit_for_name("").has_value());
    EXPECT_FALSE(kPerms.bit_for_name("contentwrite").has_value()) << "lookup is case-sensitive";

    const auto bit = kPerms.bit_for_name("FormPii");
    ASSERT_TRUE(bit.has_value());
    EXPECT_EQ(*bit, static_cast<std::uint8_t>(Perm::FormPii));
}

TEST(PermCatalogue, NameAndBitLookupsAreInverses) {
    for (const anvil::PermName& entry : kPerms.names()) {
        const auto round_tripped = kPerms.bit_for_name(entry.name);
        ASSERT_TRUE(round_tripped.has_value()) << entry.name;
        EXPECT_EQ(*round_tripped, entry.bit) << entry.name;
        EXPECT_EQ(kPerms.name_for_bit(entry.bit), entry.name);
    }
}

TEST(PermCatalogue, VisitsInBitOrderNotTableOrder) {
    // Built high bit first, so a visitor that walked the table would report them
    // in the order they were set rather than in bit order.
    anvil::PermSet held{};
    held.set(static_cast<std::size_t>(Perm::SystemAnnounce));
    held.set(static_cast<std::size_t>(Perm::ContentWrite));
    held.set(static_cast<std::size_t>(Perm::MediaUpload));

    std::vector<std::string> seen;
    kPerms.for_each_name(held, [&seen](std::string_view name) { seen.emplace_back(name); });

    // Bit order: 1, 8, 120. Two responses for one holder must be byte-identical
    // so a client can diff them, and that only holds if the order is a function
    // of the bits rather than of how the application wrote its array.
    const std::vector<std::string> expected{"ContentWrite", "MediaUpload", "SystemAnnounce"};
    EXPECT_EQ(seen, expected);
}

TEST(PermCatalogue, VisitorSkipsUndeclaredBits) {
    anvil::PermSet held{};
    held.set(static_cast<std::size_t>(Perm::ContentRead));
    held.set(5);   // reserved gap
    held.set(99);  // reserved gap

    std::vector<std::string> seen;
    kPerms.for_each_name(held, [&seen](std::string_view name) { seen.emplace_back(name); });

    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0], "ContentRead");
}

TEST(PermCatalogue, AllExcludesReservedGaps) {
    const anvil::PermSet everything = kPerms.all();

    EXPECT_EQ(everything.count(), kPerms.size());
    EXPECT_TRUE(everything.test(static_cast<std::size_t>(Perm::SystemAnnounce)));

    // The difference from ~PermSet{}, which is the whole reason all() is built
    // from the names: that would set all 128 bits, handing out authority the
    // server has no meaning for until somebody adds a permission at one of them.
    EXPECT_FALSE(everything.test(5));
    EXPECT_NE(everything, ~anvil::PermSet{});
}

TEST(PermCatalogue, RejectsMalformedTables) {
    // well_formed() is static_asserted on the real table, so these are the cases
    // that assertion is protecting against, proved to actually be rejected.
    constexpr std::array<anvil::PermName, 2> duplicate_bit{{{"A", 3}, {"B", 3}}};
    constexpr std::array<anvil::PermName, 2> duplicate_name{{{"A", 3}, {"A", 4}}};
    constexpr std::array<anvil::PermName, 1> empty_name{{{"", 3}}};
    constexpr std::array<anvil::PermName, 1> out_of_range{{{"A", 200}}};
    constexpr std::array<anvil::PermName, 2> good{{{"A", 3}, {"B", 4}}};

    EXPECT_FALSE(anvil::PermCatalogue{duplicate_bit}.well_formed());
    EXPECT_FALSE(anvil::PermCatalogue{duplicate_name}.well_formed());
    EXPECT_FALSE(anvil::PermCatalogue{empty_name}.well_formed());
    EXPECT_FALSE(anvil::PermCatalogue{out_of_range}.well_formed());
    EXPECT_TRUE(anvil::PermCatalogue{good}.well_formed());
}

TEST(PermSetWireFormat, RoundTripsAcrossBothWords) {
    anvil::PermSet original{};
    original.set(0);
    original.set(63);   // last bit of the low word
    original.set(64);   // first bit of the high word
    original.set(127);  // last bit of the high word

    const auto bytes = original.to_bytes();
    EXPECT_EQ(anvil::PermSet::from_bytes(bytes), original);

    // Little-endian, low word first, and fixed explicitly because these bits are
    // persisted: bit 0 is the low bit of byte 0, bit 127 the high bit of byte 15.
    EXPECT_EQ(bytes[0] & 0x01U, 0x01U);
    EXPECT_EQ(bytes[7] & 0x80U, 0x80U);
    EXPECT_EQ(bytes[8] & 0x01U, 0x01U);
    EXPECT_EQ(bytes[15] & 0x80U, 0x80U);
}

// --- Seam 2: locales -------------------------------------------------------

TEST(LocaleSeam, ResolvesEveryDeclaredTag) {
    for (std::size_t i = 0; i < anvil::kLocaleCount; ++i) {
        const auto by_index = anvil::Locale::from_index(i);
        ASSERT_TRUE(by_index.has_value());

        const auto by_tag = anvil::Locale::from_tag(by_index->tag());
        ASSERT_TRUE(by_tag.has_value()) << by_index->tag();
        EXPECT_EQ(*by_tag, *by_index);
        EXPECT_EQ(by_tag->index(), i);
    }
}

TEST(LocaleSeam, RejectsWhatItCannotResolve) {
    EXPECT_FALSE(anvil::Locale::from_index(anvil::kLocaleCount).has_value());
    EXPECT_FALSE(anvil::Locale::from_index(999).has_value());

    // Unknown is nothing, not the default. Silently answering an unrecognised
    // ?lang= in the default locale is how a missing translation goes unnoticed.
    EXPECT_FALSE(anvil::Locale::from_tag("zz").has_value());
    EXPECT_FALSE(anvil::Locale::from_tag("").has_value());
    EXPECT_FALSE(anvil::Locale::from_tag("EN").has_value()) << "tags are case-sensitive";
}

TEST(LocaleSeam, DefaultConstructsToTheDeclaredDefault) {
    EXPECT_EQ(anvil::Locale{}.index(), anvil::config::kDefaultLocale);
}

TEST(LocaleSeam, CarriesCollationAndDirection) {
    const auto arabic = anvil::Locale::from_tag("ar");
    ASSERT_TRUE(arabic.has_value());
    EXPECT_TRUE(arabic->rtl());
    // The one string the sort and the index that must serve it both take. A sort
    // whose collation the index was not built with is a COLLSCAN, and the symptom
    // is slowness rather than an error.
    EXPECT_EQ(arabic->collation(), "ar");

    const auto english = anvil::Locale::from_tag("en");
    ASSERT_TRUE(english.has_value());
    EXPECT_FALSE(english->rtl());
}

TEST(LocaleSeam, AllLocalesCoversTheTableExactlyOnce) {
    ASSERT_EQ(anvil::kAllLocales.size(), anvil::kLocaleCount);
    for (std::size_t i = 0; i < anvil::kAllLocales.size(); ++i) {
        EXPECT_EQ(anvil::kAllLocales[i].index(), i);
    }
}

TEST(LocalizedText, GetIndexesByLocale) {
    const anvil::LocalizedView text{{"Hello", "مرحبا"}};

    EXPECT_EQ(text.get(*anvil::Locale::from_tag("en")), "Hello");
    EXPECT_EQ(text.get(*anvil::Locale::from_tag("ar")), "مرحبا");
}

TEST(LocalizedText, PresentButEmptyIsIncomplete) {
    EXPECT_TRUE((anvil::LocalizedView{{"Hello", "مرحبا"}}.complete()));

    // The same defect as missing: it renders as a blank heading in that locale and
    // nothing reports it. A fallback to another locale would show a reader the
    // wrong language without telling them.
    EXPECT_FALSE((anvil::LocalizedView{{"Hello", ""}}.complete()));
    EXPECT_FALSE((anvil::LocalizedView{{"", "مرحبا"}}.complete()));
    EXPECT_FALSE((anvil::LocalizedView{{"", ""}}.complete()));
}

// --- The struct the two seams meet in --------------------------------------

TEST(UserContextLayout, HoldsBothSeamsWithoutGrowing) {
    // Both static_asserted in the header; restated here because the number is the
    // requirement, not an observation about today's compiler.
    EXPECT_EQ(sizeof(anvil::UserContext), 64U);

    anvil::UserContext ctx{};
    ctx.permissions = kPerms.all();
    ctx.locale = *anvil::Locale::from_tag("ar");
    ctx.user_type = anvil::UserType::Staff;

    // Copying it is what a thread-pool hop does.
    const anvil::UserContext copied = ctx;
    EXPECT_EQ(copied.permissions, kPerms.all());
    EXPECT_EQ(copied.locale.tag(), "ar");
}

// --- Result, which every seam returns through ------------------------------

TEST(ResultType, CarriesAFieldNameButNeverAValue) {
    const anvil::Result<int> failed = anvil::fail(anvil::ErrorCode::ValidationFailed, "email");
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.code(), anvil::ErrorCode::ValidationFailed);
    EXPECT_EQ(failed.error().field, "email");

    const anvil::Result<int> succeeded = 7;
    ASSERT_TRUE(succeeded.ok());
    EXPECT_EQ(succeeded.value(), 7);
    EXPECT_EQ(succeeded.code(), anvil::ErrorCode::Ok);
    EXPECT_EQ(failed.value_or(-1), -1);
}

TEST(ResultType, VoidSpecialisationDistinguishesOkFromFailure) {
    EXPECT_TRUE(anvil::ok().ok());
    EXPECT_EQ(anvil::ok().code(), anvil::ErrorCode::Ok);

    const anvil::Status failed = anvil::fail(anvil::ErrorCode::Conflict);
    EXPECT_FALSE(failed.ok());
    EXPECT_EQ(failed.code(), anvil::ErrorCode::Conflict);
    EXPECT_TRUE(failed.error().field.empty());
}

}  // namespace
