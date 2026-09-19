// The configuration readers.
//
// anvil ships no Config struct — an application declares its own — so what is
// under test here is the machinery it declares that struct WITH, and the two
// rules that machinery exists to enforce:
//
//   1. a configuration error is a boot failure naming the variable, never a
//      degraded mode discovered three hours later;
//   2. an empty variable is an unset variable.
//
// Every case uses an injected lookup rather than setenv. Calling setenv from a
// test is a data race against every other test in the binary, and the failure is
// intermittent and gets blamed on something else.

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "anvil/config/env.h"
#include "anvil/config/paths.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/secret.h"

namespace anvil::config {
namespace {

// A fixed environment. Values outlive every lookup because the map does.
class Env final {
public:
    explicit Env(std::map<std::string, std::string> values) : values_{std::move(values)} {}

    [[nodiscard]] EnvLookup lookup() const {
        return [this](const char* key) -> const char* {
            const auto it = values_.find(key);
            return it == values_.end() ? nullptr : it->second.c_str();
        };
    }

private:
    std::map<std::string, std::string> values_;
};

// --- presence --------------------------------------------------------------

TEST(ConfigEnv, EmptyIsUnset) {
    const Env env{{{"PRESENT", "value"}, {"EMPTY", ""}}};

    EXPECT_TRUE(lookup(env.lookup(), "PRESENT").has_value());
    // A deployment that exports EMPTY="" has made a mistake. Treating it as
    // present produces a much later and much stranger failure than treating it
    // as missing.
    EXPECT_FALSE(lookup(env.lookup(), "EMPTY").has_value());
    EXPECT_FALSE(lookup(env.lookup(), "ABSENT").has_value());
}

TEST(ConfigEnv, RequiredStringNamesTheVariableItIsMissing) {
    const Env env{{{"EMPTY", ""}}};

    try {
        static_cast<void>(required_string(env.lookup(), "SESSION_PEPPER"));
        FAIL() << "a missing required variable must not return";
    } catch (const ConfigError& e) {
        // The operator has to know WHICH variable. A message that says only
        // "configuration invalid" is a message that costs an hour.
        EXPECT_NE(std::string_view{e.what()}.find("SESSION_PEPPER"), std::string_view::npos);
    }

    EXPECT_THROW(static_cast<void>(required_string(env.lookup(), "EMPTY")), ConfigError);
}

TEST(ConfigEnv, OptionalStringFallsBack) {
    const Env env{{{"SET", "actual"}}};
    EXPECT_EQ(optional_string(env.lookup(), "SET", "fallback"), "actual");
    EXPECT_EQ(optional_string(env.lookup(), "UNSET", "fallback"), "fallback");
}

// --- numbers ---------------------------------------------------------------

TEST(ConfigEnv, NumbersRejectEverythingThatIsNotOne) {
    const Env env{{
        {"GOOD", "4096"},
        {"SPACED", " 4096"},
        {"SIGNED", "-1"},
        {"TRAILING", "4096x"},
        {"WORDS", "many"},
        {"EMPTY_NUM", ""},
    }};

    EXPECT_EQ(optional_number(env.lookup(), "GOOD", 0), 4096U);
    EXPECT_EQ(optional_number(env.lookup(), "UNSET", 77), 77U);
    // Empty is unset, so it takes the fallback rather than failing to parse.
    EXPECT_EQ(optional_number(env.lookup(), "EMPTY_NUM", 77), 77U);

    // from_chars rather than stoull, which accepts leading whitespace and a sign
    // and throws a type that says nothing about which variable was wrong.
    for (const char* key : {"SPACED", "SIGNED", "TRAILING", "WORDS"}) {
        EXPECT_THROW(static_cast<void>(optional_number(env.lookup(), key, 0)), ConfigError)
            << key;
    }
}

TEST(ConfigEnv, BoundedNumberRefusesOutsideItsRange) {
    const Env env{{{"LOW", "1"}, {"HIGH", "1000"}, {"OK", "8"}}};

    EXPECT_EQ(bounded_number(env.lookup(), "OK", 4, 2, 16), 8U);
    EXPECT_EQ(bounded_number(env.lookup(), "UNSET", 4, 2, 16), 4U);
    EXPECT_THROW(static_cast<void>(bounded_number(env.lookup(), "LOW", 4, 2, 16)), ConfigError);
    EXPECT_THROW(static_cast<void>(bounded_number(env.lookup(), "HIGH", 4, 2, 16)), ConfigError);
}

TEST(ConfigEnv, BooleanTyposAreAnErrorNotAFalse) {
    const Env env{{
        {"YES", "yes"}, {"ON", "ON"}, {"ONE", "1"}, {"TRUE", "True"},
        {"NO", "no"},   {"OFF", "off"}, {"ZERO", "0"}, {"FALSE", "FALSE"},
        {"TYPO", "ture"},
    }};

    for (const char* key : {"YES", "ON", "ONE", "TRUE"}) {
        EXPECT_TRUE(optional_bool(env.lookup(), key, false)) << key;
    }
    for (const char* key : {"NO", "OFF", "ZERO", "FALSE"}) {
        EXPECT_FALSE(optional_bool(env.lookup(), key, true)) << key;
    }
    EXPECT_TRUE(optional_bool(env.lookup(), "UNSET", true));

    // The case that matters. A typo in a variable that disables a security
    // control must not read as "disabled".
    EXPECT_THROW(static_cast<void>(optional_bool(env.lookup(), "TYPO", false)), ConfigError);
}

// --- keys ------------------------------------------------------------------

TEST(ConfigEnv, KeyMustBeExactlyTheDeclaredWidth) {
    const std::array<std::uint8_t, 32> material{{1, 2, 3, 4, 5, 6, 7, 8}};
    const std::string encoded = crypto::base64url_encode(material);

    std::array<std::uint8_t, 16> short_material{};
    const std::string too_short = crypto::base64url_encode(short_material);

    const Env env{{{"KEY", encoded}, {"SHORT", too_short}, {"GARBAGE", "not base64url!!"}}};

    crypto::SecretBuffer<32> out{};
    load_key(env.lookup(), "KEY", out, true);
    EXPECT_EQ(std::memcmp(out.span().data(), material.data(), material.size()), 0);

    // A key of the wrong width is not a key. Accepting a short one would mean
    // sealing under material an attacker can guess the rest of.
    crypto::SecretBuffer<32> rejected{};
    EXPECT_THROW(load_key(env.lookup(), "SHORT", rejected, true), ConfigError);
    EXPECT_THROW(load_key(env.lookup(), "GARBAGE", rejected, true), ConfigError);
}

TEST(ConfigEnv, AnAbsentRequiredKeyFailsAndAnAbsentOptionalKeyDoesNot) {
    const Env env{{}};
    crypto::SecretBuffer<32> out{};

    // The distinction is the point. A deployment that booted without a sealing
    // key would encrypt under a zero key and nothing downstream would report it;
    // a deployment with no push key simply has no push.
    EXPECT_THROW(load_key(env.lookup(), "REQUIRED_KEY", out, true), ConfigError);
    EXPECT_NO_THROW(load_key(env.lookup(), "OPTIONAL_KEY", out, false));
}

// --- shapes ----------------------------------------------------------------

TEST(ConfigEnv, OriginShapeMatchesWhatABrowserSends) {
    EXPECT_NO_THROW(require_origin_shape("SITE_ORIGIN", "https://example.test"));
    EXPECT_NO_THROW(require_origin_shape("SITE_ORIGIN", "https://example.test:8443"));
    // http for localhost only, so a developer is not forced to terminate TLS
    // locally.
    EXPECT_NO_THROW(require_origin_shape("SITE_ORIGIN", "http://localhost:8080"));
    EXPECT_NO_THROW(require_origin_shape("SITE_ORIGIN", "http://127.0.0.1:8080"));

    // A trailing slash or a path produces a comparison that can never match the
    // Origin header, and the symptom is every mutating request failing CSRF in
    // production only.
    EXPECT_THROW(require_origin_shape("SITE_ORIGIN", "https://example.test/"), ConfigError);
    EXPECT_THROW(require_origin_shape("SITE_ORIGIN", "https://example.test/app"), ConfigError);
    EXPECT_THROW(require_origin_shape("SITE_ORIGIN", "http://example.test"), ConfigError);
    EXPECT_THROW(require_origin_shape("SITE_ORIGIN", "example.test"), ConfigError);
    EXPECT_THROW(require_origin_shape("SITE_ORIGIN", "https://"), ConfigError);
}

TEST(ConfigEnv, DistinctSettingsMustDiffer) {
    EXPECT_NO_THROW(require_distinct("A", "one", "B", "two"));
    // Naming both is what tells the operator which pair collided.
    try {
        require_distinct("FORM_PII_KEY", "same", "FORM_PII_INDEX_KEY", "same");
        FAIL() << "identical values must not pass";
    } catch (const ConfigError& e) {
        const std::string_view message{e.what()};
        EXPECT_NE(message.find("FORM_PII_KEY"), std::string_view::npos);
        EXPECT_NE(message.find("FORM_PII_INDEX_KEY"), std::string_view::npos);
    }
}

TEST(ConfigEnv, TheContentOriginMustBeADifferentHostFromTheSite) {
    EXPECT_NO_THROW(require_origin_split("SITE_ORIGIN", "https://www.example.test",
                                         "CONTENT_ORIGIN", "https://content.example.test"));

    // The failure this exists for. A deployment with the two set equal passes
    // every other test in the suite while having lost the isolation the split
    // exists for: `__Host-` session cookies are host-only only with respect to a
    // DIFFERENT host, so an XSS in assembled HTML reads the session again.
    EXPECT_THROW(require_origin_split("SITE_ORIGIN", "https://www.example.test",
                                      "CONTENT_ORIGIN", "https://www.example.test"),
                 ConfigError);

    // Byte-distinct and still one host. Cookies are not port-scoped, so a
    // browser holding a `__Host-` cookie for www.example.test sends it to
    // www.example.test:8443 — a plain inequality would have accepted this.
    EXPECT_THROW(require_origin_split("SITE_ORIGIN", "https://www.example.test",
                                      "CONTENT_ORIGIN", "https://www.example.test:8443"),
                 ConfigError);

    // Both still take the shape check, so a split between two malformed origins
    // is not a split that passed.
    EXPECT_THROW(require_origin_split("SITE_ORIGIN", "https://www.example.test/",
                                      "CONTENT_ORIGIN", "https://content.example.test"),
                 ConfigError);
    EXPECT_THROW(require_origin_split("SITE_ORIGIN", "https://www.example.test",
                                      "CONTENT_ORIGIN", "http://content.example.test"),
                 ConfigError);

    try {
        require_origin_split("SITE_ORIGIN", "https://www.example.test", "CONTENT_ORIGIN",
                             "https://www.example.test");
        FAIL() << "one host for both must not pass";
    } catch (const ConfigError& e) {
        const std::string_view message{e.what()};
        EXPECT_NE(message.find("SITE_ORIGIN"), std::string_view::npos);
        EXPECT_NE(message.find("CONTENT_ORIGIN"), std::string_view::npos);
    }
}

TEST(ConfigEnv, DatabaseNameRefusesWhatTheServerWould) {
    EXPECT_NO_THROW(require_database_name("MONGO_DB", "app_production"));

    EXPECT_THROW(require_database_name("MONGO_DB", ""), ConfigError);
    EXPECT_THROW(require_database_name("MONGO_DB", std::string(64, 'a')), ConfigError);
    for (const std::string_view bad : {"has space", "has/slash", "has.dot", "has$dollar"}) {
        EXPECT_THROW(require_database_name("MONGO_DB", bad), ConfigError) << bad;
    }
}

TEST(ConfigEnv, DirectoryMustExistBeAbsoluteAndNotBeASymlink) {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "anvil-config-test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "real");

    EXPECT_THROW(static_cast<void>(require_directory("STORAGE_ROOT", "relative/path")),
                 ConfigError);
    EXPECT_THROW(static_cast<void>(require_directory("STORAGE_ROOT", (base / "missing").string())),
                 ConfigError);

    const std::filesystem::path file = base / "a-file";
    { std::ofstream{file} << "x"; }
    EXPECT_THROW(static_cast<void>(require_directory("STORAGE_ROOT", file.string())),
                 ConfigError);

    // A symlinked root is a root whose destination can be changed by anyone who
    // can write the link, so it is refused rather than followed.
    std::error_code ec;
    std::filesystem::create_directory_symlink(base / "real", base / "link", ec);
    if (!ec) {
        EXPECT_THROW(static_cast<void>(require_directory("STORAGE_ROOT", (base / "link").string())),
                     ConfigError);
    }

    EXPECT_EQ(require_directory("STORAGE_ROOT", (base / "real").string()),
              std::filesystem::canonical(base / "real").string());

    std::filesystem::remove_all(base);
}

// --- containment -----------------------------------------------------------

TEST(ConfigPaths, ContainmentIsByComponentNotByPrefix) {
    EXPECT_TRUE(path_is_within("/srv/storage", "/srv/storage"));
    EXPECT_TRUE(path_is_within("/srv/storage", "/srv/storage/ns/media"));
    EXPECT_TRUE(path_is_within("/srv/storage/", "/srv/storage/ns"));

    // The case the whole function exists for: "/srv/storage-evil" has
    // "/srv/storage" as a string prefix and is a completely different directory.
    EXPECT_FALSE(path_is_within("/srv/storage", "/srv/storage-evil"));
    EXPECT_FALSE(path_is_within("/srv/storage", "/srv/storagex"));
    EXPECT_FALSE(path_is_within("/srv/storage", "/srv"));
    EXPECT_FALSE(path_is_within("/srv/storage", "/other"));
}

TEST(ConfigPaths, WebServedRootsAreRecognised) {
    // A storage root under one of these is reachable without passing through the
    // application at all: `internal;` applies to one location block, and a second
    // block serving the same bytes as static files undoes it in silence.
    EXPECT_TRUE(is_web_served_root("/var/www/html/uploads"));
    EXPECT_TRUE(is_web_served_root("/srv/http/media"));
    EXPECT_TRUE(is_web_served_root("/usr/share/nginx/html"));

    EXPECT_FALSE(is_web_served_root("/srv/app/storage"));
    EXPECT_FALSE(is_web_served_root("/var/lib/app"));
    // Component boundary again: this is not under /var/www.
    EXPECT_FALSE(is_web_served_root("/var/wwwroot"));
}

// --- the boot summary ------------------------------------------------------

TEST(ConfigSummary, RendersValuesAndNeverSecrets) {
    const std::string rendered = RedactedSummary{}
                                     .line("MONGO_DB", "app_production")
                                     .line("DB_POOL_THREADS", std::uint64_t{16})
                                     .secret("SESSION_PEPPER", true)
                                     .secret("VAPID_PRIVATE_KEY", false)
                                     .str();

    EXPECT_NE(rendered.find("MONGO_DB=app_production"), std::string::npos);
    EXPECT_NE(rendered.find("DB_POOL_THREADS=16"), std::string::npos);

    // "set" or "UNSET" — never the value, and never its length, because a length
    // is a meaningful hint about a short secret.
    EXPECT_NE(rendered.find("SESSION_PEPPER=set"), std::string::npos);
    EXPECT_NE(rendered.find("VAPID_PRIVATE_KEY=UNSET"), std::string::npos);
}

}  // namespace
}  // namespace anvil::config
