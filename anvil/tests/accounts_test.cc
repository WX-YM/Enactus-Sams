// The account layer's pure parts: one canonical form per identifier, and the
// schema checks an application's table has to pass at compile time.

#include <gtest/gtest.h>

#include <array>
#include <string>

#include "anvil/accounts/identifier.h"
#include "anvil/accounts/schema.h"

namespace anvil::accounts {
namespace {

using input::Reason;

[[nodiscard]] std::string canonical(LoginIdentity kind, std::string_view raw) {
    std::string out;
    EXPECT_EQ(canonicalise(kind, raw, out), Reason::Ok) << raw;
    return out;
}

[[nodiscard]] Reason refused(LoginIdentity kind, std::string_view raw) {
    std::string out = "untouched";
    const Reason reason = canonicalise(kind, raw, out);
    EXPECT_EQ(out, "untouched") << "`out` is written only on success";
    return reason;
}

TEST(AccountIdentifiers, TheShapeDecidesTheKind) {
    EXPECT_EQ(kind_by_shape("user@example.com"), LoginIdentity::Email);
    EXPECT_EQ(kind_by_shape("+201001234567"), LoginIdentity::Phone);
    EXPECT_EQ(kind_by_shape("someone"), LoginIdentity::Username);
    EXPECT_EQ(kind_by_shape(" +20100"), LoginIdentity::Username)
        << "nothing is trimmed, so this is not a phone number anybody meant";
}

TEST(AccountIdentifiers, AnEmailIsFoldedToOneKey) {
    EXPECT_EQ(canonical(LoginIdentity::Email, "User@Example.COM"), "user@example.com");
    EXPECT_NE(refused(LoginIdentity::Email, "not an email"), Reason::Ok);
    EXPECT_EQ(refused(LoginIdentity::Email, ""), Reason::Required);
    // A Cyrillic "а" in the domain: refused, never folded to a Latin one.
    EXPECT_NE(refused(LoginIdentity::Email, "user@pаypal.com"), Reason::Ok);
}

TEST(AccountIdentifiers, AUsernameIsFoldedAndBounded) {
    EXPECT_EQ(canonical(LoginIdentity::Username, "Layla_99"), "layla_99");
    // Compatibility-folded: a full-width "Ａ" is the "a" a person meant.
    EXPECT_EQ(canonical(LoginIdentity::Username, "Ａbc"), "abc");
    EXPECT_EQ(canonical(LoginIdentity::Username, "ليلى"), "ليلى");
    EXPECT_EQ(refused(LoginIdentity::Username, "ab"), Reason::TooShort);
    EXPECT_EQ(refused(LoginIdentity::Username, std::string(33, 'a')), Reason::TooLong);
    EXPECT_EQ(refused(LoginIdentity::Username, "a@b"), Reason::BadCharset)
        << "an @ would make it an email by shape";
    EXPECT_NE(refused(LoginIdentity::Username, "ab‮cd"), Reason::Ok)
        << "a bidi override has no place in something compared and listed";
}

TEST(AccountIdentifiers, APhoneIsE164WithWhatPeopleTypeRemoved) {
    EXPECT_EQ(canonical(LoginIdentity::Phone, "+20 (100) 123-4567"), "+201001234567");
    EXPECT_EQ(canonical(LoginIdentity::Phone, "+1.415.555.0100"), "+14155550100");
    EXPECT_EQ(refused(LoginIdentity::Phone, "01001234567"), Reason::BadFormat)
        << "a national form, which would be stored as a different subscriber";
    EXPECT_EQ(refused(LoginIdentity::Phone, "+0201001234567"), Reason::BadFormat);
    EXPECT_EQ(refused(LoginIdentity::Phone, "+2010"), Reason::TooShort);
    EXPECT_EQ(refused(LoginIdentity::Phone, "+2010012345678901"), Reason::TooLong);
    EXPECT_EQ(refused(LoginIdentity::Phone, "+20 100 abc"), Reason::BadFormat);
}

TEST(AccountIdentifiers, AnOversizedInputIsRefusedBeforeItIsNormalised) {
    EXPECT_EQ(refused(LoginIdentity::Username, std::string(400, 'a')), Reason::TooLong);
}

// --- the schema ----------------------------------------------------------------------

inline constexpr std::array<IdentifierSpec, 2> kEmailAndUsername{{
    {LoginIdentity::Email, true, true},
    {LoginIdentity::Username, false, true},
}};
inline constexpr std::array<ProfileFieldSpec, 1> kGivenName{{
    {"given_name", input::kPersonNameRules, true},
}};

TEST(AccountSchemaChecks, AReasonableSchemaPasses) {
    constexpr AccountSchema schema{kEmailAndUsername, kGivenName};
    static_assert(account_schema_is_well_formed(schema));
    EXPECT_EQ(schema.contact, LoginIdentity::Email);
    EXPECT_EQ(schema.activation, Activation::AfterVerification);
}

TEST(AccountSchemaChecks, ASchemaThatCannotWorkIsRefused) {
    constexpr std::array<IdentifierSpec, 1> kNoSignIn{{{LoginIdentity::Email, true, false}}};
    static_assert(!account_schema_is_well_formed(AccountSchema{kNoSignIn, {}}));

    constexpr std::array<IdentifierSpec, 1> kOptionalContact{{{LoginIdentity::Email, false, true}}};
    static_assert(!account_schema_is_well_formed(AccountSchema{kOptionalContact, {}}));

    constexpr std::array<IdentifierSpec, 1> kUsernameOnly{{{LoginIdentity::Username, true, true}}};
    // A code cannot be sent to a username.
    static_assert(!account_schema_is_well_formed(
        AccountSchema{kUsernameOnly, {}, LoginIdentity::Username}));

    constexpr std::array<IdentifierSpec, 2> kTwice{{
        {LoginIdentity::Email, true, true}, {LoginIdentity::Email, false, true}}};
    static_assert(!account_schema_is_well_formed(AccountSchema{kTwice, {}}));

    constexpr std::array<ProfileFieldSpec, 1> kReserved{{{"password", input::kPersonNameRules, true}}};
    static_assert(!account_schema_is_well_formed(AccountSchema{kEmailAndUsername, kReserved}));

    constexpr std::array<ProfileFieldSpec, 1> kUpper{{{"Given", input::kPersonNameRules, true}}};
    static_assert(!account_schema_is_well_formed(AccountSchema{kEmailAndUsername, kUpper}));

    constexpr std::array<ProfileFieldSpec, 2> kDuplicate{{
        {"given_name", input::kPersonNameRules, true},
        {"given_name", input::kPersonNameRules, false}}};
    static_assert(!account_schema_is_well_formed(AccountSchema{kEmailAndUsername, kDuplicate}));

    constexpr std::array<ProfileFieldSpec, 1> kEmptyBounds{
        {{"given_name", input::TextRules{5, 2, i18n::TextClass::Prose, false}, true}}};
    static_assert(!account_schema_is_well_formed(AccountSchema{kEmailAndUsername, kEmptyBounds}));
    SUCCEED();
}

// At namespace scope with internal linkage, not a `static constexpr` in the
// body: GCC 16 under -fsanitize=null does not fold the description's null check
// on the address of a function-local static or of an external-linkage inline
// variable, and every assertion below stopped compiling (schema.h says more).
constexpr AccountSchema kSchema{kEmailAndUsername, kGivenName};

TEST(AccountSchemaChecks, TheDescriptionNeedsASaltRouteExactlyWhenTheClientHashes) {
    static constexpr std::array<AccountRoute, 2> kWithSalt{{
        {AccountRole::Salt, "auth.salt"}, {AccountRole::SignIn, "auth.login"}}};
    static constexpr std::array<AccountRoute, 1> kWithoutSalt{{
        {AccountRole::SignIn, "auth.login"}}};
    static constexpr std::array<AccountRoute, 2> kTwiceSignIn{{
        {AccountRole::SignIn, "a"}, {AccountRole::SignIn, "b"}}};

    static_assert(account_description_is_well_formed({&kSchema, Hashing::Client, kWithSalt}));
    static_assert(!account_description_is_well_formed({&kSchema, Hashing::Client, kWithoutSalt}));
    static_assert(account_description_is_well_formed({&kSchema, Hashing::Server, kWithoutSalt}));
    static_assert(!account_description_is_well_formed({&kSchema, Hashing::Server, kWithSalt}));
    static_assert(!account_description_is_well_formed({&kSchema, Hashing::Server, kTwiceSignIn}));
    static_assert(!account_description_is_well_formed({nullptr, Hashing::Server, kWithoutSalt}));
    // Client is the default.
    static_assert(AccountDescription{&kSchema, {}, kWithSalt}.hashing == Hashing::Client);
    SUCCEED();
}

}  // namespace
}  // namespace anvil::accounts
