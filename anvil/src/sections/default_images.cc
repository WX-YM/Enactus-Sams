#include "anvil/sections/default_images.h"

#ifdef ANVIL_HAS_VIPS

#include <array>
#include <cerrno>
#include <cstddef>
#include <span>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <mongocxx/client_session.hpp>
#include <mongocxx/exception/exception.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/fs/paths.h"
#include "anvil/fs/upload.h"
#include "anvil/media/pipeline.h"
#include "anvil/media/record.h"

namespace anvil::sections {
namespace {

constexpr std::size_t kReadChunkBytes = 64 * 1024;

[[nodiscard]] Result<fs::UploadResult> ingest_file(fs::UploadSink& sink, int fd) {
    std::array<std::uint8_t, kReadChunkBytes> buffer{};
    while (true) {
        const ssize_t got = ::read(fd, buffer.data(), buffer.size());
        if (got < 0) {
            if (errno == EINTR) { continue; }
            return fail(ErrorCode::Internal);
        }
        if (got == 0) { break; }
        const Status written = sink.write(
            std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(got)});
        if (!written) { return written.error(); }
    }
    // Empty: there is no client header to compare against, and the sniffed type
    // is the only one that ever mattered.
    return sink.finish({});
}

// Adjusts a freshly created default's reference count once, so the collector can
// never reclaim it. A transaction for a single $inc looks heavy, but adjust_refs
// takes a session BY DESIGN — reference counts move inside the transaction of
// whatever owns the reference, and "the deployment owns this one" is still a
// transaction.
[[nodiscard]] Status pin_refs(mongocxx::client& client, media::MediaService& media, fs::Ns ns,
                              const Uuid& id) {
    try {
        auto session = client.start_session();
        Status outcome = ok();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            outcome = media.repository().adjust_refs(client, *txn, ns, id, media::kPinnedRefs);
        });
        return outcome;
    } catch (const mongocxx::exception&) {
        return fail(ErrorCode::ServiceUnavailable);
    }
}

}  // namespace

std::optional<Uuid> register_default_image(mongocxx::client& client, media::MediaService& media,
                                           fs::Ns ns, std::string_view defaults_dir,
                                           std::string_view file, const Uuid& actor) {
    std::string path;
    path.reserve(defaults_dir.size() + file.size() + 1);
    path.append(defaults_dir);
    path.push_back('/');
    path.append(file);

    // O_NOFOLLOW: the defaults tree is read-only and operator-installed, and a
    // symlink out of it would be a way to make this process publish an arbitrary
    // file as site content.
    const fs::Fd source{::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    if (!source.valid()) {
        LOG_ERROR << "default image missing: " << path
                  << " — the section will render without it";
        return std::nullopt;
    }

    // The same namespace the row will be recorded under, so a default image is
    // held to whatever types that namespace accepts. A shipped file is still a
    // file, and exempting it would make the one picture nobody re-checks the one
    // that can be any format at all.
    Result<fs::UploadSink> opened = fs::UploadSink::open(
        fs::Storage::instance(),
        fs::UploadLimits{.max_bytes = kMaxDefaultImageBytes, .free_floor_bytes = 0}, ns);
    if (!opened) {
        LOG_ERROR << "default image " << path << " could not be staged";
        return std::nullopt;
    }
    fs::UploadSink sink = std::move(opened).value();

    const Result<fs::UploadResult> ingested = ingest_file(sink, source.get());
    if (!ingested) {
        LOG_ERROR << "default image " << path << " was rejected by the media pipeline";
        return std::nullopt;
    }

    // Idempotence comes from the content hash, not from a marker: re-running
    // bootstrap against a populated database finds the identical bytes already
    // stored and reuses that row rather than storing a second copy.
    const Result<std::optional<media::MediaRecord>> duplicate =
        media.find_duplicate(client, ns, ingested.value().sha256);
    if (!duplicate) { return std::nullopt; }
    if (duplicate.value().has_value()) {
        sink.discard();
        return duplicate.value()->id;
    }

    const Result<media::ProcessedMedia> processed = media::process(ns, ingested.value());
    if (!processed) {
        LOG_ERROR << "default image " << path << " could not be processed";
        return std::nullopt;
    }
    // No address: this is the boot ingest, not a request, and there is nobody to
    // attribute it to.
    const Status recorded =
        media.record(client, ns, actor, processed.value(), ingested.value().sha256, std::nullopt);
    if (!recorded) { return std::nullopt; }
    if (const Status pinned = pin_refs(client, media, ns, processed.value().id); !pinned) {
        LOG_ERROR << "default image row could not be pinned; the collector may reclaim it";
    }
    return processed.value().id;
}

DefaultImageResolver default_image_resolver(mongocxx::client& client, media::MediaService& media,
                                            fs::Ns ns, std::string_view defaults_dir,
                                            const Uuid& actor) {
    return [&client, &media, ns, defaults_dir, actor](
               std::string_view file) -> std::optional<Uuid> {
        return register_default_image(client, media, ns, defaults_dir, file, actor);
    };
}

}  // namespace anvil::sections

#endif  // ANVIL_HAS_VIPS
