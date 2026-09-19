#include "anvil/fs/upload.h"

#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "anvil/core/uuid.h"

namespace anvil::fs {
namespace {

// write() may return short, and it may return EINTR before writing anything.
// Treating either as success is a silently truncated file.
[[nodiscard]] bool write_all(int fd, std::span<const std::uint8_t> data) noexcept {
    std::size_t written = 0;
    while (written < data.size()) {
        const ::ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            return false;
        }
        if (n == 0) { return false; }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

[[nodiscard]] bool fsync_retry(int fd) noexcept {
    while (::fsync(fd) != 0) {
        if (errno == EINTR) { continue; }
        return false;
    }
    return true;
}

// The final component of a RelPath, NUL-terminated and on the stack. Every
// operation here is relative to a boot-time directory descriptor, so the
// directory part of the path is never handed to a syscall — which is what makes
// the descriptors, rather than the string, the thing being trusted.
struct LeafName final {
    std::array<char, 48> chars;

    explicit LeafName(const RelPath& path) noexcept : chars{} {
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

UploadSink::UploadSink(const Storage& storage, const UploadLimits& limits, Ns ns,
                       const Uuid& id, Fd file)
    : storage_{&storage},
      limits_{limits},
      digest_{},
      bytes_{0},
      id_{id},
      file_{std::move(file)},
      head_{},
      head_size_{0},
      ns_{ns},
      aborted_{false},
      finished_{false},
      published_{false} {}

// Written out rather than defaulted: a defaulted move leaves the source still
// holding the storage pointer and the id, so ITS destructor would unlink the
// .part file the destination now owns. Clearing the source is the whole point.
UploadSink::UploadSink(UploadSink&& other) noexcept
    : storage_{other.storage_},
      limits_{other.limits_},
      digest_{std::move(other.digest_)},
      bytes_{other.bytes_},
      id_{other.id_},
      file_{std::move(other.file_)},
      head_{other.head_},
      head_size_{other.head_size_},
      ns_{other.ns_},
      aborted_{other.aborted_},
      finished_{other.finished_},
      published_{other.published_} {
    other.storage_ = nullptr;
    other.published_ = true;
}

UploadSink& UploadSink::operator=(UploadSink&& other) noexcept {
    if (this != &other) {
        discard();
        storage_ = other.storage_;
        limits_ = other.limits_;
        digest_ = std::move(other.digest_);
        bytes_ = other.bytes_;
        id_ = other.id_;
        file_ = std::move(other.file_);
        head_ = other.head_;
        head_size_ = other.head_size_;
        ns_ = other.ns_;
        aborted_ = other.aborted_;
        finished_ = other.finished_;
        published_ = other.published_;
        other.storage_ = nullptr;
        other.published_ = true;
    }
    return *this;
}

UploadSink::~UploadSink() { discard(); }

Result<UploadSink> UploadSink::open(const Storage& storage, const UploadLimits& limits,
                                   Ns ns) {
    // Before a byte is accepted, not after. The temp-and-rename pattern means a
    // full disk corrupts nothing, but it produces failures that look like
    // everything except what they are (docs/07-filesystem.md §4).
    const StorageStats stats = storage.stats();
    if (stats.free_bytes < limits.free_floor_bytes) {
        return fail(ErrorCode::InsufficientStorage);
    }

    // v4, not v7: this id appears in URLs, so it must be unguessable and must
    // not encode when it was created.
    const Uuid id = uuid::generate_v4();
    const RelPath temp = temp_relative_path(id);
    const LeafName name{temp};

    // O_EXCL: never write into a file that already exists, whatever put it
    // there. O_NOFOLLOW: never follow a symlink planted in tmp/.
    Fd file{::openat(storage.tmp_fd(), name.c_str(),
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kStorageFileMode)};
    if (!file.valid()) { return fail(ErrorCode::Internal); }

    return Result<UploadSink>{UploadSink{storage, limits, ns, id, std::move(file)}};
}

Status UploadSink::write(std::span<const std::uint8_t> chunk) noexcept {
    if (aborted_) { return fail(ErrorCode::PayloadTooLarge, kRejectOversize); }
    if (finished_ || !file_.valid()) { return fail(ErrorCode::Internal); }
    if (chunk.empty()) { return ok(); }

    // Checked against bytes RECEIVED. A chunked body that contradicts its own
    // Content-Length dies here, at the first chunk that crosses the cap, having
    // written no more than the cap to disk.
    if (chunk.size() > limits_.max_bytes - bytes_) {
        aborted_ = true;
        return fail(ErrorCode::PayloadTooLarge, kRejectOversize);
    }

    // The first bytes are kept for sniffing. Copied out here because the stream
    // buffer is reused by the transport for the next chunk.
    if (head_size_ < head_.size()) {
        const std::size_t room = head_.size() - head_size_;
        const std::size_t take = chunk.size() < room ? chunk.size() : room;
        for (std::size_t i = 0; i < take; ++i) { head_[head_size_ + i] = chunk[i]; }
        head_size_ = static_cast<std::uint8_t>(head_size_ + take);
    }

    if (!write_all(file_.get(), chunk)) {
        aborted_ = true;
        return fail(ErrorCode::Internal);
    }

    try {
        digest_.update(chunk);
    } catch (...) {
        // A broken OpenSSL context is not something a request can recover from,
        // but it must not escape into a Trantor loop thread either.
        aborted_ = true;
        return fail(ErrorCode::Internal);
    }

    bytes_ += chunk.size();
    return ok();
}

Result<UploadResult> UploadSink::finish(std::string_view claimed_content_type) {
    if (aborted_) { return fail(ErrorCode::PayloadTooLarge, kRejectOversize); }
    if (finished_ || !file_.valid()) { return fail(ErrorCode::Internal); }
    if (bytes_ == 0) { return fail(ErrorCode::ValidationFailed, kRejectEmpty); }

    // Durability first, validation second: the file must be on disk before the
    // rename that publishes it, and the fsync is the slowest thing in this
    // function whether or not the validation is going to reject it.
    if (!fsync_retry(file_.get())) { return fail(ErrorCode::Internal); }
    if (!fsync_retry(storage_->tmp_fd())) { return fail(ErrorCode::Internal); }
    finished_ = true;

    const std::span<const std::uint8_t> head{head_.data(), head_size_};

    // Checked before sniff() so the rejection carries the reason an auditor
    // needs. An SVG upload is a probe, not a mistake (docs/07-filesystem.md §5).
    if (looks_like_xml(head)) {
        return fail(ErrorCode::UnsupportedMedia, kRejectVectorType);
    }

    const Mime sniffed = sniff(head);
    if (sniffed == Mime::Unknown) {
        return fail(ErrorCode::UnsupportedMedia, kRejectUnknownType);
    }

    // The NAMESPACE's list, not the global one. Checked here — before the claim
    // comparison and long before publish() — because the answer is already
    // knowable and the alternative is a file accepted, decoded and only then
    // found to be in the wrong place.
    //
    // Read from the same table the descriptor emits `accepts` from, so a client
    // offering a picker and this refusing one cannot disagree.
    if (!mime_accepted(ns_.accepts(), sniffed)) {
        return fail(ErrorCode::UnsupportedMedia, kRejectNamespaceType);
    }

    // The claim is compared, never believed. An absent or unparseable header is
    // not a disagreement — plenty of legitimate clients send neither — but a
    // header that names a DIFFERENT supported type is a probe.
    const Mime claimed = mime_from_claim(claimed_content_type);
    if (claimed != Mime::Unknown && claimed != sniffed) {
        return fail(ErrorCode::UnsupportedMedia, kRejectTypeMismatch);
    }

    crypto::Digest256 digest{};
    try {
        digest = digest_.finish();
    } catch (...) {
        return fail(ErrorCode::Internal);
    }

    return UploadResult{digest, bytes_, id_, sniffed};
}

Status UploadSink::publish() {
    if (!finished_ || published_ || !file_.valid()) { return fail(ErrorCode::Internal); }

    const Status renamed =
        publish_temp_file(*storage_, temp_relative_path(id_), ns_, id_, kMasterVariant);
    if (!renamed) { return renamed; }

    published_ = true;
    file_.reset();
    return ok();
}

void UploadSink::discard() noexcept {
    if (!published_) { unlink_temp(); }
    file_.reset();
}

void UploadSink::unlink_temp() noexcept {
    if (storage_ == nullptr) { return; }
    unlink_temp_file(*storage_, temp_relative_path(id_));
}

// --- TempSlot --------------------------------------------------------------

TempSlot::TempSlot(const Storage& storage, const Uuid& id, VariantKey key, Fd file)
    : storage_{&storage}, id_{id}, file_{std::move(file)}, key_{key}, published_{false} {}

TempSlot::TempSlot(TempSlot&& other) noexcept
    : storage_{other.storage_},
      id_{other.id_},
      file_{std::move(other.file_)},
      key_{other.key_},
      published_{other.published_} {
    other.storage_ = nullptr;
    other.published_ = true;
}

TempSlot& TempSlot::operator=(TempSlot&& other) noexcept {
    if (this != &other) {
        discard();
        storage_ = other.storage_;
        id_ = other.id_;
        file_ = std::move(other.file_);
        key_ = other.key_;
        published_ = other.published_;
        other.storage_ = nullptr;
        other.published_ = true;
    }
    return *this;
}

TempSlot::~TempSlot() { discard(); }

Result<TempSlot> TempSlot::create(const Storage& storage, const Uuid& id, VariantKey key) {
    if (!is_known_variant(key)) { return fail(ErrorCode::Internal); }

    const RelPath temp = derived_temp_path(id, key);
    const LeafName name{temp};
    Fd file{::openat(storage.tmp_fd(), name.c_str(),
                     O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kStorageFileMode)};
    if (!file.valid()) { return fail(ErrorCode::Internal); }
    return Result<TempSlot>{TempSlot{storage, id, key, std::move(file)}};
}

std::uint64_t TempSlot::size() const noexcept {
    struct ::stat info{};
    if (!file_.valid() || ::fstat(file_.get(), &info) != 0) { return 0; }
    return static_cast<std::uint64_t>(info.st_size);
}

Status TempSlot::publish(Ns ns) {
    if (published_ || !file_.valid() || storage_ == nullptr) { return fail(ErrorCode::Internal); }
    // An encoder that failed leaves a zero-byte file behind. Publishing one
    // would put a permanently broken image in front of every client that asks
    // for that variant, for as long as the file stays on disk.
    if (size() == 0) { return fail(ErrorCode::Internal); }

    const Status renamed =
        publish_temp_file(*storage_, derived_temp_path(id_, key_), ns, id_, key_);
    if (!renamed) { return renamed; }

    published_ = true;
    file_.reset();
    return ok();
}

void TempSlot::discard() noexcept {
    if (!published_ && storage_ != nullptr) {
        unlink_temp_file(*storage_, derived_temp_path(id_, key_));
    }
    file_.reset();
}

// --- shared publish path ---------------------------------------------------

Status publish_temp_file(const Storage& storage, const RelPath& temp, Ns ns, const Uuid& id,
                         VariantKey key) noexcept {
    const Result<Fd> shard = storage.open_shard_for_write(ns, id);
    if (!shard) { return shard.error(); }

    const LeafName source{temp};
    const LeafName target{media_relative_path(ns, id, key)};

    // Atomic within one filesystem, which Storage asserted at boot: a reader
    // sees either no file or the whole file, never a partial one.
    if (::renameat(storage.tmp_fd(), source.c_str(), shard.value().get(), target.c_str()) != 0) {
        return fail(ErrorCode::Internal);
    }

    // Without this, a crash can leave the rename unrecorded while the temp entry
    // is already gone — the file would then exist in no directory at all.
    if (!fsync_retry(shard.value().get())) { return fail(ErrorCode::Internal); }
    return ok();
}

void unlink_temp_file(const Storage& storage, const RelPath& temp) noexcept {
    const LeafName name{temp};
    // ENOENT is the expected outcome for a moved-from owner and for a second
    // discard(); there is nothing here to report.
    (void)::unlinkat(storage.tmp_fd(), name.c_str(), 0);
}

}  // namespace anvil::fs
