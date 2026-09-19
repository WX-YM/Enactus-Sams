#pragma once

// Media lifecycle: store, attach, release, delete.
//
// --- the two orderings that are correctness properties ----------------------
//
//   1. FILE FIRST, ROW SECOND on the way in. The row is inserted only after the
//      master has been renamed into place. A row that precedes its file is a 500
//      for whoever reads it first; a file that precedes its row is an orphan the
//      sweeper reaps.
//   2. ROW FIRST, FILE SECOND on the way out. The row is deleted, then the files
//      are unlinked. A crash between them leaves a sweepable orphan; the reverse
//      order leaves a row pointing at nothing.
//
// --- reference counting -----------------------------------------------------
//
// The count is adjusted inside the OWNING DOCUMENT'S transaction, never in a
// separate write. A count that commits while the document referencing it aborts
// is a leak no amount of sweeping can distinguish from a live reference.
// attach() and release() therefore take the caller's session rather than opening
// one.
//
// Deletion is never driven by a handler unlinking a file. It is driven by the
// count reaching zero and by the sweeper's grace period expiring.
//
// --- why the upload is three methods rather than one ------------------------
//
// The stages belong to different pools, and a single store() would have to pick
// one. Holding a mongocxx client from the bounded pool across a multi-second
// transcode starves every query in the process, and running libvips on a db_pool
// thread makes login latency depend on image sizes. The caller therefore drives:
//
//   cpu_pool : the sink's finish()   fsync and sniff
//   db_pool  : find_duplicate()      one indexed lookup
//   cpu_pool : media::process()      probe, normalise, publish, derive variants
//   db_pool  : record()              insert the row LAST
//
// Splitting it also puts the deduplication lookup BEFORE the expensive stage, so
// identical bytes cost one query instead of a full transcode.

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
#include "anvil/fs/namespace.h"
#include "anvil/fs/paths.h"
#include "anvil/fs/sniff.h"
#include "anvil/images/variants.h"
#include "anvil/media/record.h"
#include "anvil/media/repository.h"

namespace anvil::media {

// What the image stage produced. The row does not exist yet.
struct ProcessedMedia final {
    std::vector<images::VariantRecord> variants;
    std::uint64_t                      master_bytes;
    Uuid                               id;
    std::uint32_t                      width;
    std::uint32_t                      height;
    fs::Mime                           mime;
};

// How many rows ONE purge request takes. Bounded because the alternative is a
// pool thread held for as long as an attacker chose to make it: a flood is
// thousands of small files, and "delete all of them" with no bound is the
// attacker deciding how long a request runs.
//
// One request, one pass, no loop — so nothing is counted twice and nothing can
// spin. When a pass fills its bound the answer says so, and the operator asks
// again; the operation is idempotent by construction.
inline constexpr std::int32_t kPurgeBatchRows = 500;

// What one by-address purge did. `referenced` is the count it deliberately did
// NOT remove: an object something still points at is that document's record, and
// unlinking it would leave the owner pointing at nothing. It is REPORTED rather
// than silently skipped — an operator told "42 removed" has been told the wrong
// thing if eleven survived.
struct PurgeReport final {
    std::int64_t bytes;
    std::int64_t removed;
    std::int64_t referenced;
    // The pass filled its bound, so there may be more. Never a guess: it is set
    // only when the listing came back full.
    bool         more;
};

class MediaService final {
public:
    MediaService(std::string database, std::string_view collection);

    // Identical bytes already stored in this namespace, or nullopt. db_pool.
    //
    // Called BEFORE any image work. Identical bytes have already been probed,
    // normalised and transcoded once; doing it again is seconds of cpu_pool
    // spent to produce a file that already exists.
    [[nodiscard]] Result<std::optional<MediaRecord>> find_duplicate(
        mongocxx::client& client, fs::Ns ns, const crypto::Digest256& sha256) const;

    // Inserts the row, LAST, after every file is in place. On failure the files
    // are unlinked, so a failed insert leaves nothing rather than an orphan.
    // db_pool.
    //
    // `uploader_ip` is present only for an upload that had no account behind it,
    // and is OMITTED on disk otherwise. It cannot be derived from `owner`:
    // `owner` is a Uuid, so every account-less upload's is the same nothing, and
    // the address is the only handle abuse tooling has on who sent one.
    [[nodiscard]] Status record(mongocxx::client& client, fs::Ns ns, const Uuid& owner,
                                const ProcessedMedia& processed,
                                const crypto::Digest256& sha256,
                                const std::optional<std::array<std::uint8_t, 16>>&
                                    uploader_ip) const;

    // +1 and -1, inside the caller's transaction (see the header comment).
    [[nodiscard]] Status attach(mongocxx::client& client, mongocxx::client_session& session,
                                fs::Ns ns, const Uuid& id) const;
    [[nodiscard]] Status release(mongocxx::client& client, mongocxx::client_session& session,
                                 fs::Ns ns, const Uuid& id) const;

    [[nodiscard]] Result<std::optional<MediaRecord>> find(mongocxx::client& client, fs::Ns ns,
                                                          const Uuid& id) const;

    // One namespace, newest first, bounded.
    //
    // The reference count on each row is what makes such a listing safe rather
    // than merely pretty: media is reference-counted, so a picker can say "used
    // in 3 places" and whoever holds the delete authority can see, before
    // deleting, what removing this object blanks. Without it the listing is a
    // wall of thumbnails and deletion is a guess.
    [[nodiscard]] Result<std::vector<MediaRecord>> library(
        mongocxx::client& client, fs::Ns ns, const std::optional<MediaCursor>& after,
        std::int32_t limit) const;

    // Row first, then every file. Variants are enumerated from the row's own
    // list, never from a readdir glob — a glob is racy, slow, and will happily
    // delete a neighbour that shares a prefix.
    //
    // Returns NotFound when the row was already gone, which makes a repeated
    // delete safe rather than a 500: every job in this system is at-least-once.
    [[nodiscard]] Status purge(mongocxx::client& client, fs::Ns ns, const Uuid& id) const;

    // The namespace's weight and the addresses carrying it. db_pool, and never
    // on a public path: it is two aggregations, and the permission in front of
    // it is what keeps them an operator's action.
    [[nodiscard]] Result<NamespaceUsage> usage(mongocxx::client& client, fs::Ns ns,
                                               std::int32_t top_limit) const;

    // Every object one address sent into one namespace, ROWS BEFORE FILES, one
    // row at a time — the same ordering and the same atomic claim purge() makes,
    // because a bulk delete followed by a loop of unlinks would leave every file
    // of an interrupted purge unreferenced by anything that remembers it.
    //
    // A row that is still referenced is LEFT ALONE and counted. See PurgeReport.
    [[nodiscard]] Result<PurgeReport> purge_by_ip(mongocxx::client& client, fs::Ns ns,
                                                  const std::array<std::uint8_t, 16>& ip) const;

    // Deletes one unreferenced row and its files, and reports whether it found
    // anything. The claim and the delete are one atomic operation, so two
    // sweepers racing produce one deletion and one "nothing to do".
    [[nodiscard]] Result<bool> collect_one_unreferenced(mongocxx::client& client,
                                                        db::TimeMs older_than) const;

    [[nodiscard]] const MediaRepository& repository() const noexcept { return media_; }

    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;

private:
    // Declaration order is construction order: media_ is built from database_.
    const std::string database_;
    MediaRepository   media_;
};

// Removes the master and every variant. Best effort BY DESIGN: whatever survives
// is an orphan with no row, which is exactly what the sweeper collects — so
// retrying here would add a failure path to recover from a state that already
// has a recovery path.
void unlink_all_files(const fs::Storage& storage, fs::Ns ns, const Uuid& id,
                      const std::vector<images::VariantRecord>& variants) noexcept;

}  // namespace anvil::media
