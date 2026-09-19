#pragma once

// Reading and writing section content.
//
// A section read is the hottest path a CMS has: every page render reads one. The
// read path is therefore three tiers, and the top one is the only one that runs
// in steady state.
//
//   1. PROCESS-LOCAL. A `shared_ptr<const SerializedSection>` per (section,
//      locale), swapped atomically on invalidation. A hit is one atomic load and
//      a write() of bytes that are already serialised — zero JSON serialisation,
//      zero BSON decode, zero allocation, zero I/O. It runs on the event-loop
//      thread because it does nothing that could block.
//   2. REDIS, with a short TTL. Blocking, so db_pool only.
//   3. MONGODB, then populate both caches above.
//
// The whole published set is a few hundred kilobytes and is preloaded at boot,
// so tier 3 should essentially never be reached in steady state.
//
// --- invalidation is the part that is easy to get wrong ---------------------
//
// Deleting the Redis key is NOT enough. Every other process still holds its own
// tier-1 entry and would serve stale content until that entry aged out — so
// staff see their change appear and disappear depending on which instance
// answered, which is a confusing and nearly unreproducible bug. A write
// therefore also PUBLISHES the key on a Redis channel, and every instance
// subscribes and drops its local entry on receipt.
//
// Redis being down degrades this and nothing else: reads fall back to MongoDB,
// writes still succeed, and only cross-instance invalidation stops working. That
// degradation is logged rather than silent.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <mongocxx/client.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/fs/namespace.h"
#include "anvil/media/service.h"
#include "anvil/sections/content.h"
#include "anvil/sections/payload.h"
#include "anvil/sections/registry.h"
#include "anvil/sections/repository.h"

namespace anvil::sections {

struct SectionServiceConfig final {
    // Views into the frozen configuration, which outlives every request.

    // The one absolute origin an `<img src>` inside a RichText field may name.
    std::string_view     content_origin;
    // The whole URL prefix an image id is appended to — origin, path and
    // namespace segment. Built once by the application, because how a media
    // object is addressed is a routing decision anvil does not make.
    std::string_view     image_url_base;
    // Namespaces the Redis keys and the invalidation channel. anvil is a
    // library: two applications sharing one Redis must not share a cache key,
    // and a constant here would make that a deployment hazard rather than a
    // configuration choice.
    std::string_view     cache_prefix{"sec"};
    std::chrono::seconds redis_ttl{300};
    // Which media namespace a section's images live in. Part of every media
    // lookup, which is what stops an id uploaded for another API from being
    // attached to a section slot.
    fs::Ns               image_namespace;

    // --- the one member that is not a view ----------------------------------

    // Called once per invalidated section key, on whichever thread invalidated
    // it, AFTER this instance's tier-1 entry for that key has been dropped.
    // Empty by default, and an empty hook costs one branch.
    //
    // It exists because a cache ABOVE this one cannot otherwise be told a key
    // changed: `invalidate_local` and the Redis subscriber are both internal, so
    // a consumer holding a derived snapshot — typed values for a renderer, a
    // pre-rendered fragment — has no way to hear about a write. Both workarounds
    // are worse than a hook. Dropping and refilling from the application's own
    // write handler misses every write that happened on another instance, which
    // is the bug the Redis channel exists to fix, reintroduced one layer up.
    // Subscribing to the same channel separately is a second listener thread
    // doing what this one already does.
    //
    // THREE things about when it is called, each of which is the difference
    // between a working hook and one of those workarounds:
    //
    //   It runs on the SUBSCRIBER's path as well as the local write path. A hook
    //   that only fired where the write happened would be the first workaround
    //   with anvil's name on it.
    //
    //   It runs AFTER the local entry is dropped, never before, so a consumer
    //   that re-reads on the callback cannot read back the stale value it was
    //   just told about.
    //
    //   It MUST NOT BLOCK. On the subscriber's path it runs on the one thread
    //   draining the subscription, so anything slow here stops every other
    //   instance's invalidations from being noticed. Post the work; do not do
    //   it here. It must also not re-enter the service: calling back into a
    //   write from inside it invalidates while invalidating.
    //
    // An exception escaping it is caught and logged rather than allowed to
    // propagate — `invalidate_local` is noexcept and the subscriber thread must
    // survive a consumer's bug (ENGINEERING_RULES.md §4).
    //
    // `key` is always a key the registry declares. A published key that is not
    // in the table is discarded before this is reached, which is the same
    // allow-list treatment a request gets.
    std::function<void(std::string_view key)> on_invalidated;
};

struct SectionWriteOutcome final {
    std::array<char, 18> etag;
    std::int64_t         version;
};

class SectionService final {
public:
    // `registry` must outlive the service — it is a view of the application's
    // `constexpr` table, which lives in `.rodata` and outlives everything.
    SectionService(std::string database, std::string_view collection,
                   std::span<const SectionSpec> registry, media::MediaService& media,
                   SectionServiceConfig config);
    ~SectionService();

    // --- read ---------------------------------------------------------------

    // Tier 1 only. No I/O, no lock, no allocation — safe to call from a Trantor
    // event-loop thread, which is the entire point. nullptr means "not cached
    // here", NEVER "does not exist".
    [[nodiscard]] std::shared_ptr<const SerializedSection> peek(const SectionSpec& spec,
                                                                Locale locale) const noexcept;

    // Tiers 2 and 3, populating tier 1 on the way back. BLOCKING: db_pool only.
    // A null payload means the section has never been written, which for a
    // registry key is a bootstrap failure rather than a client error.
    [[nodiscard]] Result<std::shared_ptr<const SerializedSection>> load(
        mongocxx::client& client, const SectionSpec& spec, Locale locale);

    // Every published section in every locale, into tier 1. Called once at boot
    // so a cold miss never happens on a real request.
    [[nodiscard]] Result<std::size_t> preload(mongocxx::client& client);

    // --- write --------------------------------------------------------------

    // Reads the stored document, merges `patch` over it, enforces `required`
    // against the MERGED result, verifies every image against its ImageSpec, and
    // writes — all inside one transaction that also adjusts media reference
    // counts. BLOCKING: db_pool only.
    //
    // Returns VersionMismatch when another writer got there first, which is the
    // expected outcome of two staff editing one section, not an exception.
    [[nodiscard]] Result<SectionWriteOutcome> write(mongocxx::client& client,
                                                    const SectionSpec& spec, SectionState state,
                                                    std::int64_t expected_version,
                                                    const SectionContent& patch,
                                                    const Uuid& actor);

    // The stored content, for callers that need the VALUES rather than the
    // rendered bytes — a draft derived from the live section, or a snapshot.
    [[nodiscard]] Result<std::optional<SectionDocument>> read_document(
        mongocxx::client& client, const SectionSpec& spec, SectionState state) const;

    // Every image slot in `content` must name media that exists, lives in the
    // configured namespace, and MEETS the slot's ImageSpec. A hero slot needing
    // 1920x1080 rejects a 200x200 upload rather than letting the public site
    // render broken.
    [[nodiscard]] Status verify_images(mongocxx::client& client, const SectionSpec& spec,
                                       const SectionContent& content) const;

    // --- invalidation -------------------------------------------------------

    // Drops the local entry only, then calls `config.on_invalidated` if there is
    // one. Called by the subscriber when another instance publishes, and by the
    // local write path — which is what makes the hook hear about both.
    void invalidate_local(std::string_view key) noexcept;

    // Local drop + Redis DEL + PUBLISH. Never throws and never fails a request:
    // a Redis outage degrades cross-instance freshness and nothing else.
    void publish_invalidation(std::string_view key) noexcept;

    // ONE dedicated thread, because a Redis subscribe loop never returns and so
    // cannot live on a bounded pool without permanently consuming one of its
    // workers. Reconnects with backoff; a failure here is logged and degrades
    // freshness, never availability.
    void start_invalidation_listener();
    void stop_invalidation_listener() noexcept;

    [[nodiscard]] const SectionRepository& repository() const noexcept { return sections_; }
    [[nodiscard]] std::span<const SectionSpec> registry() const noexcept { return registry_; }

    SectionService(const SectionService&) = delete;
    SectionService& operator=(const SectionService&) = delete;

private:
    // Tier 1. One slot per (registry index, locale).
    //
    // std::atomic over a shared_ptr rather than a mutex-guarded map: readers then
    // need no lock at all, and a swap during a read leaves the reader holding the
    // old payload alive rather than reading a half-updated one.
    using Slot = std::atomic<std::shared_ptr<const SerializedSection>>;

    // The first draft of a section, derived from the published document. Called
    // by `write` and only by it, for the one state that is allowed not to exist
    // yet: bootstrap seeds a Published document per registry section and no
    // Draft of any of them.
    [[nodiscard]] Result<SectionWriteOutcome> create_first_draft(mongocxx::client& client,
                                                                 const SectionSpec& spec,
                                                                 const SectionContent& patch,
                                                                 const Uuid& actor);

    [[nodiscard]] std::string cache_key(std::string_view section_key, Locale locale) const;
    void store_local(std::size_t index, Locale locale,
                     std::shared_ptr<const SerializedSection> payload) noexcept;
    void listener_loop() noexcept;

    // Declaration order is construction order: sections_ is built from
    // database_, cache_ is sized from registry_, and listener_ is started last,
    // after everything it touches exists (ENGINEERING_RULES.md §3.2).
    const std::string            database_;
    std::span<const SectionSpec> registry_;
    media::MediaService&         media_;
    const SectionServiceConfig   config_;
    // Built once at construction from the prefix, rather than concatenated per
    // publish. A channel name is not something to allocate on a write path.
    const std::string            channel_;
    SectionRepository            sections_;

    // ONE heap allocation, at construction, for registry.size() * kLocaleCount
    // slots — never a std::vector, whose reallocation would move the atomics a
    // reader is holding a reference to. The registry arrives as a span because
    // it dimensions nothing anvil compiles (docs/01-seams.md §6), so the size is
    // a runtime value and this is the one place that costs anything.
    std::unique_ptr<Slot[]>      cache_;

    std::atomic<bool>            listening_;
    std::thread                  listener_;
};

}  // namespace anvil::sections
