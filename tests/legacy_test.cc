// The legacy import's pure mappings, and the one property the user migration
// rests on: a legacy argon2id record, wrapped offline, accepts exactly the
// credential the browser computes from the same password.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "anvil/auth/password.h"
#include "anvil/auth/prehash.h"
#include "anvil/crypto/argon2.h"
#include "anvil/i18n/normalize.h"
#include "app/legacy.h"
#include "perms.h"
#include "sections.h"

namespace {

using namespace enactus;
namespace auth = anvil::auth;

constexpr anvil::crypto::Argon2Params kCheap{.memory_kib = 8192, .iterations = 2, .parallelism = 1};

[[nodiscard]] anvil::crypto::Key256 key_of(std::uint8_t first) {
    anvil::crypto::Key256 key;
    for (std::size_t i = 0; i < key.size(); ++i) { key.mutable_span()[i] = static_cast<std::uint8_t>(first + i); }
    return key;
}

[[nodiscard]] auth::PrehashPolicy policy(anvil::crypto::Argon2Params client = auth::kDefaultArgon2Params) {
    return auth::PrehashPolicy{
        .client = client,
        .server = auth::PrehashKeyedDigestStage{.key_id = "p1", .key = key_of(0x00)},
        .retired_peppers = {},
        .salt_key = key_of(0x20),
    };
}

// What hammer's browser prehash computes: Argon2id over NFC(password) with the
// salt and parameters the salt route served.
[[nodiscard]] auth::PrehashKey browser_key(std::string_view password, const auth::PrehashSaltAnswer& served) {
    const std::string nfc = anvil::i18n::normalize(password, anvil::i18n::NormalizeMode::Nfc).value_or("");
    auth::PrehashKey k;
    anvil::crypto::argon2_hash_raw(anvil::crypto::Argon2Type::Argon2id, anvil::crypto::kArgon2Version13,
                                   served.params, {reinterpret_cast<const std::uint8_t*>(nfc.data()), nfc.size()},
                                   served.salt, {}, {}, k.mutable_span());
    return k;
}

TEST(LegacyPasswords, AWrappedRecordAcceptsTheSamePasswordFromTheBrowser) {
    // The old backend hashed with anvil's PasswordHasher (src/handlers/api.cc).
    const auth::PasswordHasher old{kCheap};
    const std::string legacy_record = old.hash("correct horse battery");

    const auth::PrehashHasher hasher{policy()};
    const auto wrapped = hasher.wrap_legacy(legacy_record);
    ASSERT_TRUE(wrapped.ok());

    // The salt route serves the record's own salt and parameters.
    const auth::PrehashSaltAnswer served = hasher.answer_for(wrapped.value(), 0, "member@enactussams.org");
    EXPECT_EQ(served.params, kCheap);

    EXPECT_EQ(hasher.verify(wrapped.value(), browser_key("correct horse battery", served)).outcome,
              auth::VerifyOutcome::Match);
    EXPECT_NE(hasher.verify(wrapped.value(), browser_key("wrong password", served)).outcome,
              auth::VerifyOutcome::Match);
}

TEST(LegacyPasswords, TheLegacyRecordItselfIsNotACredential) {
    const auth::PasswordHasher old{kCheap};
    const std::string legacy_record = old.hash("pw-pw-pw-pw-pw");
    const auth::PrehashHasher hasher{policy()};
    EXPECT_EQ(hasher.verify(legacy_record, auth::PrehashKey{}).outcome, auth::VerifyOutcome::Malformed);
}

TEST(LegacyPermissions, NamesMapToGrantableBitsOnly) {
    const anvil::PermSet set = legacy::permissions_from({"dashboard", " Teams ", "users", "nonsense", "media_upload"});
    EXPECT_TRUE(holds(set, Perm::Dashboard));
    EXPECT_TRUE(holds(set, Perm::Teams));
    EXPECT_TRUE(holds(set, Perm::Users));
    EXPECT_FALSE(holds(set, Perm::MediaUpload)) << "an implied bit is never granted directly";
    EXPECT_FALSE(holds(set, Perm::Content));
    EXPECT_TRUE(legacy::permissions_from({}).none());
}

TEST(LegacyRoles, MapToAccessControlRoles) {
    EXPECT_EQ(legacy::role_from("vice_manager"), "vice manager");
    EXPECT_EQ(legacy::role_from("Manager"), "manager");
    EXPECT_EQ(legacy::role_from("superadmin"), "high board");
    EXPECT_EQ(legacy::role_from("hr"), "HR");
    EXPECT_EQ(legacy::role_from("something else"), "member");
    EXPECT_TRUE(legacy::is_superadmin("superadmin", "x@y.org"));
    EXPECT_TRUE(legacy::is_superadmin("member", "Admin@EnactusSams.org"));
    EXPECT_FALSE(legacy::is_superadmin("manager", "x@y.org"));
}

TEST(LegacyApplications, NamesAndStatuses) {
    EXPECT_EQ(legacy::split_name("Mona  Ahmed Ali").first, "Mona");
    EXPECT_EQ(legacy::split_name("Mona  Ahmed Ali").last, "Ahmed Ali");
    EXPECT_EQ(legacy::split_name("Mona").last, "-");
    EXPECT_EQ(legacy::status_from("Interview Scheduled"), "interview_scheduled");
    EXPECT_EQ(legacy::status_from("accepted"), "accepted");
    EXPECT_EQ(legacy::status_from("weird"), "pending");
}

TEST(LegacyForms, OptionValuesAndTypes) {
    EXPECT_EQ(legacy::option_value("Project Management", 1), "project_management");
    EXPECT_EQ(legacy::option_value("العلاقات", 3), "opt3");
    EXPECT_EQ(legacy::field_type_from("textarea"), "TEXT_LONG");
    EXPECT_EQ(legacy::field_type_from("tel"), "PHONE");
    EXPECT_EQ(legacy::field_type_from("select"), "SELECT_SINGLE");
    EXPECT_EQ(legacy::field_type_from("date"), "TEXT_SHORT");
}

TEST(LegacyContent, EveryMappedKeyLandsOnADeclaredField) {
    for (const std::string_view key : {"aboutHeading", "footerSocialFb", "heroStat2Num", "recruitmentOpen",
                                       "tafrahH4Desc", "closedBannerTitle", "lifeTitle", "insideDesc"}) {
        const auto target = legacy::section_target(key);
        ASSERT_TRUE(target.has_value()) << key;
        const anvil::sections::SectionSpec* spec = anvil::sections::find_section(kSections, target->section);
        ASSERT_NE(spec, nullptr) << key;
        const bool declared = std::any_of(spec->fields.begin(), spec->fields.end(),
                                          [&](const anvil::sections::FieldSpec& f) { return f.key == target->field; });
        EXPECT_TRUE(declared) << key;
    }
    EXPECT_FALSE(legacy::section_target("mediaGallery").has_value());
}

TEST(LegacyImages, OnlyLocalFilesUnderTheirRoots) {
    const auto asset = legacy::local_file_for("/assets/uploads/a-1.webp?v=2", "/srv/www", "/srv/uploads");
    ASSERT_TRUE(asset.has_value());
    EXPECT_EQ(asset->directory, "/srv/www/assets/uploads");
    EXPECT_EQ(asset->file, "a-1.webp");

    const auto upload = legacy::local_file_for("https://enactussams.org/uploads/471A1374.jpg", "/srv/www", "/srv/up");
    ASSERT_TRUE(upload.has_value());
    EXPECT_EQ(upload->directory, "/srv/up");
    EXPECT_EQ(upload->file, "471A1374.jpg");

    // The old site's bundled images were stored document-relative.
    const auto relative = legacy::local_file_for("assets/tafrah-site.jpg", "/srv/www", "/srv/up");
    ASSERT_TRUE(relative.has_value());
    EXPECT_EQ(relative->directory, "/srv/www/assets");
    EXPECT_EQ(relative->file, "tafrah-site.jpg");
    const auto dotted = legacy::local_file_for("./uploads/a.png", "/srv/www", "/srv/up");
    ASSERT_TRUE(dotted.has_value());
    EXPECT_EQ(dotted->directory, "/srv/up");
    EXPECT_EQ(dotted->file, "a.png");
    EXPECT_FALSE(legacy::local_file_for("assets/../etc/passwd", "/srv/www", "/u").has_value());
    EXPECT_FALSE(legacy::local_file_for("etc/passwd", "/srv/www", "/u").has_value());
    EXPECT_FALSE(legacy::local_file_for("assets", "/srv/www", "/u").has_value());

    EXPECT_FALSE(legacy::local_file_for("/assets/../../etc/passwd", "/srv/www", "/u").has_value());
    EXPECT_FALSE(legacy::local_file_for("/assets/.hidden", "/srv/www", "/u").has_value());
    EXPECT_FALSE(legacy::local_file_for("/etc/passwd", "/srv/www", "/u").has_value());
    EXPECT_FALSE(legacy::local_file_for("javascript:alert(1)", "/srv/www", "/u").has_value());
    EXPECT_FALSE(legacy::local_file_for("/assets/a%2F..%2Fb.jpg", "/srv/www", "/u").has_value());
    EXPECT_FALSE(legacy::local_file_for("/assets", "/srv/www", "/u").has_value());
}

}  // namespace
