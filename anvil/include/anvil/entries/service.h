#pragma once

// Entries: create, edit, publish, place, list and remove the instances of a
// repeating kind (registry.h), with the section CMS's validation, media
// accounting and cross-instance invalidation.
//
// --- what this service owns and what it leaves to the application ------------
//
// It owns every INVARIANT: content bound and checked against the kind's shape,
// a slug that is well formed and unique, flags the kind declares, a parent that
// exists and is not full, a version on every edit, one media reference per
// distinct image an entry holds, and a notice to every instance when what
// readers see has changed.
//
// It owns no POLICY. Who may create a post, whether a member may edit only their
// own, whether an unpublished project is visible to a preview — those are
// permission decisions and they are the application's, made in its handlers
// against its own permission table. The one policy hook offered is WriteGuard,
// because "only if you wrote it" is a condition on the stored row that a handler
// could otherwise only check with a read before the write.
//
// --- caching -------------------------------------------------------------------
//
// None, and deliberately, unlike SectionService's three tiers. A section table is
// a few hundred kilobytes whatever happens, so holding all of it in every
// process is free; an entry kind can be a forum with a million replies. What an
// application needs is the SIGNAL — on_invalidated(kind), on the local write
// path and on every other instance through Redis — so that a cache it builds
// for its own reads (a rendered page, a snapshot of the pinned few) is refilled
// when it should be and never otherwise.
//
// --- threads -------------------------------------------------------------------
//
// Every method taking a client is BLOCKING and belongs on db_pool.
// invalidate_local() and publish_invalidation() are safe from any thread.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/entries/document.h"
#include "anvil/entries/registry.h"
#include "anvil/entries/repository.h"
#include "anvil/fs/namespace.h"
#include "anvil/media/service.h"
#include "anvil/sections/bootstrap.h"
#include "anvil/sections/content.h"

namespace anvil::entries {

struct EntryServiceConfig final {
    // The media namespace every image slot of every kind draws from. A media id
    // uploaded into another namespace is NotFound to an entry, exactly as it is
    // to a section.
    fs::Ns                                    image_namespace;
    // Prefixes the Redis channel, so two applications on one Redis do not hear
    // each other's kinds.
    std::string_view                          channel_prefix{"ent"};
    // Called once per invalidated kind with the REGISTRY's spelling of the key,
    // after the change is durable, on the writing instance and on every
    // subscriber. It MUST NOT BLOCK: on the subscriber's path it runs on the one
    // thread draining the channel. Post the work. Exceptions are caught and
    // logged.
    std::function<void(std::string_view kind)> on_invalidated;
};

struct NewEntry final {
    sections::SectionContent content;
    // Required when the kind's slug rule is Unique, refused when it is None.
    std::string              slug;
    // Required for a child kind, refused for a root kind.
    std::optional<Uuid>      parent;
    FlagSet                  flags{0};
};

struct EntryEdit final {
    // Merged onto the working copy, exactly as a section patch is merged onto
    // its stored content: a key the patch does not mention keeps its value.
    sections::SectionContent   patch;
    // A new slug, or nullopt to keep the current one.
    std::optional<std::string> slug;
};

// "Only if this user wrote it", enforced against the stored row. Default: no
// restriction. An entry's author never changes, so the check needs no lock —
// the row it is compared against cannot be rewritten under it.
struct WriteGuard final {
    std::optional<Uuid> author;
};

struct EntryWriteOutcome final {
    Uuid                 id;
    std::int64_t         version;
    // Of the copy the write changed: the draft for an Editorial edit, the
    // published copy for a publish or for any write to an Immediate kind.
    std::array<char, 18> etag;
};

struct EntryBootstrapReport final {
    std::size_t kinds_seeded;
    // Seeded by an earlier boot on some instance, and therefore left alone —
    // including a kind staff have since emptied, which stays empty.
    std::size_t kinds_already_seeded;
    std::size_t entries_created;
    std::size_t images_registered;
    std::size_t images_missing;
};

class EntryService final {
public:
    // `kinds` must outlive the service; it is a view of the application's
    // constexpr table and is never copied.
    EntryService(std::string database, std::string_view collection,
                 std::span<const KindSpec> kinds, media::MediaService& media,
                 EntryServiceConfig config);
    ~EntryService();

    // --- reads --------------------------------------------------------------------

    [[nodiscard]] Result<std::optional<EntryDocument>> find(mongocxx::client& client,
                                                            const KindSpec& kind, const Uuid& id,
                                                            Stage stage) const;
    [[nodiscard]] Result<std::optional<EntryDocument>> find_by_slug(mongocxx::client& client,
                                                                    const KindSpec& kind,
                                                                    std::string_view slug,
                                                                    Stage stage) const;
    // ValidationFailed ("parent") when the query's parent does not match the
    // kind: a child kind is listed per parent and a root kind has none.
    [[nodiscard]] Result<EntryPage> list(mongocxx::client& client, const KindSpec& kind,
                                         const EntryQuery& query) const;

    // --- writes -------------------------------------------------------------------
    //
    // Failures, in the vocabulary every other anvil write uses:
    //   ValidationFailed  a field, slug, flag or parent that the kind refuses,
    //                     named by the registry's spelling where there is one
    //   NotFound          no such entry under this kind, no such parent, an
    //                     image that is not in the namespace — or a WriteGuard
    //                     that does not match, which must read identically
    //   VersionMismatch   the caller's version is stale
    //   Conflict          the slug is taken, the scope or parent is full, the
    //                     entry still has children, or a reorder named a set of
    //                     entries the scope does not hold

    // Editorial: the entry exists only as a draft until publish(). Immediate: it
    // is live at once.
    [[nodiscard]] Result<EntryWriteOutcome> create(mongocxx::client& client, const KindSpec& kind,
                                                   const NewEntry& entry, const Uuid& actor);

    [[nodiscard]] Result<EntryWriteOutcome> write(mongocxx::client& client, const KindSpec& kind,
                                                  const Uuid& id, std::int64_t expected_version,
                                                  const EntryEdit& edit, const Uuid& actor,
                                                  const WriteGuard& guard = {});

    // Editorial kinds only: the draft becomes what readers see. The draft is
    // kept, and equal to the published copy until the next edit.
    [[nodiscard]] Result<EntryWriteOutcome> publish(mongocxx::client& client,
                                                    const KindSpec& kind, const Uuid& id,
                                                    std::int64_t expected_version,
                                                    const Uuid& actor);

    // Editorial kinds only: readers stop seeing it; the draft stays.
    [[nodiscard]] Result<EntryWriteOutcome> unpublish(mongocxx::client& client,
                                                      const KindSpec& kind, const Uuid& id,
                                                      std::int64_t expected_version,
                                                      const Uuid& actor);

    // Sets and clears named switches. `set & clear` must be empty. Unversioned:
    // see EntryRepository::set_flags.
    [[nodiscard]] Status set_flags(mongocxx::client& client, const KindSpec& kind,
                                   const Uuid& id, FlagSet set, FlagSet clear,
                                   const WriteGuard& guard = {});

    // Manual kinds only. `order` names every entry in the scope exactly once, in
    // the order wanted; anything else — a missing entry, an extra one, a
    // duplicate — is Conflict, because it means the caller is looking at a list
    // that has changed since it was read.
    [[nodiscard]] Status reorder(mongocxx::client& client, const KindSpec& kind,
                                 const std::optional<Uuid>& parent, std::span<const Uuid> order);

    // Refused with Conflict while the entry has children: deleting a thread
    // with replies is the application's decision to make one reply at a time,
    // or not at all.
    [[nodiscard]] Status remove(mongocxx::client& client, const KindSpec& kind, const Uuid& id,
                                std::int64_t expected_version, const WriteGuard& guard = {});

    // --- the fresh database -------------------------------------------------------

    // Seeds each kind in `seeds` ONCE, EVER, across every boot of every
    // instance. A per-kind claim is inserted in the same transaction as the
    // entries, so N instances booting together produce one seeding, and a kind
    // staff later empty is not refilled on the next deploy — which
    // insert-if-absent, the section rule, would do, because an entry that was
    // deleted and an entry that was never created look the same.
    //
    // Images are resolved first, through the same hook section bootstrap uses,
    // and a missing one is counted rather than fatal.
    [[nodiscard]] Result<EntryBootstrapReport> bootstrap(
        mongocxx::client& client, std::span<const KindSeeds> seeds,
        const sections::DefaultImageResolver& resolve, const Uuid& actor);

    // --- invalidation -------------------------------------------------------------

    // Calls on_invalidated for a declared kind; ignores anything else.
    void invalidate_local(std::string_view kind) noexcept;
    // invalidate_local here, then the same notice to every other instance.
    void publish_invalidation(std::string_view kind) noexcept;
    void start_invalidation_listener();
    void stop_invalidation_listener() noexcept;

    [[nodiscard]] std::span<const KindSpec> kinds() const noexcept { return kinds_; }
    [[nodiscard]] const EntryRepository& repository() const noexcept { return entries_; }

    EntryService(const EntryService&) = delete;
    EntryService& operator=(const EntryService&) = delete;

private:
    [[nodiscard]] Status check_images(mongocxx::client& client, const KindSpec& kind,
                                      const sections::SectionContent& content) const;
    [[nodiscard]] Result<EntryWriteOutcome> replace_stages(
        mongocxx::client& client, const KindSpec& kind, const EntryDocument& before,
        std::int64_t expected_version, const std::optional<EntryContent>& published,
        const std::optional<EntryContent>& draft, const std::optional<std::string>& slug,
        const EntryContent& changed);
    void listener_loop() noexcept;

    // Declaration order is construction order: entries_ is built from
    // database_ and kinds_, and channel_ from config_.
    const std::string         database_;
    std::span<const KindSpec> kinds_;
    media::MediaService&      media_;
    const EntryServiceConfig  config_;
    const std::string         channel_;
    EntryRepository           entries_;
    std::atomic<bool>         listening_;
    std::thread               listener_;
};

}  // namespace anvil::entries
