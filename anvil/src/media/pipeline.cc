#include "anvil/media/pipeline.h"

#ifdef ANVIL_HAS_VIPS

#include <fcntl.h>

#include <array>
#include <string_view>
#include <utility>

#include <unistd.h>

#include <cerrno>

#include "anvil/core/uuid.h"
#include "anvil/fs/paths.h"
#include "anvil/images/edit.h"
#include "anvil/images/probe.h"
#include "anvil/images/strip.h"
#include "anvil/images/variants.h"

namespace anvil::media {
namespace {

// The leaf name of a RelPath, NUL-terminated. Every syscall here stays relative
// to a boot-time directory descriptor: resolving a path to a string and opening
// it by name is the TOCTOU race the descriptor walk exists to close.
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

}  // namespace

Result<ProcessedMedia> process(fs::Ns ns, const fs::UploadResult& upload) {
    // A stored file or a sealed blob is never handed to a decoder. Refused
    // before anything opens it, so a caller that forgot to branch on the class
    // gets a failure rather than libvips guessing at a PDF or at ciphertext.
    if (fs::mime_class(upload.mime) != fs::MimeClass::Image) { return fail(ErrorCode::Internal); }
    const fs::Storage& storage = fs::Storage::instance();

    // The raw upload is still in tmp/ and is opened read-only for probing and
    // normalisation. The sink the caller still holds keeps ownership of the
    // write descriptor and of the file's removal.
    const LeafName raw_name{fs::temp_relative_path(upload.id)};
    const fs::Fd raw{
        ::openat(storage.tmp_fd(), raw_name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    if (!raw.valid()) { return fail(ErrorCode::Internal); }

    // HEADER ONLY: a decompression bomb is rejected here, before a decoder is
    // handed a single pixel. Deciding afterwards means the memory has already
    // been allocated, which is the whole attack.
    const Result<images::ImageInfo> probed = images::probe(raw.get(), upload.bytes);
    if (!probed) { return probed.error(); }

    // The stored master is the NORMALISED image — orientation applied to pixels,
    // sRGB, every metadata block dropped — so no downstream consumer can ever be
    // handed the original EXIF. Stripping without applying the orientation first
    // would silently rotate every photograph taken sideways.
    Result<fs::TempSlot> master_slot =
        fs::TempSlot::create(storage, upload.id, fs::kMasterVariant);
    if (!master_slot) { return master_slot.error(); }
    fs::TempSlot master = std::move(master_slot).value();

    const Result<images::ImageInfo> normalised =
        images::normalise_master(raw.get(), upload.mime, master.fd());
    if (!normalised) { return normalised.error(); }

    const std::uint64_t master_bytes = master.size();
    if (master_bytes == 0) { return fail(ErrorCode::Internal, images::kRejectEncode); }

    const Status master_published = master.publish(ns);
    if (!master_published) { return master_published.error(); }

    // Reopened through the descriptor walk every read uses. Variants are derived
    // from the MASTER and never from each other: chaining lossy re-encodes
    // through a smaller variant compounds artefacts.
    const fs::Fd master_file = storage.open_media(ns, upload.id, fs::kMasterVariant);
    if (!master_file.valid()) { return fail(ErrorCode::Internal); }

    Result<std::vector<images::VariantRecord>> variants = images::generate_variants(
        storage, ns, upload.id, master_file.get(), normalised.value());
    if (!variants) {
        // The master is already published, so a failure here leaves a file with
        // no row. Removing it now keeps the failure from costing the sweeper's
        // whole grace period of disk.
        unlink_all_files(storage, ns, upload.id, {});
        return variants.error();
    }

    return ProcessedMedia{
        .variants = std::move(variants).value(),
        .master_bytes = master_bytes,
        .id = upload.id,
        .width = normalised.value().width,
        .height = normalised.value().height,
        .mime = upload.mime,
    };
}

namespace {

// The derived master's bytes, hashed in one streaming pass. It is at most the
// widest rung, so this is a few megabytes read once, on the pool that just wrote
// them.
[[nodiscard]] Result<crypto::Digest256> hash_file(int fd) {
    crypto::Sha256Stream digest;
    std::array<std::uint8_t, fs::kStreamChunkBytes> buffer{};
    while (true) {
        const ssize_t got = ::read(fd, buffer.data(), buffer.size());
        if (got < 0) {
            if (errno == EINTR) { continue; }
            return fail(ErrorCode::Internal);
        }
        if (got == 0) { break; }
        digest.update(std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(got)});
    }
    return digest.finish();
}

}  // namespace

Result<RenderedMedia> render(fs::Ns ns, const PreparedEdit& edit) {
    const fs::Storage& storage = fs::Storage::instance();

    const fs::Fd source = storage.open_media(ns, edit.source, fs::kMasterVariant);
    if (!source.valid()) { return fail(ErrorCode::NotFound); }

    // A fresh id for every attempt, including a retry of the same edit. The
    // unique {ns, src, esha} index decides which attempt's row survives, and
    // the loser's files are under an id nothing else will ever name.
    const Uuid id = uuid::generate_v4();
    Result<images::RenderedEdit> rendered = images::render_edit(
        storage, ns, id, source.get(), edit.mime, edit.recipe, edit.plan);
    if (!rendered) {
        unlink_all_files(storage, ns, id, {});
        return rendered.error();
    }
    images::RenderedEdit output = std::move(rendered).value();

    const fs::Fd master = storage.open_media(ns, id, fs::kMasterVariant);
    Result<crypto::Digest256> sha256 =
        master.valid() ? hash_file(master.get()) : Result<crypto::Digest256>{fail(ErrorCode::Internal)};
    if (!sha256) {
        unlink_all_files(storage, ns, id, output.variants);
        return sha256.error();
    }

    return RenderedMedia{
        ProcessedMedia{
            .variants = std::move(output.variants),
            .master_bytes = output.master_bytes,
            .id = id,
            .width = output.width,
            .height = output.height,
            .mime = edit.mime,
        },
        sha256.value(),
    };
}

}  // namespace anvil::media

#endif  // ANVIL_HAS_VIPS
