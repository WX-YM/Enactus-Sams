#pragma once

// The upload pipeline (docs/07-filesystem.md §4–§5).
//
// The invariants, in the order they are enforced:
//
//   1. Free space is checked BEFORE a byte is accepted. A full disk discovered
//      halfway through an upload produces confusing failures everywhere else.
//   2. Bytes are counted as they arrive and the stream is aborted the moment it
//      passes the cap. Content-Length is an attacker-supplied hint and a chunked
//      body can exceed it, so the cap is enforced against bytes RECEIVED, never
//      against the header.
//   3. The body never lands in the heap. It goes chunk by chunk into
//      tmp/<uuid>.part, and the SHA-256 is computed in the same pass — hashing
//      the finished file would be a second full read of every byte.
//   4. The content type is sniffed from magic bytes. The claimed type is
//      compared against it and a disagreement is rejected and audited. In a
//      SEALED namespace nothing is sniffed: the client declares the SHA-256 of
//      its ciphertext and the digest from step 3 must equal it.
//   5. fsync the file, fsync the temp directory, THEN rename into the namespace
//      directory, then fsync that directory. Rename within one filesystem is
//      atomic, so a reader never observes a partial file; without the directory
//      syncs the rename can survive a crash while its contents do not.
//   6. The `media` row is inserted only after the rename succeeds. A row
//      pointing at a missing file is a 500 on the read path; a file with no row
//      is an orphan the sweeper reaps.
//
// --- Threading ---
//
// write() is a page-cache write and is the only method cheap enough to run on a
// Trantor loop thread; it is called from Drogon's stream reader, which has no
// backpressure mechanism, so buffering chunks for a worker would mean an
// unbounded queue. The two genuinely slow steps — finish(), which fsyncs, and
// publish(), which renames and fsyncs again — MUST be posted to a worker pool.
// See for the full argument.
//
// --- Lifetime ---
//
// An UploadSink that is destroyed without a successful publish() unlinks its
// .part file. A SIGKILL instead leaves the .part behind with no `media` row,
// which is exactly what the sweeper's one-hour tmp/ rule collects.
//
// A sink and a TempSlot BORROW the Storage they were opened from and must not
// outlive it. Storage is a boot-time singleton torn down after every pool has
// stopped, so the ordering holds by construction in main(); it is stated here
// because the failure mode is a use-after-free in a destructor, which is the
// hardest place to notice one.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/digest.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/paths.h"
#include "anvil/fs/sniff.h"

namespace anvil::fs {

// Failure::field values, so a caller can audit WHY an upload was refused
// without the reason ever reaching the client. Compile-time names, never
// submitted bytes.
inline constexpr std::string_view kRejectOversize = "upload.oversize";
inline constexpr std::string_view kRejectEmpty = "upload.empty";
inline constexpr std::string_view kRejectUnknownType = "upload.magic";
inline constexpr std::string_view kRejectVectorType = "upload.svg";
inline constexpr std::string_view kRejectTypeMismatch = "upload.claim_mismatch";
// A type the pipeline decodes, offered to a namespace that does not take it.
// Distinct from `upload.magic` on purpose: an unrecognised file is a probe or a
// mistake, and this is a legitimate image in the wrong place, which is the
// difference between an auditor looking for an attacker and an operator looking
// at a client that offered the wrong picker.
inline constexpr std::string_view kRejectNamespaceType = "upload.ns_type";
// A sealed upload whose bytes do not hash to what the client declared. Its own
// reason because it is neither a probe nor a wrong picker: it is a truncated
// or altered stream, or a client that lies about its own ciphertext.
inline constexpr std::string_view kRejectSealedHash = "upload.sealed_hash";

struct UploadLimits final {
    std::uint64_t max_bytes;
    std::uint64_t free_floor_bytes;
};

// What a completed stream produced. Small and trivially copyable, so it crosses
// a thread-pool boundary by value.
struct UploadResult final {
    crypto::Digest256 sha256;   // 32
    std::uint64_t     bytes;    //  8
    Uuid              id;       // 16
    Mime              mime;     //  1
};

// The streaming buffer size the transport should use. 64 KB is the peak-heap
// budget in docs/07-filesystem.md §9: one buffer per concurrent upload and nothing else.
inline constexpr std::size_t kStreamChunkBytes = 64 * 1024;

// Renames a file already written into tmp/ onto its final path, and fsyncs the
// destination directory. Shared by the upload sink and by every image output so
// that the atomicity argument lives in exactly one place.
[[nodiscard]] Status publish_temp_file(const Storage& storage, const RelPath& temp, Ns ns,
                                       const Uuid& id, VariantKey key) noexcept;

// Best-effort removal of a temp file. A failure means the sweeper collects it
// an hour later, which is the same outcome a SIGKILL produces.
void unlink_temp_file(const Storage& storage, const RelPath& temp) noexcept;

class UploadSink final {
public:
    // Refuses with InsufficientStorage when free space is below the floor, and
    // with Internal when the temp file cannot be created. Generates the media id
    // itself: v4, because the id appears in URLs and must be unguessable and
    // must not leak a creation time (docs/09-mongodb.md §2).
    //
    // `ns` is taken HERE rather than at publish() because the namespace decides
    // which types this upload may be, and a sink that learns its namespace only
    // once the bytes are on disk cannot refuse one. It is also the fact that used
    // to be passed twice — once to publish() and never to the validation — and a
    // fact passed twice is two things to keep in agreement.
    [[nodiscard]] static Result<UploadSink> open(const Storage& storage,
                                                 const UploadLimits& limits, Ns ns);

    ~UploadSink();
    UploadSink(UploadSink&&) noexcept;
    UploadSink& operator=(UploadSink&&) noexcept;
    UploadSink(const UploadSink&) = delete;
    UploadSink& operator=(const UploadSink&) = delete;

    // Appends to the temp file and to the running digest. Returns
    // PayloadTooLarge the first time the cap is passed and refuses everything
    // afterwards, so a caller that ignores one return value cannot keep writing.
    [[nodiscard]] Status write(std::span<const std::uint8_t> chunk) noexcept;

    // fsync, then sniff and validate against the allow-list AND against what
    // this sink's namespace accepts. `claimed_content_type` is the client's
    // header: it is compared, never believed. Blocking — post it to a pool.
    //
    // Refuses a SEALED namespace with Internal before touching anything: that
    // is a caller on the wrong path, not a client mistake, and sniffing
    // ciphertext would find a "type" one time in a few hundred.
    [[nodiscard]] Result<UploadResult> finish(std::string_view claimed_content_type);

    // The only way to finish an upload into a SEALED namespace (Ns::sealed()),
    // and refused with Internal on any other: two methods rather than a flag,
    // so a sealed namespace cannot be sniffed and an image namespace cannot
    // skip the sniff, whichever one the caller meant.
    //
    // fsync, then compare the digest the stream already computed with the
    // client's declared SHA-256 of its ciphertext. Nothing about the bytes is
    // inspected: no XML probe, no sniff, no claim. A mismatch is
    // ValidationFailed with kRejectSealedHash. The result's mime is
    // Mime::Sealed; hand it to media::store_sealed. The byte cap is the one
    // write() already enforced during the stream.
    //
    // What this does NOT bound is how many sealed bytes one account stores. A
    // namespace that cannot inspect content is otherwise free file hosting, so
    // a per-account BYTE budget belongs in front of every sealed upload — and
    // it lives in the chat upload route, which knows the account and can
    // refuse before the stream opens. anvil's sink knows neither
    // (docs/22-chat.md §6.4). Blocking — post it to a pool.
    [[nodiscard]] Result<UploadResult> finish_sealed(const crypto::Digest256& declared_sha256);

    // Creates the shard directories, renames the temp file into place and fsyncs
    // the destination directory, under the namespace this sink was opened for.
    // Refused with Internal unless a finish accepted the bytes.
    // Blocking — post it to a pool. After this the sink owns nothing and the
    // destructor unlinks nothing.
    [[nodiscard]] Status publish();

    // Drop the temp file deliberately: the dedup path has found the identical
    // bytes already stored, so this upload's copy is redundant (docs/07-filesystem.md §4 step
    // 6). Idempotent.
    void discard() noexcept;

    [[nodiscard]] const Uuid& id() const noexcept { return id_; }
    [[nodiscard]] std::uint64_t bytes_received() const noexcept { return bytes_; }
    [[nodiscard]] bool published() const noexcept { return published_; }

private:
    UploadSink(const Storage& storage, const UploadLimits& limits, Ns ns, const Uuid& id,
               Fd file);

    void unlink_temp() noexcept;

    // The steps both finishes share: the stream ended cleanly and is durable.
    [[nodiscard]] Status make_durable() noexcept;

    // Declaration order is construction order and also largest-first: the
    // digest owns a heap context, the counters are 8 bytes, the id is 16 raw
    // bytes, and the flags are last.
    const Storage*        storage_;
    UploadLimits          limits_;
    crypto::Sha256Stream  digest_;
    std::uint64_t         bytes_;
    Uuid                  id_;
    Fd                    file_;
    SniffBuffer           head_;
    std::uint8_t          head_size_;
    // One byte, and it sits with the flags rather than with limits_ because that
    // is where the largest-first ordering puts it (CLAUDE.md §2.3).
    Ns                    ns_;
    bool                  aborted_;
    bool                  finished_;
    // Set only by a finish that ACCEPTED the bytes. finished_ alone said the
    // stream was durable, which is also true of a stream finish refused, so a
    // caller that ignored the refusal could still publish it.
    bool                  accepted_;
    bool                  published_;
};

// A derived output being written into tmp/ before it is published by rename.
//
// Image work produces the normalised master and every variant through one of
// these, so a libvips failure mid-encode leaves a temp file the sweeper reaps
// rather than a zero-byte file in the namespace directory that would be served
// as a broken image (docs/08-images.md §5).
class TempSlot final {
public:
    // Opens tmp/<hex>[.variant].tmp for writing. O_EXCL, so a retry of a job
    // whose previous attempt is still on disk fails loudly rather than
    // interleaving two encoders' output into one file.
    [[nodiscard]] static Result<TempSlot> create(const Storage& storage, const Uuid& id,
                                                 VariantKey key);

    ~TempSlot();
    TempSlot(TempSlot&&) noexcept;
    TempSlot& operator=(TempSlot&&) noexcept;
    TempSlot(const TempSlot&) = delete;
    TempSlot& operator=(const TempSlot&) = delete;

    // Borrowed, never closed by the caller: the slot owns it.
    [[nodiscard]] int fd() const noexcept { return file_.get(); }

    // Bytes currently in the file. A zero-length output is a failed encode, and
    // publish() refuses it.
    [[nodiscard]] std::uint64_t size() const noexcept;

    // fsync, rename into the namespace shard, fsync the destination directory.
    // Blocking — this runs on cpu_pool with the rest of the image work.
    [[nodiscard]] Status publish(Ns ns);

    void discard() noexcept;

private:
    TempSlot(const Storage& storage, const Uuid& id, VariantKey key, Fd file);

    const Storage* storage_;
    Uuid           id_;
    Fd             file_;
    VariantKey     key_;
    bool           published_;
};

}  // namespace anvil::fs
