// The permission grid against a live cluster, and the one invariant a
// transaction alone does not enforce.
//
// "The last account of a privileged type cannot be demoted or disabled" is a
// MULTI-DOCUMENT invariant, and MongoDB gives snapshot isolation rather than
// serialisability. Two transactions demoting two different privileged accounts
// each count the other as still active, neither writes a document the other
// wrote, and both commit — the population reaches zero with no error anywhere.
// The concurrent case below is the one that demonstrates it.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "anvil/core/uuid.h"
#include "anvil/identity/authz.h"
#include "anvil/identity/staff.h"
#include "anvil/identity/users.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/perms.h"

namespace {

using anvil::PermSet;
using anvil::Uuid;
using anvil::UserStatus;
using anvil::UserType;
using anvil::identity::AuthzService;
using anvil::identity::NewUser;
using anvil::identity::RoleTable;
using anvil::identity::StaffService;
using anvil::identity::UserRepository;
using anvil::testfixture::scratch_names;

constexpr std::string_view kUsers = "users";
constexpr std::string_view kSessions = "user_sessions";
constexpr std::string_view kGuard = "staff_guard";

class StaffDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        // Every write here bumps perm_epoch, and that goes through the Redis
        // mirror. Without it there is no revocation channel and the assertions
        // would be about half a mechanism.
        ANVIL_REQUIRE_REDIS();
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kUsers);
        anvil::testfixture::clear_collection(**client_, kSessions);
        anvil::testfixture::clear_collection(**client_, kGuard);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static UserRepository users() {
        return UserRepository{std::string{scratch_names().for_collection(kUsers)}, kUsers};
    }

    // NOT [[nodiscard]]: several cases create an account purely so the
    // privileged population is above one, and have no use for its id.
    Uuid create(std::string_view local, UserType type, UserStatus status) {
        const Uuid id = anvil::uuid::generate_v7();
        const std::string email = std::string{local} + "@example.test";
        const std::string username{local};
        EXPECT_TRUE(users()
                        .insert(db(), NewUser{
                                          .id = id,
                                          .email_normalised = email,
                                          .email_display = email,
                                          .username_normalised = username,
                                          .username_display = username,
                                          .password_hash = "$argon2id$x",
                                          .phone_e164 = {},
                                          .locale = anvil::Locale{},
                                          .status = status,
                                      })
                        .ok());
        if (type != UserType::Client) {
            // Promoted through the repository, so the row under test is one this
            // system actually produces.
            auto session = db().start_session();
            session.start_transaction();
            EXPECT_TRUE(
                users()
                    .set_user_type(db(), session, id, 1, type, PermSet{}, PermSet{})
                    .ok());
            session.commit_transaction();
        }
        return id;
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

[[nodiscard]] AuthzService& authz() {
    static AuthzService service{std::string{scratch_names().for_collection(kUsers)}, kUsers};
    return service;
}

[[nodiscard]] StaffService staff() {
    return StaffService{std::string{scratch_names().for_collection(kUsers)}, kUsers, kSessions,
                        kGuard, authz()};
}

[[nodiscard]] std::int64_t stored_epoch(mongocxx::client& client, const Uuid& id) {
    const auto found = UserRepository{std::string{scratch_names().for_collection(kUsers)},
                                      kUsers}
                           .find_permissions(client, id);
    EXPECT_TRUE(found.ok());
    EXPECT_TRUE(found.value().has_value());
    return found.value()->perm_epoch;
}

// --- permission changes -----------------------------------------------------

TEST_F(StaffDb, APermissionChangeBumpsTheEpoch) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid target = create("grid", UserType::Staff, UserStatus::Active);
    const std::int64_t before = stored_epoch(db(), target);

    const auto found = users().find_account(db(), target);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());

    StaffService service = staff();
    const auto changed = service.set_permissions(db(), target, found.value()->version,
                                                 testapp::kContentAuthor, {}, RoleTable{});
    ASSERT_TRUE(changed.ok());

    // Without the bump, the outstanding access token keeps the old permissions
    // until it expires — which for a privileged account is exactly the window
    // the epoch channel exists to close.
    EXPECT_GT(stored_epoch(db(), target), before);
}

TEST_F(StaffDb, APermissionChangeReportsBothHalvesForTheAuditRow) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid target = create("both", UserType::Staff, UserStatus::Active);
    const auto found = users().find_account(db(), target);
    ASSERT_TRUE(found.ok());

    StaffService service = staff();
    const auto changed = service.set_permissions(db(), target, found.value()->version,
                                                 testapp::kFormSupervisor, {}, RoleTable{});
    ASSERT_TRUE(changed.ok());

    // A row saying only "somebody's permissions changed" cannot answer the
    // question an audit log is read to answer.
    EXPECT_EQ(changed.value().before.count(), 0U);
    EXPECT_EQ(changed.value().after, testapp::kFormSupervisor);
}

TEST_F(StaffDb, TheDirectGrantsAndTheStoredUnionMoveTogether) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid role_id = anvil::uuid::generate_v7();
    const RoleTable roles{{{role_id, testapp::kFormSupervisor}}};
    const std::array<Uuid, 1> held{role_id};

    const Uuid target = create("union", UserType::Staff, UserStatus::Active);
    const auto found = users().find_account(db(), target);
    ASSERT_TRUE(found.ok());

    StaffService service = staff();
    ASSERT_TRUE(service
                    .set_permissions(db(), target, found.value()->version,
                                     testapp::kContentAuthor, held, roles)
                    .ok());

    const auto after = users().find_account(db(), target);
    ASSERT_TRUE(after.ok());
    // `perms` is what was edited; `eff` is the union, computed at write time so
    // a read is one 16-byte load and one AND.
    EXPECT_EQ(after.value()->direct_permissions, testapp::kContentAuthor);
    EXPECT_TRUE(after.value()->effective_permissions.test(
        static_cast<std::size_t>(testapp::Perm::ContentWrite)));
    EXPECT_TRUE(after.value()->effective_permissions.test(
        static_cast<std::size_t>(testapp::Perm::FormPii)));
}

TEST_F(StaffDb, AStaleVersionRefusesRatherThanOverwriting) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid target = create("stale", UserType::Staff, UserStatus::Active);
    const auto found = users().find_account(db(), target);
    ASSERT_TRUE(found.ok());
    const std::int64_t version = found.value()->version;

    StaffService service = staff();
    ASSERT_TRUE(
        service.set_permissions(db(), target, version, testapp::kContentAuthor, {}, RoleTable{})
            .ok());

    // Two administrators with the grid open on the same person is the NORMAL
    // case for a small team, and an unconditional write loses one of them in
    // silence.
    const auto second =
        service.set_permissions(db(), target, version, testapp::kFormSupervisor, {},
                                RoleTable{});
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(second.error().code, anvil::ErrorCode::VersionMismatch);
}

// --- the population invariant -----------------------------------------------

TEST_F(StaffDb, TheLastPrivilegedAccountCannotBeDemoted) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid only = create("only", UserType::SuperAdmin, UserStatus::Active);
    const auto found = users().find_account(db(), only);
    ASSERT_TRUE(found.ok());

    StaffService service = staff();
    const auto demoted =
        service.set_user_type(db(), only, found.value()->version, UserType::Staff, PermSet{}, {},
                              RoleTable{}, UserType::SuperAdmin);
    ASSERT_FALSE(demoted.ok());
    // Conflict, not Forbidden: nothing about the caller's authority is wrong,
    // and the remedy is to promote somebody else first.
    EXPECT_EQ(demoted.error().code, anvil::ErrorCode::Conflict);

    const auto unchanged = users().find_account(db(), only);
    ASSERT_TRUE(unchanged.ok());
    EXPECT_EQ(unchanged.value()->user_type, UserType::SuperAdmin);
}

TEST_F(StaffDb, TheLastPrivilegedAccountCannotBeDisabled) {
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid only = create("onlydisable", UserType::SuperAdmin, UserStatus::Active);

    StaffService service = staff();
    const auto disabled = service.set_status(db(), only, UserStatus::Disabled,
                                             UserType::SuperAdmin, anvil::db::now_ms());
    ASSERT_FALSE(disabled.ok());
    EXPECT_EQ(disabled.error().code, anvil::ErrorCode::Conflict);
}

TEST_F(StaffDb, DemotingOneOfTwoPrivilegedAccountsIsAllowed) {
    ANVIL_REQUIRE_TRANSACTIONS();
    create("keep", UserType::SuperAdmin, UserStatus::Active);
    const Uuid going = create("going", UserType::SuperAdmin, UserStatus::Active);
    const auto found = users().find_account(db(), going);
    ASSERT_TRUE(found.ok());

    StaffService service = staff();
    const auto demoted =
        service.set_user_type(db(), going, found.value()->version, UserType::Staff, PermSet{},
                              {}, RoleTable{}, UserType::SuperAdmin);
    ASSERT_TRUE(demoted.ok());
    EXPECT_EQ(demoted.value().before, UserType::SuperAdmin);
    EXPECT_EQ(demoted.value().after, UserType::Staff);
}

TEST_F(StaffDb, TwoConcurrentDemotionsCannotBothSucceedAndEmptyThePopulation) {
    // THE case the guard document exists for. Snapshot isolation alone lets both
    // transactions commit: each counts the other as still active, and neither
    // writes a document the other wrote. The shared guard is what makes them
    // collide, so one is rolled back and retried against the other's committed
    // state.
    ANVIL_REQUIRE_TRANSACTIONS();
    const Uuid first = create("racer1", UserType::SuperAdmin, UserStatus::Active);
    const Uuid second = create("racer2", UserType::SuperAdmin, UserStatus::Active);

    const auto found_first = users().find_account(db(), first);
    const auto found_second = users().find_account(db(), second);
    ASSERT_TRUE(found_first.ok());
    ASSERT_TRUE(found_second.ok());

    std::atomic<int> succeeded{0};
    const auto demote = [&succeeded](const Uuid& id, std::int64_t version) {
        auto client = anvil::db::MongoPool::instance().acquire();
        StaffService service = staff();
        const auto result = service.set_user_type(*client, id, version, UserType::Staff,
                                                  PermSet{}, {}, RoleTable{},
                                                  UserType::SuperAdmin);
        if (result.ok()) { succeeded.fetch_add(1, std::memory_order_relaxed); }
    };

    std::thread one{demote, first, found_first.value()->version};
    std::thread two{demote, second, found_second.value()->version};
    one.join();
    two.join();

    EXPECT_EQ(succeeded.load(), 1);

    const auto remaining =
        users().list_accounts(db(), anvil::identity::AccountQuery{
                                        .user_type = UserType::SuperAdmin, .limit = 10});
    ASSERT_TRUE(remaining.ok());
    // The population never reaches zero. That is the whole invariant.
    EXPECT_EQ(remaining.value().size(), 1U);
}

// --- status changes ---------------------------------------------------------

TEST_F(StaffDb, DisablingReportsThePreImageAndBumpsTheEpoch) {
    ANVIL_REQUIRE_TRANSACTIONS();
    create("other-admin", UserType::SuperAdmin, UserStatus::Active);
    const Uuid target = create("disable-me", UserType::Staff, UserStatus::Active);
    const std::int64_t before = stored_epoch(db(), target);

    StaffService service = staff();
    const auto disabled = service.set_status(db(), target, UserStatus::Disabled,
                                             UserType::SuperAdmin, anvil::db::now_ms());
    ASSERT_TRUE(disabled.ok());
    // The audit row records what the account changed FROM, and reading that
    // separately would record a value that was true a moment earlier.
    EXPECT_EQ(disabled.value(), UserStatus::Active);
    EXPECT_GT(stored_epoch(db(), target), before);
}

TEST_F(StaffDb, DisablingLeavesThePermissionGridUntouched) {
    ANVIL_REQUIRE_TRANSACTIONS();
    create("admin2", UserType::SuperAdmin, UserStatus::Active);
    const Uuid target = create("keeps-grid", UserType::Staff, UserStatus::Active);

    const auto found = users().find_account(db(), target);
    ASSERT_TRUE(found.ok());
    StaffService service = staff();
    ASSERT_TRUE(service
                    .set_permissions(db(), target, found.value()->version,
                                     testapp::kContentAuthor, {}, RoleTable{})
                    .ok());
    ASSERT_TRUE(service
                    .set_status(db(), target, UserStatus::Disabled, UserType::SuperAdmin,
                                anvil::db::now_ms())
                    .ok());

    const auto after = users().find_account(db(), target);
    ASSERT_TRUE(after.ok());
    // Turning an account back on restores exactly what it had, which is what an
    // enable/disable control promises — clearing the grid would make re-enabling
    // a reconstruction job.
    EXPECT_EQ(after.value()->direct_permissions, testapp::kContentAuthor);
    EXPECT_EQ(after.value()->status, UserStatus::Disabled);
}

TEST_F(StaffDb, ChangingTheStatusOfNoSuchAccountIsNotFound) {
    ANVIL_REQUIRE_TRANSACTIONS();
    StaffService service = staff();
    const auto missing = service.set_status(db(), anvil::uuid::generate_v7(),
                                            UserStatus::Disabled, UserType::SuperAdmin,
                                            anvil::db::now_ms());
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, anvil::ErrorCode::NotFound);
}

TEST_F(StaffDb, AnAccountListingIsOrderedAndPaginatesByItsCompoundCursor) {
    ANVIL_REQUIRE_TRANSACTIONS();
    for (int i = 0; i < 3; ++i) {
        create("client" + std::to_string(i), UserType::Client, UserStatus::Active);
    }
    for (int i = 0; i < 3; ++i) {
        create("staff" + std::to_string(i), UserType::Staff, UserStatus::Active);
    }

    std::vector<Uuid> seen;
    std::optional<anvil::identity::AccountCursor> cursor;
    for (int page = 0; page < 6; ++page) {
        const auto rows = users().list_accounts(
            db(), anvil::identity::AccountQuery{.after = cursor, .limit = 2});
        ASSERT_TRUE(rows.ok());
        if (rows.value().empty()) { break; }
        for (const auto& row : rows.value()) { seen.push_back(row.id); }
        const auto& last = rows.value().back();
        cursor = anvil::identity::AccountCursor{last.id, last.user_type};
    }

    EXPECT_EQ(seen.size(), 6U);
    std::sort(seen.begin(), seen.end());
    // A cursor on `_id` alone could not express a position inside the
    // (user_type, _id) order, and would serve or skip rows at every boundary.
    EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
}

TEST_F(StaffDb, AListingNarrowsByTypeAndByStatusIndependently) {
    ANVIL_REQUIRE_TRANSACTIONS();
    create("a-client", UserType::Client, UserStatus::Active);
    create("a-staff", UserType::Staff, UserStatus::Active);
    create("d-staff", UserType::Staff, UserStatus::Disabled);

    const auto staff_rows = users().list_accounts(
        db(), anvil::identity::AccountQuery{.user_type = UserType::Staff, .limit = 10});
    ASSERT_TRUE(staff_rows.ok());
    EXPECT_EQ(staff_rows.value().size(), 2U);

    const auto disabled = users().list_accounts(
        db(), anvil::identity::AccountQuery{.status = UserStatus::Disabled, .limit = 10});
    ASSERT_TRUE(disabled.ok());
    EXPECT_EQ(disabled.value().size(), 1U);
}

TEST_F(StaffDb, AUsernamePrefixMatchesAnchoredAndAnEmailMustBeKnownInFull) {
    ANVIL_REQUIRE_TRANSACTIONS();
    create("alpha-one", UserType::Client, UserStatus::Active);
    create("alpha-two", UserType::Client, UserStatus::Active);
    create("beta-one", UserType::Client, UserStatus::Active);

    const auto prefixed = users().list_accounts(
        db(), anvil::identity::AccountQuery{.username_prefix = "alpha", .limit = 10});
    ASSERT_TRUE(prefixed.ok());
    EXPECT_EQ(prefixed.value().size(), 2U);

    // Anchored: a prefix is a RANGE over the index, never a substring search.
    const auto substring = users().list_accounts(
        db(), anvil::identity::AccountQuery{.username_prefix = "one", .limit = 10});
    ASSERT_TRUE(substring.ok());
    EXPECT_TRUE(substring.value().empty());

    // An address is an EQUALITY. A substring search across every address in the
    // system is a harvesting tool with a staff session in front of it.
    const auto partial_email = users().list_accounts(
        db(), anvil::identity::AccountQuery{.email_normalised = "alpha", .limit = 10});
    ASSERT_TRUE(partial_email.ok());
    EXPECT_TRUE(partial_email.value().empty());

    const auto exact_email = users().list_accounts(
        db(), anvil::identity::AccountQuery{.email_normalised = "alpha-one@example.test",
                                            .limit = 10});
    ASSERT_TRUE(exact_email.ok());
    EXPECT_EQ(exact_email.value().size(), 1U);
}

}  // namespace
