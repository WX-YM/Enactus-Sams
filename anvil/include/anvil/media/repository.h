#pragma once

// The media collection. See anvil/media/record.h for the three rules every
// method here obeys.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/repository.h"
#include "anvil/media/record.h"

namespace anvil::media {

class MediaRepository final : public repo::RepositoryBase {
public:
    MediaRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    // Inserted only AFTER the file is in place. A row that precedes its file is
    // a 500 waiting for the first reader.
    [[nodiscard]] Status insert(mongocxx::client& client, const NewMedia& media) const;

    // nullopt when no row matches BOTH the id and the namespace. The caller maps
    // that to a stealth 404 and never distinguishes "wrong namespace" from "does
    // not exist" — the two must be indistinguishable to a client, or the pair of
    // them is an existence oracle over every namespace at once.
    [[nodiscard]] Result<std::optional<MediaRecord>> find(mongocxx::client& client, fs::Ns ns,
                                                          const Uuid& id) const;

    // Deduplication: identical bytes are stored once and shared. Scoped to a
    // namespace, because sharing a FILE across namespaces would let an upload
    // into one resolve through a handler for another.
    [[nodiscard]] Result<std::optional<MediaRecord>> find_by_hash(
        mongocxx::client& client, fs::Ns ns, const crypto::Digest256& sha256) const;

    // $inc by `delta`, inside the caller's transaction. `session` is the OWNING
    // document's session, which is what makes the count and the reference it
    // describes commit or abort together.
    [[nodiscard]] Status adjust_refs(mongocxx::client& client, mongocxx::client_session& session,
                                     fs::Ns ns, const Uuid& id, std::int32_t delta) const;

    // Claims the row and removes it, but ONLY while the count is still zero. One
    // atomic operation: a concurrent attach that lands first leaves the filter
    // matching nothing, and the caller learns it must not unlink. nullopt means
    // "not claimed", never "deleted anyway".
    [[nodiscard]] Result<std::optional<MediaRecord>> delete_if_unreferenced(
        mongocxx::client& client, fs::Ns ns, const Uuid& id) const;

    // The sweeper's first direction: claims ONE row that is unreferenced and
    // older than the grace period, and removes it in the same operation. Two
    // sweepers racing produce one deletion and one "nothing to do".
    [[nodiscard]] Result<std::optional<MediaRecord>> claim_unreferenced(
        mongocxx::client& client, db::TimeMs older_than) const;

    // A listing, newest first, ONE NAMESPACE per call.
    //
    // The scoping is the point rather than a filter: a cross-namespace listing
    // would be the one place in the system where a media id appears outside the
    // namespace that owns it, and the whole reason an id from one namespace does
    // not resolve through another's handler is that the namespace is in every
    // filter.
    [[nodiscard]] Result<std::vector<MediaRecord>> list_namespace(
        mongocxx::client& client, fs::Ns ns, const std::optional<MediaCursor>& after,
        std::int32_t limit) const;

    // The namespace's total bytes and row count, plus the `top_limit` addresses
    // that sent the most bytes into it.
    //
    // Two aggregations rather than one $facet: the totals cover every row in the
    // namespace, including uploads that carry no address, while the per-address
    // group covers only the rows that have one — which is exactly the partial
    // {ns, ip} index's range. A $facet computes both from one $match and would
    // therefore have to be the wider of the two.
    //
    // allowDiskUse is deliberately left OFF. A group whose keys exceed the
    // server's blocking-stage budget FAILS here rather than spilling to disk,
    // which is the correct outcome for a screen: an aggregation that takes a
    // minute and a gigabyte of scratch is not a screen, it is an incident.
    [[nodiscard]] Result<NamespaceUsage> usage(mongocxx::client& client, fs::Ns ns,
                                               std::int32_t top_limit) const;

    // Every row one address sent into one namespace, bounded. A purge's input
    // and an abuse screen's drill-down are the same query, so they are the same
    // method.
    [[nodiscard]] Result<std::vector<MediaRecord>> list_by_ip(
        mongocxx::client& client, fs::Ns ns, const std::array<std::uint8_t, 16>& ip,
        std::int32_t limit) const;

    // Every row, in `_id` order, for the sweeper's second direction: rows whose
    // file is missing. Paginated by cursor — never skip(n), which is O(n)
    // server-side (ENGINEERING_RULES.md §7).
    [[nodiscard]] Result<std::vector<MediaRecord>> scan_after(mongocxx::client& client,
                                                              const std::optional<Uuid>& after,
                                                              std::int32_t limit) const;

    // Sets the count to zero, but ONLY if it still reads `expected`. Answers
    // whether it matched.
    //
    // The expected count is in the FILTER rather than checked beforehand, for
    // the reason every read-modify-write in this system carries its version: a
    // sweeper decides a row is unreferenced by asking the owning collection, and
    // an attach landing between that question and this write would otherwise
    // have its brand-new reference zeroed — which is a live file collected
    // twenty-four hours later. Any concurrent attach or release moves the count,
    // so the mismatch is the interlock.
    [[nodiscard]] Result<bool> clear_refs_if(mongocxx::client& client, fs::Ns ns, const Uuid& id,
                                             std::int32_t expected) const;
};

}  // namespace anvil::media
