// Drops this process's two scratch databases when the binary is finished.
//
// Linked into EVERY test binary that writes a scratch database, exactly once
// each: a static initialiser in a header would register one environment per
// translation unit that included it.
//
// `drop_scratch_databases` has existed since the fixture did, and its comment
// said it was "registered by whichever suite owns the process" — but nothing
// registered it, so every run left two databases behind with all their
// collections and indexes. It was then registered for anvil_db_tests alone, and
// the listener, peer and load binaries went on leaving two per case: ctest runs
// each case as its own process, so a full suite left a couple of hundred.
// Hundreds of runs later that was 374 databases, and docs/13 §1 is explicit about
// what that costs: a WiredTiger file plus one per index, persistent in-memory
// metadata per table, and a slower `listCollections` every time anything
// starts. On a nearly full copy-on-write filesystem it took one majority write to
// 270 ms and one index build to half a second, so every database case spent
// twenty seconds applying the schema it had just applied in the last process.
//
// It drops only a database THIS process minted (g_scratch_database_owned). A
// process that never asked for one has nothing to drop and does not probe a
// cluster a unit run may not have; a chat peer handed its parent's database
// through ANVIL_TEST_SCRATCH_DB leaves it to the parent.
//
// It runs after the last test in the process, which means it does not run when
// the binary is killed — so this reduces the leak rather than eliminating it. A
// cleanup that cannot survive SIGKILL is the normal case, and the alternative (a
// sweep of every `anvil_t_*` database at startup) would race a concurrently
// running suite for databases that are not its own.

#include <gtest/gtest.h>

#include "app_fixture.h"
#include "db_fixture.h"

namespace {

class ScratchDatabaseCleanup final : public ::testing::Environment {
public:
    void TearDown() override {
        if (!anvil::testfixture::g_scratch_database_owned.load()) { return; }
        if (!anvil::testfixture::pool_ready()) { return; }
        anvil::testfixture::drop_scratch_databases();
    }
};

const ::testing::Environment* kCleanup =
    ::testing::AddGlobalTestEnvironment(new ScratchDatabaseCleanup{});

}  // namespace
