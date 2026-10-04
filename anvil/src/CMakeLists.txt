# Three libraries, split along the layering in docs/00-architecture.md §2.
#
# anvil_foundation  pure CPU: text, crypto, validation, value types. Depends on
#                   OpenSSL, ICU, simdutf, xxHash and argon2 only. No web
#                   framework, no database driver, no event loop.
# anvil_platform    everything that talks to the outside world.
# anvil_app         the reusable application layer built on top of it.
#
# The split is not cosmetic: it keeps the foundation testable and fast to build,
# and it makes an accidental dependency from a validator onto a repository a LINK
# ERROR rather than a review comment.

# ---------------------------------------------------------------------------
add_library(anvil_foundation STATIC
    core/version.cc
    core/uuid.cc
    core/descriptor_budget.cc

    crypto/aead.cc
    crypto/argon2.cc
    crypto/base64url.cc
    crypto/digest.cc
    crypto/ed25519.cc
    crypto/fast_hash.cc
    crypto/random.cc
    crypto/siv.cc
    crypto/x25519.cc

    i18n/utf8.cc
    i18n/digits.cc
    i18n/bidi.cc
    i18n/normalize.cc

    input/json.cc
    input/schema.cc
    input/fields.cc
    input/email.cc
    input/datetime.cc
    input/url.cc
    input/html.cc

    auth/token.cc
    auth/password.cc
    auth/prehash.cc

    # The account layer's pure half: canonical identifiers. The flows, which
    # need the database and Drogon, are in anvil_app below.
    accounts/identifier.cc
    auth/epoch_cache.cc

    # The two halves of the forms subsystem that need no driver. They are HERE
    # rather than beside the repository so that a locale module — which links
    # nothing but OpenSSL and ICU — can ship a field-type validator of its own.
    forms/validators.cc
    forms/pii.cc

    # The template renderer: a constexpr table lookup and a bounded substitution.
    # Pure CPU, so it sits beside the seam it serves rather than with the
    # repository that stores what it renders.
    notifications/render.cc

    # The image edit's pure half: the recipe codec and the stroke rasteriser.
    # No libvips, so a build without the image subsystem still validates a
    # recipe, and the foundation suite tests both without a decoder.
    images/recipe.cc
    images/stroke_raster.cc

    # Chat's message validators: text, mentions, link previews and reactions.
    # Pure CPU over ICU and the input/ predicates, so the foundation suite
    # tests every rule without a database or a socket.
    chat/text.cc

    # The chat socket's frame codec. Pure bytes with no drogon in it, so the
    # grammar is tested and fuzzed without a socket, and the hub that owns
    # the socket sits above it.
    chat/frames.cc
    chat/hub.cc

    # The metrics half of the analytics subsystem. Pure CPU — atomics, a
    # constexpr table and a string buffer — and it belongs HERE rather than in
    # platform because a counter is incremented inside every layer, so the cell
    # array cannot live above the layer that increments it
    # (docs/00-architecture.md §2).
    analytics/counters.cc
    analytics/snapshot.cc
    analytics/openmetrics.cc

    descriptor/emit.cc

    http/errors.cc
    http/request_id.cc
    http/trace_context.cc
    http/json_writer.cc
    http/csv_writer.cc
    http/html_writer.cc
    http/origin_check.cc
    http/client_address.cc

    fs/paths.cc
    fs/upload.cc

    config/paths.cc
    config/env.cc
)
add_library(anvil::foundation ALIAS anvil_foundation)
anvil_target(anvil_foundation)

target_compile_definitions(anvil_foundation PRIVATE
    ANVIL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ANVIL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ANVIL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    ANVIL_VERSION_STRING="${PROJECT_VERSION}"
)

# argon2 belongs here, not in platform: it is pure CPU with no driver, no socket
# and no event loop, which is exactly the boundary this split draws.
target_link_libraries(anvil_foundation PUBLIC
    OpenSSL::Crypto
    ICU::uc
    ICU::i18n
    ICU::data
    simdutf::simdutf
    xxHash::xxhash
    PkgConfig::ARGON2
)

# ---------------------------------------------------------------------------
# Egyptian national-identity and mobile-number validators.
#
# OPTIONAL and off by default, because they are a locale's rules rather than
# anvil's. They ship because they are self-contained, genuinely reusable, and a
# worked template for a phone_<cc> family: the shape — fold Arabic-Indic digits
# first, validate the calendar rather than the digit pattern, and distinguish
# "not allowed" from "malformed" so the UI can say which — generalises even where
# the digits do not.
if(ANVIL_WITH_EGY)
    target_sources(anvil_foundation PRIVATE
        locale_egy/identity_egy.cc
        locale_egy/phone_egy.cc
    )
    target_compile_definitions(anvil_foundation PUBLIC ANVIL_HAS_EGY=1)
endif()

# ---------------------------------------------------------------------------
# mongo-cxx-driver and redis-plus-plus export differently named targets for
# static and shared builds, and which exists depends on the triplet's
# VCPKG_LIBRARY_LINKAGE. Resolve rather than hardcode, so the build is not
# silently tied to one triplet.
function(anvil_pick_target out_var)
    foreach(candidate IN LISTS ARGN)
        if(TARGET ${candidate})
            set(${out_var} ${candidate} PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "None of these targets exist: ${ARGN}")
endfunction()

anvil_pick_target(ANVIL_MONGOCXX mongo::mongocxx_static mongo::mongocxx_shared)
anvil_pick_target(ANVIL_BSONCXX  mongo::bsoncxx_static  mongo::bsoncxx_shared)
anvil_pick_target(ANVIL_REDISPP  redis++::redis++_static redis++::redis++)

# ---------------------------------------------------------------------------
# thread_pools logs through trantor, so it sits on this side of the boundary even
# though nothing else about it needs a framework. That is the boundary doing its
# job rather than an exception to it: anvil::foundation stays linkable without an
# event loop, which is what makes the foundation test binary fast.
add_library(anvil_platform STATIC
    core/thread_pools.cc

    # The two internal gauges that need a driver or a pool to read, so they
    # cannot live beside the cells in anvil::foundation.
    analytics/gauges.cc

    db/mongo_pool.cc
    db/codec.cc
    db/server_floor.cc
    db/repository.cc
    db/migrations.cc
    db/migration_step.cc
    db/migration_ledger.cc
    db/data_migrations.cc
    db/migrate_cli.cc
    db/collection_options.cc
    db/query_catalogue.cc

    redis/redis_client.cc

    accesscontrol/decision.cc
    accesscontrol/route_projection.cc
    accesscontrol/stealth.cc
    accesscontrol/access_filter.cc
    accesscontrol/upgrade_filter.cc
    accesscontrol/upgrade_gate.cc
    accesscontrol/cookies.cc

    http/content_headers.cc
    http/rate_limit.cc
    http/retry_after.cc
    http/idempotency.cc
    http/client_address_drogon.cc
    http/origin_check_drogon.cc
    http/request_scope.cc

    timer/streams.cc
    timer/promoter.cc
)
add_library(anvil::platform ALIAS anvil_platform)
anvil_target(anvil_platform)

target_link_libraries(anvil_platform PUBLIC
    anvil::foundation
    Drogon::Drogon
    ${ANVIL_MONGOCXX}
    ${ANVIL_BSONCXX}
    ${ANVIL_REDISPP}
    OpenSSL::SSL
)
if(ANVIL_WITH_VIPS)
    target_sources(anvil_platform PRIVATE
        images/probe.cc
        images/strip.cc
        images/variants.cc
        images/edit.cc
    )
    target_link_libraries(anvil_platform PUBLIC PkgConfig::VIPS)
    target_compile_definitions(anvil_platform PUBLIC ANVIL_HAS_VIPS=1)
endif()

if(ANVIL_WITH_RE2)
    target_link_libraries(anvil_platform PUBLIC re2::re2)
    target_compile_definitions(anvil_platform PUBLIC ANVIL_HAS_RE2=1)
endif()

# ---------------------------------------------------------------------------
# The reusable application layer: identity, audit, media, the sections CMS and
# the form maker.
#
# It links anvil::platform and nothing new. That is the layering holding: these
# are repositories and services over the primitives below them, and a dependency
# this target needed that platform did not have would mean something here is
# actually a platform concern.
add_library(anvil_app STATIC
    identity/users.cc
    identity/sessions.cc
    identity/session_service.cc
    identity/authz.cc
    identity/password_service.cc
    identity/prehash_service.cc
    identity/capabilities.cc
    identity/verification.cc
    identity/staff.cc

    accounts/service.cc
    accounts/routes.cc

    audit/buffer.cc
    audit/repository.cc
    audit/service.cc

    # The event half of the analytics subsystem. It needs the driver and the
    # pools, so it sits here rather than beside the cells in anvil::foundation.
    analytics/buffer.cc
    analytics/sessions.cc
    analytics/repository.cc
    analytics/ingest.cc
    analytics/rollup.cc
    analytics/query.cc

    media/grant.cc
    media/repository.cc
    media/service.cc
    media/serving.cc
    media/grant_route.cc

    sections/payload.cc
    sections/repository.cc
    sections/service.cc
    sections/bootstrap.cc

    entries/payload.cc
    entries/repository.cc
    entries/service.cc

    chat/repository.cc
    chat/service.cc
    chat/routes.cc
    chat/render.cc
    chat/live.cc
    chat/socket.cc
    chat/presence.cc
    chat/activity_gate.cc
    chat/wakes.cc
    chat/member_cache.cc
    chat/devices.cc
    chat/prekeys.cc
    chat/device_queue.cc
    chat/push.cc

    forms/repository.cc
    forms/service.cc
    forms/submission_service.cc
    forms/export.cc

    notifications/repository.cc
    notifications/publish.cc
    notifications/inbox.cc
    notifications/sse.cc
    notifications/outbound.cc
    notifications/webhook.cc
    notifications/webpush.cc
    notifications/email.cc
)
add_library(anvil::app ALIAS anvil_app)
anvil_target(anvil_app)

target_link_libraries(anvil_app PUBLIC anvil::platform)

# The image stage of an upload, and the registration of a section's shipped
# default images. These are the only two places either subsystem needs libvips.
#
# A deployment built without the image subsystem still stores, serves and deletes
# objects, and still creates, reads and writes its sections — it simply cannot
# derive new sizes and has no default pictures. Neither row lifecycle may live in
# a translation unit that depends on the encoder, and the no-vips configuration
# is what proves it.
if(ANVIL_WITH_VIPS)
    target_sources(anvil_app PRIVATE
        media/pipeline.cc
        media/edit_routes.cc
        sections/default_images.cc
    )
endif()
