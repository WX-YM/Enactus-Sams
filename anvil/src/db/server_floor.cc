#include "anvil/db/server_floor.h"

#include <charconv>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <bsoncxx/stdx/string_view.hpp>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/operation_exception.hpp>

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

[[nodiscard]] std::string read_string(const bsoncxx::document::view& doc, std::string_view field) {
    const bsoncxx::document::element element =
        doc[bsoncxx::stdx::string_view{field.data(), field.size()}];
    if (!element || element.type() != bsoncxx::type::k_string) { return {}; }
    const auto value = element.get_string().value;
    return std::string{value.data(), value.size()};
}

// "7.0.14" -> {7, 0}. Hand-written rather than sscanf: from_chars neither
// allocates nor consults the locale, and a malformed version string must read
// as "below the floor" rather than as an exception from a parser.
[[nodiscard]] std::pair<std::int32_t, std::int32_t> parse_version(std::string_view version) {
    const auto component = [&version](std::size_t from) -> std::pair<std::int32_t, std::size_t> {
        std::size_t end = from;
        while (end < version.size() && version[end] >= '0' && version[end] <= '9') { ++end; }
        std::int32_t value = 0;
        if (end > from) {
            std::from_chars(version.data() + from, version.data() + end, value);
        }
        return {value, end};
    };

    const auto [major, after_major] = component(0);
    if (after_major >= version.size() || version[after_major] != '.') { return {major, 0}; }
    const auto [minor, after_minor] = component(after_major + 1);
    (void)after_minor;
    return {major, minor};
}

}  // namespace

ServerCheck assert_server_supported(mongocxx::client& client) {
    mongocxx::database admin = client["admin"];

    const auto build_info = admin.run_command(make_document(kvp("buildInfo", 1)));
    ServerCheck check{};
    check.version = read_string(build_info.view(), "version");
    const auto [major, minor] = parse_version(check.version);
    check.major = major;
    check.minor = minor;

    if (major < kMinServerMajor || (major == kMinServerMajor && minor < kMinServerMinor)) {
        throw std::runtime_error{"MongoDB " + check.version + " is below the required floor of " +
                                 std::to_string(kMinServerMajor) + "." +
                                 std::to_string(kMinServerMinor)};
    }

    const auto hello = admin.run_command(make_document(kvp("hello", 1)));
    // setName is present only on a member of a replica set. Transactions
    // require one, and a standalone that silently accepts every write until the
    // first transaction is the worst possible time to find out.
    check.is_replica_set = static_cast<bool>(hello.view()["setName"]);
    if (!check.is_replica_set) {
        throw std::runtime_error{
            "the configured MongoDB is not a replica set; transactions require one"};
    }

    // getParameter is not available to every deployment user, and being unable
    // to read FCV is not a reason to refuse to start.
    try {
        const auto parameter = admin.run_command(make_document(
            kvp("getParameter", 1), kvp("featureCompatibilityVersion", 1)));
        const bsoncxx::document::element fcv = parameter.view()["featureCompatibilityVersion"];
        if (fcv && fcv.type() == bsoncxx::type::k_document) {
            check.feature_compatibility = read_string(fcv.get_document().value, "version");
        }
    } catch (const mongocxx::operation_exception&) {
        check.feature_compatibility.clear();
    }

    if (!check.feature_compatibility.empty()) {
        const auto [fcv_major, fcv_minor] = parse_version(check.feature_compatibility);
        check.feature_compatibility_lags =
            fcv_major < major || (fcv_major == major && fcv_minor < minor);
    }

    return check;
}

}  // namespace anvil::db
