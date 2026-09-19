# Each binary exists because something in it cannot share a process with something
# in another. See docs/16-test-plan.md for the reasoning behind each one.
#
# Phase 0 created the first: anvil_foundation_tests, which links only
# anvil::foundation and is therefore fast enough to run on every save. That speed
# is the point of the target split, not a side effect of it.
#
# Phase 8 added anvil_load_tests, the only one excluded from every default preset:
# it saturates thread pools on purpose.
#
# Phase 10 added anvil_listener_tests, which boots a REAL Drogon listener.
# drogon::app() is a process singleton whose run() does not return until quit(),
# so a second listener cannot share the process with it.

find_package(GTest CONFIG REQUIRED)

# DISCOVERY_MODE PRE_TEST: the default runs the binary at BUILD time to enumerate
# cases, which under ASan on a machine with no database means the build itself
# waits on a connection timeout.
include(GoogleTest)

# ---------------------------------------------------------------------------
add_executable(anvil_foundation_tests
    dependency_smoke_test.cc
    seams_test.cc
    descriptor_test.cc

    uuid_test.cc
    crypto_test.cc
    utf8_test.cc
    i18n_test.cc
    json_test.cc
    fields_test.cc
    html_sanitize_test.cc
    html_writer_test.cc
    errors_test.cc
    response_writer_test.cc
    trace_context_test.cc
    core_types_test.cc
    token_test.cc
    password_test.cc
    filesystem_test.cc
    client_address_test.cc
    config_test.cc
    metrics_test.cc
)

# --------------------------------------------------------------------------
# Replaces global operator new to count allocations, so it cannot share a
# process with anything that allocates for its own reasons.
add_executable(anvil_alloc_tests
    validation_fuzz_test.cc
    metrics_alloc_test.cc
)
anvil_target(anvil_alloc_tests)
target_link_libraries(anvil_alloc_tests PRIVATE
    anvil::foundation
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(anvil_alloc_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_MODE PRE_TEST
)
anvil_target(anvil_foundation_tests)
target_link_libraries(anvil_foundation_tests PRIVATE
    anvil::foundation
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(anvil_foundation_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_MODE PRE_TEST
)

# ---------------------------------------------------------------------------
# Everything that needs a driver linked. Separate from the foundation binary so
# that binary stays driver-free and the layering boundary stays enforceable.
add_executable(anvil_platform_tests
    platform_smoke_test.cc
    testapp/jobs.cc
    testapp/migrations.cc

    codec_test.cc
    access_control_test.cc
    route_declaration_test.cc
    timer_test.cc
    images_test.cc
    migration_seam_test.cc
    migrate_cli_test.cc

    content_headers_test.cc
    upgrade_test.cc
    rate_limit_test.cc
    idempotency_test.cc
    audit_buffer_test.cc
    metrics_wiring_test.cc
    event_seam_test.cc
    event_buffer_test.cc
    sessions_test.cc
    media_serving_test.cc
    identity_test.cc
    seams_phase3_test.cc

    sections_test.cc
    forms_test.cc
    notifications_test.cc
    sse_test.cc
    webhook_test.cc
    webpush_test.cc
    email_test.cc
)
anvil_target(anvil_platform_tests)
target_link_libraries(anvil_platform_tests PRIVATE
    anvil::app
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(anvil_platform_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_MODE PRE_TEST
)

# ---------------------------------------------------------------------------
# One real listener, one process. drogon::app() is a singleton and run() blocks
# until quit(), so this cannot be a fixture inside anvil_platform_tests — and it
# has to exist at all because the defect it guards against is invisible to every
# test that drives a repository, a service or a filter directly: a render path
# that authenticates from a cookie the deployment never delivers.
#
# The files here share that one listener through tests/listener_fixture.h: each
# installs its routes through a registrar and the listener boots from the first
# case that asks for a port. A second binary per listener file would be a process
# per feature, which is not what the singleton costs.
#
# It links anvil::app rather than anvil::platform because the flush-timer cases
# drive AuditService and EventSink, which live there. That is the same link
# anvil_platform_tests already makes, and for the same reason: nothing in
# anvil::app needs a process the other suites must not share.
add_executable(anvil_listener_tests
    preview_listener_test.cc
    session_listener_test.cc
    sink_timer_listener_test.cc
    websocket_route_test.cc
)
anvil_target(anvil_listener_tests)
target_link_libraries(anvil_listener_tests PRIVATE
    anvil::app
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(anvil_listener_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_MODE PRE_TEST
)

# And the same binary ONCE, as one process, which is a different question.
#
# `gtest_discover_tests` gives every case its own ctest entry, and that isolation
# is what makes these parallelisable — but it is also what let a broken stealth
# claim sit on the mainline. The pair asserting that a refused upgrade and an
# unmatched one are byte-identical never shared a listener with anything else, so
# both requests always landed on a fresh IO thread; Drogon sets `Connection:
# close` on the per-IO-thread COPY of the shared 404, so whether the unmatched
# answer carried that header depended on what the thread had served earlier, and
# with one request per process it never had.
#
# A server is one process serving many requests. This entry is that, and it costs
# one more run of a suite that takes seconds.
add_test(NAME listener_in_one_process COMMAND anvil_listener_tests)
set_tests_properties(listener_in_one_process PROPERTIES LABELS "unit")

# ---------------------------------------------------------------------------
# The concurrency suite. Selected by `ctest --preset tsan`; the `database` label
# is excluded there because vcpkg builds libmongoc uninstrumented, so every
# recycled pool client reports as a race that is not one.
add_executable(anvil_concurrency_tests
    thread_pools_test.cc
    metrics_concurrency_test.cc
    epoch_cache_concurrency_test.cc
    sse_concurrency_test.cc
)
anvil_target(anvil_concurrency_tests)
# anvil::app, for the live-stream registry. It is the one piece of the
# notification subsystem with no driver in it at all, and it is exactly the piece
# whose races TSan has to see.
target_link_libraries(anvil_concurrency_tests PRIVATE
    anvil::app
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(anvil_concurrency_tests
    PROPERTIES LABELS "concurrency"
    DISCOVERY_MODE PRE_TEST
)

# ---------------------------------------------------------------------------
# Needs a live MongoDB. Labelled `database` as well, so the tsan preset skips it.
add_executable(anvil_db_tests
    db_fixture_check.cc
    testapp/migrations.cc
    versioned_concurrency_test.cc
    transaction_retry_db_test.cc

    auth_db_test.cc
    audit_db_test.cc
    audit_service_db_test.cc
    media_db_test.cc
    staff_db_test.cc
    verification_db_test.cc
    query_catalogue_db_test.cc
    auth_service_db_test.cc
    media_concurrency_test.cc
    content_db_test.cc
    forms_db_test.cc
    notifications_db_test.cc
    publish_db_test.cc
    inbox_db_test.cc
    outbound_db_test.cc
    analytics_db_test.cc
    rollup_db_test.cc
    index_expansion_db_test.cc
    migration_lock_db_test.cc
    data_migration_db_test.cc
)
anvil_target(anvil_db_tests)
target_link_libraries(anvil_db_tests PRIVATE
    anvil::app
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(anvil_db_tests
    PROPERTIES LABELS "concurrency;database"
    DISCOVERY_MODE PRE_TEST
)

# ---------------------------------------------------------------------------
# The load gate. One binary, labelled `load` and EXCLUDED from every default
# preset, because it saturates pools on purpose — which is antisocial to run
# beside a suite that assumes it can post a task.
#
# It is not a benchmark suite and it measures no latencies. It exists because two
# real defects were invisible to 1,088 passing tests and neither of them is a
# timing property: an audit sink that dropped 8,485 batches against a live, busy
# mongod, and 393,116 transaction retries with no backoff behind them. See
# docs/16-test-plan.md.
add_executable(anvil_load_tests
    load_gate_test.cc
    testapp/migrations.cc
)
anvil_target(anvil_load_tests)
target_link_libraries(anvil_load_tests PRIVATE
    anvil::app
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(anvil_load_tests
    PROPERTIES LABELS "load;database"
    DISCOVERY_MODE PRE_TEST
)

# ---------------------------------------------------------------------------
# The reference application's descriptor emitter. It links anvil::foundation and
# nothing else — the whole point of the seam being spans of constexpr tables is
# that generating a client needs no driver, no event loop and no database.
add_executable(testapp_emit_descriptor
    testapp/emit_descriptor.cc
)
anvil_target(testapp_emit_descriptor)
target_link_libraries(testapp_emit_descriptor PRIVATE anvil::foundation)

# ---------------------------------------------------------------------------
# The reference application, on a port.
#
# It is a TEST BINARY and it stays one. The five rules that keep it from becoming
# a deployment are in tests/testapp/reference_server.cc, and the one this file is
# responsible for is the last of them: **it is never install()ed**. anvil declares
# no install() rules at all today, so nothing here has to opt out — what this
# comment does is make adding one a deliberate act rather than a side effect of
# somebody adding packaging.
#
# It links anvil::app, because it drives the real session service, the real
# authz service and the real access filter. A reference server standing in for
# any of those would be demonstrating a system this library does not have.
add_executable(anvil_reference_server
    testapp/reference_server.cc
    testapp/migrations.cc
)
anvil_target(anvil_reference_server)
target_link_libraries(anvil_reference_server PRIVATE anvil::app)

# "The reference server starts in CI and answers a request" — one third of the
# phase-12 gate, and a thing no unit test can express: anvil_listener_tests boots
# Drogon in-process, and nothing had ever started the reference APPLICATION as a
# process against a real MongoDB and a real Redis.
#
# SKIP_RETURN_CODE 77 is the distinction the script is built around: the server
# exits 3 when a dependency is unreachable, which is a skip, and 1 when it refuses
# for any other reason, which is a failure. One non-zero exit for both would turn
# "you pointed me at somebody's data" into a green run on a machine with no
# database.
#
# Labelled `database` as well, so the tsan preset skips it for the same reason it
# skips anvil_db_tests: vcpkg builds libmongoc uninstrumented.
add_test(NAME reference_server
         COMMAND "${ANVIL_SOURCE_DIR}/tools/check-reference-server.sh"
                 "$<TARGET_FILE:anvil_reference_server>")
set_tests_properties(reference_server PROPERTIES
                     LABELS "database"
                     SKIP_RETURN_CODE 77
                     TIMEOUT 180)

# ---------------------------------------------------------------------------
# Source rules that no unit test can express: they are about what the source is
# ALLOWED to contain, not about what it computes. Running them as CTest entries
# rather than as a CI-only step means a violation fails the same command a
# developer already runs.
add_test(NAME source_bans
         COMMAND "${ANVIL_SOURCE_DIR}/tools/check-source-bans.sh")
set_tests_properties(source_bans PROPERTIES LABELS "lint")

add_test(NAME db_discipline
         COMMAND "${ANVIL_SOURCE_DIR}/tools/check-db-discipline.sh")
set_tests_properties(db_discipline PROPERTIES LABELS "lint")

add_test(NAME vocabulary
         COMMAND "${ANVIL_SOURCE_DIR}/tools/check-vocabulary.sh")
set_tests_properties(vocabulary PROPERTIES LABELS "lint")

# The two files a release publishes, checked the way the release step checks
# them: emitted twice and diffed.
#
# It runs in the suite rather than only at a tag because determinism is not a
# packaging detail — the descriptor's hash is what tells a long-lived tab whether
# it is talking to the server it was built from, and a hash over bytes that are
# not reproducible answers that question wrongly in the safe-looking direction
# (docs/01-seams.md §14). A tag is the worst moment to find out.
add_test(NAME release_artefacts
         COMMAND "${ANVIL_SOURCE_DIR}/tools/release-artefacts.sh" --check
                 "$<TARGET_FILE:anvil_emit_envelopes>"
                 "$<TARGET_FILE:testapp_emit_descriptor>")
set_tests_properties(release_artefacts PROPERTIES LABELS "lint")
