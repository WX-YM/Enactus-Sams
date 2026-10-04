#include "anvil/media/service.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <exception>
#include <string_view>
#include <utility>

#include <mongocxx/exception/exception.hpp>

#include "anvil/crypto/digest.h"
#include "anvil/images/edit.h"

namespace anvil::media {
namespace {

// The leaf name of a RelPath, NUL-terminated, so every syscall stays relative to
// a boot-time directory descriptor rather than resolving a path by name — which
// is the TOCTOU window the descriptor walk exists to close (anvil/fs/paths.h).
struct LeafName final {
    std::array<char, 48> chars;

    explicit LeafName(const fs::RelPath& path) noexcept : chars{} {
        const std::string_view view = path.view();
        const std::size_t slash = view.rfind('/');
        const std::string_view leaf =
            slash == std::string_view::npos ? view : view.substr(slash + 1);
        const std::size_t take = leaf.size() < chars.size() - 1 ? leaf.size() : chars.size() - 1;
        for (std::size_t i = 0; i < take; ++i) { chars[i] = leaf[i]; }
        chars[take] = '\0';
    }

    [[nodiscard]] const char* c_str() const noexcept { return chars.data(); }
};

// Removes one file of an object, relative to its shard descriptor. Best effort:
// see unlink_all_files.
void unlink_one(const fs::Storage& storage, fs::Ns ns, const Uuid& id,
                fs::VariantKey key) noexcept {
    const Result<fs::Fd> shard = storage.open_shard_for_write(ns, id);
    if (!shard) { return; }
    const LeafName name{fs::media_relative_path(ns, id, key)};
    (void)::unlinkat(shard.value().get(), name.c_str(), 0);
}

struct AbortTransaction final : std::exception {
    explicit AbortTransaction(Failure f) noexcept : failure{f} {}
    Failure failure;
};

}  // namespace

// Deliberately NOT images::unlink_variants, even though that function does the
// same thing and needs nothing from libvips.
//
// It lives in a translation unit this build compiles only when the image
// subsystem is enabled, and the media ROW lifecycle must not depend on whether
// this build can encode anything: a deployment serving already-stored objects
// with no encoder present still has to be able to delete one. The naming is not
// duplicated — fs::media_relative_path is the single source of it on both paths,
// which is the part that would actually be dangerous to have twice.
void unlink_all_files(const fs::Storage& storage, fs::Ns ns, const Uuid& id,
                      const std::vector<images::VariantRecord>& variants) noexcept {
    for (const images::VariantRecord& variant : variants) {
        unlink_one(storage, ns, id, fs::VariantKey{variant.width, variant.format});
    }
    unlink_one(storage, ns, id, fs::kMasterVariant);
}

namespace {

// Publish the bytes that arrived as the master, and describe nothing about
// them. The class check is each caller's, so that neither entry point accepts
// the other's input.
[[nodiscard]] Result<ProcessedMedia> publish_as_sent(fs::UploadSink& sink,
                                                     const fs::UploadResult& upload) {
    const Status published = sink.publish();
    if (!published) { return published.error(); }
    return ProcessedMedia{
        .variants = {},
        .master_bytes = upload.bytes,
        .id = upload.id,
        .width = 0,
        .height = 0,
        .mime = upload.mime,
    };
}

}  // namespace

Result<ProcessedMedia> store_file(fs::UploadSink& sink, const fs::UploadResult& upload) {
    if (fs::mime_class(upload.mime) != fs::MimeClass::File) { return fail(ErrorCode::Internal); }
    return publish_as_sent(sink, upload);
}

Result<ProcessedMedia> store_sealed(fs::UploadSink& sink, const fs::UploadResult& upload) {
    if (fs::mime_class(upload.mime) != fs::MimeClass::Sealed) {
        return fail(ErrorCode::Internal);
    }
    return publish_as_sent(sink, upload);
}

MediaService::MediaService(std::string database, std::string_view collection)
    : database_{std::move(database)}, media_{database_, collection} {}

Result<std::optional<MediaRecord>> MediaService::find_duplicate(
    mongocxx::client& client, fs::Ns ns, const Uuid& owner,
    const crypto::Digest256& sha256) const {
    return media_.find_by_hash(client, ns, owner, sha256);
}

Status MediaService::record(mongocxx::client& client, fs::Ns ns, const Uuid& owner,
                            const ProcessedMedia& processed, const crypto::Digest256& sha256,
                            const std::optional<std::array<std::uint8_t, 16>>& uploader_ip)
    const {
    // The row goes in LAST. Everything before it can crash and leave nothing but
    // a file with no row, which the sweeper collects; a row inserted first and
    // then abandoned is a permanent 500 on the read path.
    const Status inserted = media_.insert(client, NewMedia{
                                                      .variants = processed.variants,
                                                      .sha256 = sha256,
                                                      .bytes = processed.master_bytes,
                                                      .id = processed.id,
                                                      .owner = owner,
                                                      .uploader_ip = uploader_ip,
                                                      .width = processed.width,
                                                      .height = processed.height,
                                                      .ns = ns,
                                                      .mime = processed.mime,
                                                  });
    if (!inserted) {
        // No row will ever point at these files, and the sweeper's grace period
        // would eventually collect them anyway; removing them now keeps the
        // failure from costing a day of disk.
        unlink_all_files(fs::Storage::instance(), ns, processed.id, processed.variants);
        return inserted.error();
    }
    return ok();
}

Status MediaService::attach(mongocxx::client& client, mongocxx::client_session& session,
                            fs::Ns ns, const Uuid& id) const {
    return media_.adjust_refs(client, session, ns, id, 1);
}

Status MediaService::release(mongocxx::client& client, mongocxx::client_session& session,
                             fs::Ns ns, const Uuid& id) const {
    return media_.adjust_refs(client, session, ns, id, -1);
}

Result<std::optional<MediaRecord>> MediaService::find(mongocxx::client& client, fs::Ns ns,
                                                      const Uuid& id) const {
    return media_.find(client, ns, id);
}

Result<std::variant<EditedMedia, PreparedEdit>> MediaService::prepare_edit(
    mongocxx::client& client, fs::Ns ns, const Uuid& source,
    std::span<const std::uint8_t> recipe, bool detach) const {
    // Decoded before any lookup: a malformed recipe costs a parse, never a
    // round trip.
    Result<images::Recipe> decoded = images::decode_recipe(recipe, images::kEditLimits);
    if (!decoded) { return decoded.error(); }

    const Result<std::optional<MediaRecord>> found = media_.find(client, ns, source);
    if (!found) { return found.error(); }
    if (!found.value().has_value()) { return fail(ErrorCode::NotFound); }
    const MediaRecord& row = *found.value();
    if (row.source.has_value()) {
        return fail(ErrorCode::ValidationFailed, images::kFaultNotSource);
    }
    // A stored file or a sealed blob has no pixels this process wrote and no
    // size to plan against. Refused here rather than left to the planner, whose answer for a
    // zero-sized frame would name a crop fault for a document nobody cropped.
    if (fs::mime_class(row.mime) != fs::MimeClass::Image) {
        return fail(ErrorCode::UnsupportedMedia);
    }

    const Result<images::EditPlan> plan = images::plan_edit(
        decoded.value(), images::ImageInfo{row.width, row.height, 1}, images::kEditLimits);
    if (!plan) { return plan.error(); }

    const crypto::Digest256 edit_sha = crypto::sha256(recipe);
    if (!detach) {
        const Result<std::optional<MediaRecord>> existing =
            media_.find_edit(client, ns, source, edit_sha);
        if (!existing) { return existing.error(); }
        if (existing.value().has_value()) {
            const MediaRecord& edit = *existing.value();
            return std::variant<EditedMedia, PreparedEdit>{
                EditedMedia{edit.id, edit.width, edit.height, false}};
        }
    }

    return std::variant<EditedMedia, PreparedEdit>{PreparedEdit{
        .recipe = std::move(decoded).value(),
        .canonical = std::vector<std::uint8_t>(recipe.begin(), recipe.end()),
        .edit_sha = edit_sha,
        .plan = plan.value(),
        .source = source,
        .mime = row.mime,
        .detach = detach,
    }};
}

Result<EditedMedia> MediaService::record_edit(mongocxx::client& client, fs::Ns ns,
                                              const Uuid& owner, const PreparedEdit& edit,
                                              const ProcessedMedia& rendered,
                                              const crypto::Digest256& sha256) const {
    NewMedia row{
        .variants = rendered.variants,
        .sha256 = sha256,
        .bytes = rendered.master_bytes,
        .id = rendered.id,
        .owner = owner,
        .uploader_ip = std::nullopt,
        .width = rendered.width,
        .height = rendered.height,
        .ns = ns,
        .mime = rendered.mime,
    };
    if (!edit.detach) {
        row.edit = edit.canonical;
        row.source = edit.source;
        row.edit_sha = edit.edit_sha;
    }

    Status inserted = ok();
    try {
        auto session = client.start_session();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            const Status written = media_.insert_edit(client, *txn, row);
            if (!written) { throw AbortTransaction{written.error()}; }
        });
    } catch (const AbortTransaction& aborted) {
        inserted = aborted.failure;
    } catch (const mongocxx::exception&) {
        inserted = fail(ErrorCode::ServiceUnavailable);
    }

    if (inserted) { return EditedMedia{rendered.id, rendered.width, rendered.height, true}; }

    // Whatever happened, no row points at these files: they are this attempt's,
    // under the id it minted, and nothing else can reference them.
    unlink_all_files(fs::Storage::instance(), ns, rendered.id, rendered.variants);

    if (inserted.error().code == ErrorCode::Conflict && !edit.detach) {
        // An identical edit of the same source committed between this attempt's
        // lookup and its insert. That one is the answer.
        const Result<std::optional<MediaRecord>> winner =
            media_.find_edit(client, ns, edit.source, edit.edit_sha);
        if (!winner) { return winner.error(); }
        if (winner.value().has_value()) {
            const MediaRecord& found = *winner.value();
            return EditedMedia{found.id, found.width, found.height, false};
        }
    }
    return inserted.error();
}

Result<std::vector<MediaRecord>> MediaService::library(mongocxx::client& client, fs::Ns ns,
                                                       const std::optional<MediaCursor>& after,
                                                       std::int32_t limit) const {
    return media_.list_namespace(client, ns, after, limit);
}

Status MediaService::purge(mongocxx::client& client, fs::Ns ns, const Uuid& id) const {
    // The row is claimed and removed in ONE operation, conditional on the count
    // still being zero. A separate read followed by a delete would let an attach
    // land in between, and the files behind a live reference would be unlinked.
    const Result<std::optional<MediaRecord>> claimed =
        media_.delete_if_unreferenced(client, ns, id);
    if (!claimed) { return claimed.error(); }
    if (!claimed.value().has_value()) {
        // Either the row was already gone — a repeated purge, which every job in
        // this system must tolerate — or it is still referenced. Neither is a
        // reason to touch a file.
        return fail(ErrorCode::NotFound);
    }

    const MediaRecord& record = *claimed.value();
    unlink_all_files(fs::Storage::instance(), ns, id, record.variants);
    return ok();
}

Result<NamespaceUsage> MediaService::usage(mongocxx::client& client, fs::Ns ns,
                                           std::int32_t top_limit) const {
    return media_.usage(client, ns, top_limit);
}

Result<PurgeReport> MediaService::purge_by_ip(mongocxx::client& client, fs::Ns ns,
                                              const std::array<std::uint8_t, 16>& ip) const {
    const Result<std::vector<MediaRecord>> page =
        media_.list_by_ip(client, ns, ip, kPurgeBatchRows);
    if (!page) { return page.error(); }

    PurgeReport report{};
    report.more = page.value().size() == static_cast<std::size_t>(kPurgeBatchRows);
    for (const MediaRecord& row : page.value()) {
        // Read off the row rather than discovered by a refused delete, so an
        // object something still points at costs no round trip and is reported
        // as what it is. The claim below re-checks it anyway: this is the cheap
        // answer, not the authoritative one.
        if (row.refs > 0) {
            ++report.referenced;
            continue;
        }
        // Row first, then the files, one row at a time — the same atomic claim a
        // single delete makes. A bulk delete followed by a loop of unlinks would
        // leave an interrupted purge with files nothing remembers.
        const Status purged = purge(client, ns, row.id);
        if (!purged) {
            // NotFound covers "already gone" and "attached between the listing
            // and now" alike. Neither is a failure of this request.
            if (purged.error().code != ErrorCode::NotFound) { return purged.error(); }
            ++report.referenced;
            continue;
        }
        report.bytes += static_cast<std::int64_t>(row.bytes);
        ++report.removed;
    }
    return report;
}

Result<bool> MediaService::collect_one_unreferenced(mongocxx::client& client,
                                                    db::TimeMs older_than) const {
    const Result<std::optional<MediaRecord>> claimed =
        media_.claim_unreferenced(client, older_than);
    if (!claimed) { return claimed.error(); }
    if (!claimed.value().has_value()) { return false; }

    const MediaRecord& record = *claimed.value();
    unlink_all_files(fs::Storage::instance(), record.ns, record.id, record.variants);
    return true;
}

}  // namespace anvil::media
