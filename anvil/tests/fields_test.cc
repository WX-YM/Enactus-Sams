// The field validators.

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

#include "anvil/input/fields.h"

namespace anvil::input {
namespace {

// --- 2, 3: email ----------------------------------------------------------

TEST(Email, AcceptsOrdinaryAddresses) {
    EXPECT_EQ(check_email("ahmed@example.com"), Reason::Ok);
    EXPECT_EQ(check_email("ahmed.youssef+tag@mail.example.co.uk"), Reason::Ok);
    EXPECT_EQ(check_email("a@b.co"), Reason::Ok);
    EXPECT_EQ(check_email("user_name-1@sub-domain.example.org"), Reason::Ok);
    EXPECT_EQ(check_email("xn--mgbh0fb.xn--kgbechtv@xn--mgbaam7a8h.example"), Reason::Ok)
        << "punycode is the encoded form an IDNA domain arrives in";
}

TEST(Email, RejectsQuotedLocalPartsAndComments) {
    // Both are legal RFC 5322 and neither has a legitimate use. Each is a place
    // where this scanner and a receiving MTA can disagree about who the address
    // belongs to (docs/05-auth-sessions.md §7).
    EXPECT_EQ(check_email(R"("john doe"@example.com)"), Reason::NotAllowed);
    EXPECT_EQ(check_email("john(work)@example.com"), Reason::NotAllowed);
    EXPECT_EQ(check_email("john@example.com, jane@example.com"), Reason::NotAllowed);
    EXPECT_EQ(check_email("john doe@example.com"), Reason::NotAllowed);
}

TEST(Email, EnforcesStructureAndLengthBounds) {
    EXPECT_EQ(check_email(""), Reason::Required);
    EXPECT_EQ(check_email("no-at-sign.example.com"), Reason::BadFormat);
    EXPECT_EQ(check_email("two@at@example.com"), Reason::BadFormat);
    EXPECT_EQ(check_email("@example.com"), Reason::TooShort);
    EXPECT_EQ(check_email("user@"), Reason::TooShort);
    EXPECT_EQ(check_email(".leading@example.com"), Reason::BadFormat);
    EXPECT_EQ(check_email("trailing.@example.com"), Reason::BadFormat);
    EXPECT_EQ(check_email("double..dot@example.com"), Reason::BadFormat);
    EXPECT_EQ(check_email("user@example"), Reason::BadFormat) << "a TLD is required";
    EXPECT_EQ(check_email("user@example..com"), Reason::BadFormat);
    EXPECT_EQ(check_email("user@-example.com"), Reason::BadFormat);
    EXPECT_EQ(check_email("user@[192.0.2.1]"), Reason::NotAllowed);
    EXPECT_EQ(check_email("user@192.0.2.1"), Reason::NotAllowed) << "a numeric TLD is an IP";

    EXPECT_EQ(check_email(std::string(65, 'a') + "@example.com"), Reason::TooLong);
    EXPECT_EQ(check_email("a@" + std::string(250, 'b') + ".com"), Reason::TooLong);
}

TEST(Email, RejectsCyrillicHomoglyphDomains) {
    // `pаypal.com` with a Cyrillic а. Domains are IDNA-encoded before storage,
    // so a byte above 0x7F proves the value has not been through that step —
    // which is exactly the property that makes the homoglyph fail.
    EXPECT_EQ(check_email("user@pаypal.com"), Reason::BadCharset);
    EXPECT_EQ(check_email("usеr@example.com"), Reason::BadCharset);
}

// --- 4, 5: password -------------------------------------------------------

TEST(Password, EnforcesLengthInCodePointsNotBytes) {
    EXPECT_EQ(check_password("short"), Reason::TooShort);
    EXPECT_EQ(check_password("elevenchars"), Reason::TooShort);
    EXPECT_EQ(check_password("twelvechars!"), Reason::Ok);

    // 40 Arabic characters is ~80 bytes. A byte-based limit would give Arabic
    // users half the allowance, and a byte-based MINIMUM would accept a 6-
    // character Arabic password.
    std::string arabic;
    for (int i = 0; i < 40; ++i) { arabic += "ك"; }
    EXPECT_EQ(arabic.size(), 80U);
    EXPECT_EQ(check_password(arabic), Reason::Ok);

    std::string at_limit;
    for (int i = 0; i < 128; ++i) { at_limit += "م"; }
    EXPECT_EQ(check_password(at_limit), Reason::Ok) << "128 code points, ~256 bytes";
    EXPECT_EQ(check_password(at_limit + "م"), Reason::TooLong)
        << "the cap is enforced before Argon2 ever sees the value";
}

TEST(Password, HasNoCompositionRulesAndNeverTrims) {
    // NIST 800-63B: length plus a breach check. Composition rules push users to
    // `Password1!` and measurably reduce entropy (docs/05-auth-sessions.md §3).
    EXPECT_EQ(check_password("all lowercase letters and spaces"), Reason::Ok);
    EXPECT_EQ(check_password("            twelve spaces leading"), Reason::Ok)
        << "trimming would change a password the user chose";
    EXPECT_EQ(check_password("قهوة سادة من فضلك"), Reason::Ok);
}

TEST(Password, BreachedPasswordsAreRejectedWithNoNetworkCall) {
    // The filter is .rodata built at compile time: no socket, no dependency on
    // anyone else's uptime, and no disclosure of the credential.
    EXPECT_EQ(check_password("123456789012"), Reason::Breached);
    EXPECT_EQ(check_password("passwordpassword"), Reason::Breached);
    EXPECT_EQ(check_password("p@ssw0rd1234"), Reason::Breached);
    EXPECT_TRUE(is_breached_password("qwertyuiop123"));

    // A Bloom filter has no false negatives and vanishingly few false
    // positives; an ordinary passphrase must not collide.
    EXPECT_FALSE(is_breached_password("correct horse battery staple"));
    EXPECT_EQ(check_password("correct horse battery staple"), Reason::Ok);
}

// --- 6, 7: dates and timestamps -------------------------------------------

TEST(Date, ValidatesTheCalendarNotThePattern) {
    CalendarDate date{};
    EXPECT_EQ(parse_date("2024-02-29", date), Reason::Ok) << "2024 is a leap year";
    EXPECT_EQ(date.year, 2024);
    EXPECT_EQ(date.month, 2);
    EXPECT_EQ(date.day, 29);

    EXPECT_EQ(parse_date("2025-02-29", date), Reason::OutOfRange) << "2025 is not";
    EXPECT_EQ(parse_date("2026-02-30", date), Reason::OutOfRange);
    EXPECT_EQ(parse_date("2026-04-31", date), Reason::OutOfRange);
    EXPECT_EQ(parse_date("2026-13-01", date), Reason::OutOfRange);
    EXPECT_EQ(parse_date("2026-00-10", date), Reason::OutOfRange);
    EXPECT_EQ(parse_date("2026-01-00", date), Reason::OutOfRange);

    // The century rules that catch a naive leap-year check.
    EXPECT_EQ(parse_date("2000-02-29", date), Reason::Ok);
    EXPECT_EQ(parse_date("2100-02-29", date), Reason::OutOfRange);

    EXPECT_EQ(parse_date("2026-8-4", date), Reason::BadFormat) << "fixed width only";
    EXPECT_EQ(parse_date("04/08/2026", date), Reason::BadFormat);
    EXPECT_EQ(parse_date("1969-12-31", date), Reason::OutOfRange);
}

TEST(Timestamp, RequiresAnExplicitOffset) {
    std::int64_t ms = 0;
    // "18:00" means nothing until someone decides whose 18:00 it was, and that
    // decision silently becomes the server's.
    EXPECT_EQ(parse_timestamp("2026-08-04T18:00:00", ms), Reason::BadFormat);
    EXPECT_EQ(parse_timestamp("2026-08-04 18:00:00", ms), Reason::BadFormat);
    EXPECT_EQ(parse_timestamp("2026-08-04", ms), Reason::BadFormat);

    EXPECT_EQ(parse_timestamp("2026-08-04T18:00:00Z", ms), Reason::Ok);
    EXPECT_EQ(parse_timestamp("2026-08-04T18:00:00+02:00", ms), Reason::Ok);
}

TEST(Timestamp, ConvertsToUtcMilliseconds) {
    std::int64_t utc = 0;
    std::int64_t cairo = 0;
    ASSERT_EQ(parse_timestamp("2026-08-04T18:00:00Z", utc), Reason::Ok);
    ASSERT_EQ(parse_timestamp("2026-08-04T20:00:00+02:00", cairo), Reason::Ok);
    EXPECT_EQ(utc, cairo) << "the offset is applied, then discarded — storage is UTC";

    std::int64_t epoch = 0;
    ASSERT_EQ(parse_timestamp("1970-01-01T00:00:00Z", epoch), Reason::Ok);
    EXPECT_EQ(epoch, 0);

    std::int64_t with_millis = 0;
    ASSERT_EQ(parse_timestamp("2026-08-04T18:00:00.250Z", with_millis), Reason::Ok);
    EXPECT_EQ(with_millis - utc, 250);

    std::int64_t truncated = 0;
    ASSERT_EQ(parse_timestamp("2026-08-04T18:00:00.250999Z", truncated), Reason::Ok);
    EXPECT_EQ(truncated, with_millis) << "BSON stores milliseconds; finer is truncated here";
}

TEST(Timestamp, RejectsShapesThatTwoParsersWouldReadDifferently) {
    std::int64_t ms = 0;
    EXPECT_EQ(parse_timestamp("2026-08-04T24:00:00Z", ms), Reason::OutOfRange);
    EXPECT_EQ(parse_timestamp("2026-06-30T23:59:60Z", ms), Reason::OutOfRange) << "leap second";
    EXPECT_EQ(parse_timestamp("2026-08-04T18:00:00+0200", ms), Reason::BadFormat);
    EXPECT_EQ(parse_timestamp("2026-08-04T18:00:00+02", ms), Reason::BadFormat);
    EXPECT_EQ(parse_timestamp("2026-08-04T18:00:00Z ", ms), Reason::BadFormat);
    EXPECT_EQ(parse_timestamp("2026-08-04T18:00:00.Z", ms), Reason::BadFormat);
}

// --- 8: numbers -----------------------------------------------------------

TEST(Number, ParsesWithFromCharsAndReportsOverflow) {
    std::int64_t value = 0;
    EXPECT_EQ(parse_int("42", 0, 100, value), Reason::Ok);
    EXPECT_EQ(value, 42);

    EXPECT_EQ(parse_int("-1", 0, 100, value), Reason::OutOfRange);
    EXPECT_EQ(parse_int("101", 0, 100, value), Reason::OutOfRange);
    EXPECT_EQ(parse_int("99999999999999999999", 0, 100, value), Reason::OutOfRange)
        << "twenty digits is within the length bound and outside int64";
    EXPECT_EQ(parse_int("999999999999999999999", 0, 100, value), Reason::BadFormat)
        << "past the widest int64, the length bound rejects before parsing";
    EXPECT_EQ(parse_int("9223372036854775808", 0, 100, value), Reason::OutOfRange)
        << "int64 max plus one is reported, never wrapped";
    EXPECT_EQ(parse_int("12abc", 0, 100, value), Reason::BadFormat)
        << "atoi would silently return 12";
    EXPECT_EQ(parse_int("", 0, 100, value), Reason::BadFormat);
    EXPECT_EQ(parse_int(" 42", 0, 100, value), Reason::BadFormat);
    EXPECT_EQ(parse_int("1.5", 0, 100, value), Reason::BadFormat);
}

TEST(Number, AcceptsArabicIndicDigits) {
    // An Egyptian user typing ١٢٣ on an Arabic keyboard is entering 123, and
    // rejecting it is a defect dressed as a control.
    std::int64_t arabic = 0;
    std::int64_t eastern = 0;
    ASSERT_EQ(parse_int("١٢٣", 0, 1000, arabic), Reason::Ok);
    ASSERT_EQ(parse_int("۱۲۳", 0, 1000, eastern), Reason::Ok);
    EXPECT_EQ(arabic, 123);
    EXPECT_EQ(eastern, 123);
}

// --- 9, 10: URLs ----------------------------------------------------------

TEST(Url, RejectsEveryUnlistedSchemeRatherThanNamingDangerousOnes) {
    // A deny-list of dangerous schemes is always incomplete: javascript, data
    // and vbscript are the three everyone remembers.
    EXPECT_EQ(check_url("javascript:alert(1)", UrlUse::Link), Reason::NotAllowed);
    EXPECT_EQ(check_url("JaVaScRiPt:alert(1)", UrlUse::Link), Reason::NotAllowed);
    EXPECT_EQ(check_url("data:text/html;base64,PHNjcmlwdD4=", UrlUse::Link), Reason::NotAllowed);
    EXPECT_EQ(check_url("vbscript:msgbox", UrlUse::Link), Reason::NotAllowed);
    EXPECT_EQ(check_url("file:///etc/passwd", UrlUse::Link), Reason::NotAllowed);
    EXPECT_EQ(check_url("http://example.com", UrlUse::Link), Reason::NotAllowed);
    EXPECT_EQ(check_url("blob:https://example.com/uuid", UrlUse::Link), Reason::NotAllowed);

    EXPECT_EQ(check_url("https://example.com/page", UrlUse::Link), Reason::Ok);
    EXPECT_EQ(check_url("/about", UrlUse::Link), Reason::Ok);
    EXPECT_EQ(check_url("//evil.test/path", UrlUse::Link), Reason::NotAllowed)
        << "protocol-relative is not site-relative";
    EXPECT_EQ(check_url("mailto:info@example.com", UrlUse::Link), Reason::Ok);
    // A fragment reference: the only shape that names a page of a hash-routed
    // SPA, and the narrowest one there is — no scheme, no host, no way off the
    // origin. Never fetchable, though: there is no resource at a fragment.
    EXPECT_EQ(check_url("#/cafe", UrlUse::Link), Reason::Ok);
    EXPECT_EQ(check_url("#", UrlUse::Link), Reason::Ok);
    EXPECT_EQ(check_url("#/cafe", UrlUse::ServerFetch), Reason::NotAllowed);
    // Still a fragment, still inert: the scheme-looking text is part of the
    // fragment, not a scheme.
    EXPECT_EQ(check_url("#javascript:alert(1)", UrlUse::Link), Reason::Ok);
}

TEST(Url, RejectsCredentialsIpLiteralsAndNonAsciiHosts) {
    EXPECT_EQ(check_url("https://www.bank.com@evil.test/", UrlUse::Link), Reason::NotAllowed);
    EXPECT_EQ(check_url("https://192.0.2.1/x", UrlUse::Link), Reason::NotAllowed);
    EXPECT_EQ(check_url("https://2130706433/x", UrlUse::Link), Reason::NotAllowed)
        << "the decimal spelling of 127.0.0.1";
    EXPECT_EQ(check_url("https://[::1]/x", UrlUse::Link), Reason::NotAllowed);
    EXPECT_EQ(check_url("https://pаypal.com/", UrlUse::Link), Reason::BadCharset);
    EXPECT_EQ(check_url("https://example.com/\r\nX-Injected: 1", UrlUse::Link),
              Reason::BadCharset)
        << "a control character in a URL is header splitting waiting for a header";
}

TEST(Url, ServerFetchTargetsRejectInternalNames) {
    // The metadata endpoint is one GET away from cloud credentials. The
    // post-DNS and connect-time checks belong to the HTTP client; this is the
    // cheap first pass.
    EXPECT_EQ(check_url("https://169.254.169.254/latest/meta-data/", UrlUse::ServerFetch),
              Reason::NotAllowed);
    EXPECT_EQ(check_url("https://localhost/hook", UrlUse::ServerFetch), Reason::NotAllowed);
    EXPECT_EQ(check_url("https://metadata.google.internal/x", UrlUse::ServerFetch),
              Reason::NotAllowed);
    EXPECT_EQ(check_url("https://db.internal/hook", UrlUse::ServerFetch), Reason::NotAllowed);
    EXPECT_EQ(check_url("/relative", UrlUse::ServerFetch), Reason::NotAllowed);

    EXPECT_EQ(check_url("https://hooks.example.com/endpoint", UrlUse::ServerFetch), Reason::Ok);
}

// --- text -----------------------------------------------------------------

TEST(Text, BoundsAreCodePointsAndBidiPolicyDependsOnTheFieldClass) {
    constexpr TextRules kTitle{1, 120, i18n::TextClass::Prose, false};

    std::string arabic_120;
    for (int i = 0; i < 120; ++i) { arabic_120 += "ن"; }
    EXPECT_EQ(check_text(arabic_120, kTitle), Reason::Ok) << "120 code points, 240 bytes";
    EXPECT_EQ(check_text(arabic_120 + "ن", kTitle), Reason::TooLong);
    EXPECT_EQ(check_text("", kTitle), Reason::Required);

    // U+202E renders `exe.<override>gnp.evil` as `evil.png`.
    EXPECT_EQ(check_text("exe.\u202Egnp.evil", kIdentifierRules), Reason::BadCharset);
    EXPECT_EQ(check_text("exe.\u202Egnp.evil", kTitle), Reason::BadCharset);
    // Isolates are needed to render mixed Arabic and English correctly, so they
    // are prose-legal and identifier-illegal.
    EXPECT_EQ(check_text("\u2066English\u2069 داخل عربي", kTitle), Reason::Ok);
    EXPECT_EQ(check_text("\u2066name\u2069", kIdentifierRules), Reason::BadCharset);
    // Zero-width characters make two different identifiers look identical.
    EXPECT_EQ(check_text("ahmed\u200By", kIdentifierRules), Reason::BadCharset);

    EXPECT_EQ(check_text("line\nbreak", kTitle), Reason::BadCharset);
    EXPECT_EQ(check_text("line\nbreak", kLongProseRules), Reason::Ok);
}

// --- uuid and enum --------------------------------------------------------

TEST(Uuid, AcceptsCanonicalAndBase64UrlFormsOnly) {
    Uuid id{};
    EXPECT_EQ(parse_uuid("018f3a2b-0011-2233-4455-66778899aabb", id), Reason::Ok);
    EXPECT_EQ(parse_uuid("018f3a2b00112233445566778899aabb", id), Reason::BadFormat)
        << "unhyphenated hex is a third format nobody agreed on";
    EXPECT_EQ(parse_uuid("not-a-uuid", id), Reason::BadFormat);
    EXPECT_EQ(parse_uuid("", id), Reason::BadFormat);
    EXPECT_EQ(parse_uuid("018f3a2b-0011-2233-4455-66778899aabb-extra", id), Reason::BadFormat);
}

TEST(Enum, BindsThroughAConstexprTableAndRejectsAnythingElse) {
    enum class Domain : std::uint8_t { Events = 1, Menu = 2 };
    static constexpr std::array<EnumEntry<Domain>, 2> kTable{
        EnumEntry<Domain>{"events", Domain::Events},
        EnumEntry<Domain>{"menu", Domain::Menu},
    };

    Domain domain{};
    EXPECT_EQ(parse_enum("menu", kTable, domain), Reason::Ok);
    EXPECT_EQ(domain, Domain::Menu);
    EXPECT_EQ(parse_enum("Menu", kTable, domain), Reason::NotAllowed) << "exact match only";
    EXPECT_EQ(parse_enum("admin", kTable, domain), Reason::NotAllowed);
    EXPECT_EQ(parse_enum("", kTable, domain), Reason::NotAllowed);
}

}  // namespace
}  // namespace anvil::input
