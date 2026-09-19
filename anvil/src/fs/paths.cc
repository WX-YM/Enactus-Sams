#include "anvil/fs/paths.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "anvil/config/paths.h"

namespace anvil::fs {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::string_view kNsRoot = "ns";
constexpr std::string_view kTmpDir = "tmp";
constexpr std::string_view kPartSuffix = ".part";

// rwx for the owner, r-x for the group, NOTHING for anyone else. Media is served
// by Nginx through X-Accel-Redirect, and the deployment convention this mode
// assumes is that Nginx runs as a member of the application's group, rather than
// that the directory modes are widened to 0700.
//
// This was 0700, and under 0700 that convention cannot work — a mode granting
// the group nothing makes group membership irrelevant, so every private image
// answered 403 from Nginx after the application had authorised it. The 403 comes
// from the file open, not the redirect, so `internal;` and the authorisation both
// look correct while every image on the site is broken.
//
// The security property is unchanged and is carried by the LAST digit: nothing
// world-readable, so "keep the storage private" still does not depend on the URL
// space alone. Group-readable is what the design asked for all along.


std::unique_ptr<Storage> g_storage;

[[nodiscard]] Fd open_directory_at(int parent, const char* name) noexcept {
    return Fd{::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
}

// mkdirat that treats an existing directory as success. Two uploads racing to
// create the same shard is the normal case, not an error.
[[nodiscard]] bool make_directory_at(int parent, const char* name) noexcept {
    if (::mkdirat(parent, name, kStorageDirMode) == 0) { return true; }
    return errno == EEXIST;
}

// One shard component: the lowercase hex of a single byte, NUL-terminated.
struct ShardName final {
    std::array<char, 3> chars;

    explicit ShardName(std::uint8_t byte) noexcept : chars{} {
        chars[0] = kHexDigits[byte >> 4U];
        chars[1] = kHexDigits[byte & 0x0FU];
        chars[2] = '\0';
    }

    [[nodiscard]] const char* c_str() const noexcept { return chars.data(); }
};

// The 32-hex-character basename, NUL-terminated. On the stack, so opening a
// media file allocates nothing at all.
struct MediaName final {
    std::array<char, 33> chars;

    explicit MediaName(const Uuid& id) noexcept : chars{} {
        std::size_t out = 0;
        for (const std::uint8_t byte : id) {
            chars[out++] = kHexDigits[byte >> 4U];
            chars[out++] = kHexDigits[byte & 0x0FU];
        }
        chars[out] = '\0';
    }

    [[nodiscard]] const char* c_str() const noexcept { return chars.data(); }
};

}  // namespace

// --- RelPath ---------------------------------------------------------------

void RelPath::append(std::string_view text) noexcept {
    // The capacity is computed from the grammar, so a truncation here means the
    // grammar changed without the constant changing. Clamping keeps this
    // noexcept and allocation-free; the static_assert on the capacity and the
    // path tests are what stop it from ever mattering.
    const std::size_t room = chars_.size() - 1 - size_;
    const std::size_t take = text.size() < room ? text.size() : room;
    for (std::size_t i = 0; i < take; ++i) { chars_[size_ + i] = text[i]; }
    size_ = static_cast<std::uint8_t>(size_ + take);
    chars_[size_] = '\0';
}

void RelPath::append_byte_hex(std::uint8_t byte) noexcept {
    const std::array<char, 2> hex{kHexDigits[byte >> 4U], kHexDigits[byte & 0x0FU]};
    append(std::string_view{hex.data(), hex.size()});
}

RelPath shard_relative_path(Ns ns, const Uuid& id) noexcept {
    RelPath path;
    path.append(kNsRoot);
    path.append("/");
    path.append(ns.dir());
    path.append("/");
    path.append_byte_hex(id[0]);
    path.append("/");
    path.append_byte_hex(id[1]);
    return path;
}

// ".w640.avif". The digits are written by hand rather than through
// std::to_string, which would allocate on a path that must not.
void RelPath::append_variant(VariantKey key) noexcept {
    append(".w");
    std::array<char, 5> digits{};
    std::size_t written = 0;
    std::uint16_t value = key.width;
    while (value > 0 && written < digits.size()) {
        digits[written++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    for (std::size_t i = written; i > 0; --i) {
        append(std::string_view{&digits[i - 1], 1});
    }
    append(".");
    append(format_extension(key.format));
}

RelPath media_relative_path(Ns ns, const Uuid& id, VariantKey key) noexcept {
    RelPath path = shard_relative_path(ns, id);
    path.append("/");
    for (const std::uint8_t byte : id) { path.append_byte_hex(byte); }
    if (!key.is_master()) { path.append_variant(key); }
    return path;
}

RelPath derived_temp_path(const Uuid& id, VariantKey key) noexcept {
    RelPath path;
    path.append(kTmpDir);
    path.append("/");
    for (const std::uint8_t byte : id) { path.append_byte_hex(byte); }
    if (key.is_master()) {
        path.append(".m");
    } else {
        path.append_variant(key);
    }
    path.append(".tmp");
    return path;
}

RelPath temp_relative_path(const Uuid& id) noexcept {
    RelPath path;
    path.append(kTmpDir);
    path.append("/");
    for (const std::uint8_t byte : id) { path.append_byte_hex(byte); }
    path.append(kPartSuffix);
    return path;
}

std::optional<VariantKey> parse_variant(std::string_view text) noexcept {
    // Grammar: 'w' <1-4 digits, no leading zero> '.' <"avif"|"webp">. Anything
    // else is rejected outright — this is the only place a request-supplied byte
    // influences a path, and it influences it by SELECTING a table entry rather
    // than by contributing characters to it.
    if (text.size() < 7 || text.front() != 'w') { return std::nullopt; }
    const std::size_t dot = text.find('.', 1);
    if (dot == std::string_view::npos) { return std::nullopt; }

    const std::string_view digits = text.substr(1, dot - 1);
    if (digits.empty() || digits.size() > 4 || digits.front() == '0') { return std::nullopt; }
    std::uint32_t width = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') { return std::nullopt; }
        width = width * 10 + static_cast<std::uint32_t>(c - '0');
    }

    const std::string_view extension = text.substr(dot + 1);
    Format format{};
    if (extension == format_extension(Format::Avif)) {
        format = Format::Avif;
    } else if (extension == format_extension(Format::Webp)) {
        format = Format::Webp;
    } else {
        return std::nullopt;
    }

    const VariantKey key{static_cast<std::uint16_t>(width), format};
    if (!is_known_variant(key) || key.is_master()) { return std::nullopt; }
    return key;
}

// --- Fd --------------------------------------------------------------------

void Fd::reset() noexcept {
    if (fd_ >= 0) {
        // close() can report EINTR, and on Linux the descriptor is released
        // regardless; retrying could close a descriptor another thread has just
        // been handed.
        ::close(fd_);
        fd_ = -1;
    }
}

// --- Storage ---------------------------------------------------------------

Storage::Storage(std::string_view storage_root)
    : root_path_{}, root_{}, tmp_{}, namespaces_{} {
    std::error_code ec;
    const std::filesystem::path canonical =
        std::filesystem::canonical(std::filesystem::path{storage_root}, ec);
    if (ec) {
        throw std::runtime_error{"STORAGE_ROOT does not exist or is not resolvable"};
    }
    root_path_ = canonical.string();

    root_ = Fd{::open(root_path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (!root_.valid()) { throw std::runtime_error{"STORAGE_ROOT is not an openable directory"}; }

    const std::string tmp_name{kTmpDir};
    const std::string ns_name{kNsRoot};
    if (!make_directory_at(root_.get(), tmp_name.c_str()) ||
        !make_directory_at(root_.get(), ns_name.c_str())) {
        throw std::runtime_error{"cannot create tmp/ and ns/ under STORAGE_ROOT"};
    }

    tmp_ = open_directory_at(root_.get(), tmp_name.c_str());
    const Fd ns_root = open_directory_at(root_.get(), ns_name.c_str());
    if (!tmp_.valid() || !ns_root.valid()) {
        // O_NOFOLLOW: one of them is a symlink, which is either a compromise or
        // a misconfiguration. Both are boot failures.
        throw std::runtime_error{"tmp/ or ns/ under STORAGE_ROOT is not a real directory"};
    }

    struct ::stat tmp_stat{};
    if (::fstat(tmp_.get(), &tmp_stat) != 0) {
        throw std::runtime_error{"cannot stat tmp/ under STORAGE_ROOT"};
    }

    for (const Ns ns : kAllNamespaces) {
        const std::size_t i = ns.index();
        const std::string directory{ns.dir()};
        if (!make_directory_at(ns_root.get(), directory.c_str())) {
            throw std::runtime_error{"cannot create a namespace directory under ns/"};
        }
        namespaces_[i] = open_directory_at(ns_root.get(), directory.c_str());
        if (!namespaces_[i].valid()) {
            throw std::runtime_error{"a namespace directory under ns/ is not a real directory"};
        }

        struct ::stat ns_stat{};
        if (::fstat(namespaces_[i].get(), &ns_stat) != 0) {
            throw std::runtime_error{"cannot stat a namespace directory"};
        }
        // A cross-device rename is not a rename: it degrades to a copy and stops
        // being atomic, so a reader can observe a half-written file.
        // Checked at boot because discovering it during an upload means it has
        // already happened.
        if (ns_stat.st_dev != tmp_stat.st_dev) {
            throw std::runtime_error{
                "tmp/ and ns/ are on different filesystems; the publish rename would not be "
                "atomic"};
        }
    }
}

void Storage::init(std::string_view storage_root) {
    if (g_storage) { throw std::runtime_error{"fs::Storage::init called twice"}; }
    g_storage = std::make_unique<Storage>(storage_root);
}

void Storage::shutdown() noexcept { g_storage.reset(); }

bool Storage::initialised() noexcept { return static_cast<bool>(g_storage); }

const Storage& Storage::instance() {
    if (!g_storage) { throw std::runtime_error{"fs::Storage::init has not been called"}; }
    return *g_storage;
}

Fd Storage::open_media(Ns ns, const Uuid& id, VariantKey key) const noexcept {
    if (!is_known_variant(key)) { return Fd{}; }

    // Component by component, O_NOFOLLOW on each: O_NOFOLLOW applies to the
    // final component only, so a single openat("aa/bb/<hex>") would leave both
    // shard levels open to a planted symlink.
    const ShardName first{id[0]};
    const Fd level_one = open_directory_at(ns_fd(ns), first.c_str());
    if (!level_one.valid()) { return Fd{}; }

    const ShardName second{id[1]};
    const Fd level_two = open_directory_at(level_one.get(), second.c_str());
    if (!level_two.valid()) { return Fd{}; }

    const MediaName name{id};
    if (key.is_master()) {
        return Fd{::openat(level_two.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
    }

    // The variant suffix is appended to the same 32-hex basename. Built here
    // rather than reusing media_relative_path so the open stays relative to the
    // shard descriptor rather than re-walking from the namespace.
    const RelPath full = media_relative_path(ns, id, key);
    const std::string_view basename = full.view().substr(full.view().rfind('/') + 1);
    std::array<char, 48> leaf{};
    if (basename.size() >= leaf.size()) { return Fd{}; }
    for (std::size_t i = 0; i < basename.size(); ++i) { leaf[i] = basename[i]; }
    return Fd{::openat(level_two.get(), leaf.data(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
}

Result<Fd> Storage::open_shard_for_write(Ns ns, const Uuid& id) const noexcept {
    // The two shard levels are created here rather than at boot: 65 536
    // directories per namespace is 327 680 inodes to create on every start, for
    // a tree that fills in on demand. This is the upload path only — never a
    // read — and it is at most two mkdirat against an fsync that costs orders of
    // magnitude more (see the header for what IS created at boot).
    const ShardName first{id[0]};
    if (!make_directory_at(ns_fd(ns), first.c_str())) { return fail(ErrorCode::Internal); }

    const Fd level_one = open_directory_at(ns_fd(ns), first.c_str());
    if (!level_one.valid()) { return fail(ErrorCode::Internal); }

    const ShardName second{id[1]};
    if (!make_directory_at(level_one.get(), second.c_str())) { return fail(ErrorCode::Internal); }

    Fd level_two = open_directory_at(level_one.get(), second.c_str());
    if (!level_two.valid()) { return fail(ErrorCode::Internal); }
    return Result<Fd>{std::move(level_two)};
}

StorageStats Storage::stats() const noexcept {
    struct ::statvfs vfs{};
    if (::fstatvfs(root_.get(), &vfs) != 0) { return StorageStats{0, 0}; }
    // f_bavail, not f_bfree: the reserved blocks are not available to this
    // process, and counting them is how a "full" disk still reports free space.
    return StorageStats{static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize,
                        static_cast<std::uint64_t>(vfs.f_blocks) * vfs.f_frsize};
}

std::optional<std::string> resolve_within_root(std::string_view root,
                                               std::string_view candidate) noexcept {
    try {
        const std::filesystem::path root_path{root};
        std::error_code ec;
        const std::filesystem::path canonical_root =
            std::filesystem::weakly_canonical(root_path, ec);
        if (ec) { return std::nullopt; }

        const std::filesystem::path target{candidate};
        // An absolute candidate is checked as given; a relative one is joined.
        // operator/ with an absolute right-hand side REPLACES the left, which is
        // exactly the traversal this function exists to catch, so the two cases
        // are separated rather than left to the operator.
        const std::filesystem::path joined =
            target.is_absolute() ? target : (canonical_root / target);
        const std::filesystem::path resolved = std::filesystem::weakly_canonical(joined, ec);
        if (ec) { return std::nullopt; }

        const std::string resolved_string = resolved.string();
        if (!config::path_is_within(canonical_root.string(), resolved_string)) {
            return std::nullopt;
        }
        return resolved_string;
    } catch (...) {
        // weakly_canonical allocates and can throw bad_alloc; a resolver that
        // throws would be a resolver callers stop checking.
        return std::nullopt;
    }
}

}  // namespace anvil::fs
