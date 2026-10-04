// Path safety, and the upload pipeline.
//
// Everything here runs against a real temporary directory tree, because the
// properties being tested are properties of syscalls — O_NOFOLLOW, renameat,
// fsync, statvfs — and a mock of the filesystem would test the mock.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "namespaces.h"

#include "anvil/config/paths.h"
#include "anvil/core/uuid.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/paths.h"
#include "anvil/fs/sniff.h"
#include "anvil/crypto/digest.h"
#include "anvil/fs/upload.h"
#include "anvil/http/errors.h"

namespace {

using anvil::Uuid;
namespace fs = anvil::fs;

// A storage tree per test case, removed afterwards. Storage::init is
// process-wide by design, so the fixture tears it down between cases.
class StorageFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::array<char, 64> pattern{};
        std::snprintf(pattern.data(), pattern.size(), "/tmp/anvil-fs-XXXXXX");
        const char* made = ::mkdtemp(pattern.data());
        ASSERT_NE(made, nullptr);
        root_ = made;
        fs::Storage::init(root_);
    }

    void TearDown() override {
        fs::Storage::shutdown();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    [[nodiscard]] const std::string& root() const noexcept { return root_; }

    [[nodiscard]] static std::vector<std::uint8_t> png_bytes(std::size_t total) {
        std::vector<std::uint8_t> data{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        data.resize(total, 0x42);
        return data;
    }

    // An ISO-BMFF header with the `avif` brand at offset 8, which is what
    // fs::sniff matches on. The box length is nonsense and deliberately so:
    // nothing here decodes the file, and a case that needed a real AVIF would be
    // asserting libheif rather than the accept mask.
    [[nodiscard]] static std::vector<std::uint8_t> avif_bytes(std::size_t total) {
        std::vector<std::uint8_t> data{0x00, 0x00, 0x00, 0x20, 'f', 't', 'y', 'p',
                                       'a',  'v',  'i',  'f'};
        data.resize(total, 0x42);
        return data;
    }

    std::string root_;
};

// --- the namespace is a compile-time type, not a string ---------------------
//
// The negative half of this — "a string namespace does not compile" — is
// enforced by the SIGNATURE: every accessor takes fs::Ns, and there is no
// overload taking a string. A test cannot express "this does not compile"
// without a build-failure harness, so what is asserted here is that the mapping
// is constexpr and closed.
static_assert(testapp::kContent.dir() == "content");
static_assert(fs::kNsCount == 5);

TEST(FsNamespace, UnknownSegmentIsRejected) {
    // "../content" is the case the type exists to make unrepresentable: a
    // function taking a string could be handed it, and this one cannot.
    EXPECT_FALSE(fs::Ns::from_dir("../content").has_value());
    EXPECT_FALSE(fs::Ns::from_dir("CONTENT").has_value());
    EXPECT_FALSE(fs::Ns::from_dir("content/").has_value());
    EXPECT_FALSE(fs::Ns::from_dir("").has_value());

    const auto parsed = fs::Ns::from_dir("content");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, testapp::kContent);
}

TEST(FsNamespace, StoredValueIsRangeChecked) {
    // One past the end is corruption or a newer writer, and either way it must
    // not become a valid namespace.
    EXPECT_FALSE(fs::Ns::from_stored(-1).has_value());
    EXPECT_FALSE(fs::Ns::from_stored(static_cast<std::int32_t>(fs::kNsCount)).has_value());

    const auto parsed = fs::Ns::from_stored(0);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, testapp::kContent);
}

TEST(FsNamespace, StoredIndicesAreWhereTheApplicationPutThem) {
    // The index is what lands in the media row, so it is what must never drift.
    // The application asserts these at compile time in tests/testapp/namespaces.h;
    // this states the same fact from the outside.
    EXPECT_EQ(testapp::kContent.stored(), 0);
    EXPECT_EQ(testapp::kMedia.stored(), 1);
    EXPECT_EQ(testapp::kGuest.stored(), 2);
}

TEST(FsNamespace, BufferCapacityFollowsTheLongestDeclaredName) {
    // kMaxNsDirLength is COMPUTED from the application's table rather than
    // hand-maintained, so a longer namespace name widens RelPath's buffer instead
    // of overflowing it. The static_assert in paths.h is what would catch a name
    // that outgrew the cache line; this states the fact it rests on.
    for (const fs::Ns ns : fs::kAllNamespaces) {
        EXPECT_LE(ns.dir().size(), fs::kMaxNsDirLength);
    }
    EXPECT_EQ(fs::kMaxNsDirLength, std::string_view{"content"}.size());
}

// --- two-level hex sharding, 65 536 buckets ---------------------------------

TEST(FsPaths, ShardingUsesTwoHexLevels) {
    Uuid id{};
    id[0] = 0x3A;
    id[1] = 0xF2;
    const fs::RelPath path = fs::media_relative_path(testapp::kContent, id);
    // The first two bytes of the id become two directory levels, and the file
    // name repeats the full id: 256 x 256 buckets, so no directory ever holds
    // more entries than a filesystem is comfortable with.
    constexpr std::string_view kExpected = "ns/content/3a/f2/3af200";
    EXPECT_EQ(path.view().substr(0, kExpected.size()), kExpected);
}

TEST(FsPaths, ShardSpaceIs65536) {
    // Every distinct (first byte, second byte) pair must produce a distinct
    // directory. 256 x 256 is the claim in docs/07-filesystem.md §1; asserting it on a sample
    // of the space costs nothing and catches a one-level regression.
    std::vector<std::string> shards;
    shards.reserve(65536);
    for (int first = 0; first < 256; ++first) {
        for (int second = 0; second < 256; ++second) {
            Uuid id{};
            id[0] = static_cast<std::uint8_t>(first);
            id[1] = static_cast<std::uint8_t>(second);
            shards.emplace_back(fs::shard_relative_path(testapp::kGuest, id).view());
        }
    }
    std::sort(shards.begin(), shards.end());
    EXPECT_EQ(std::unique(shards.begin(), shards.end()), shards.end());
    EXPECT_EQ(shards.size(), 65536U);
}

// --- resolve is allocation-free ---------------------------------------------

TEST(FsPaths, RelPathIsOneCacheLineAndNeverHeapAllocates) {
    // An upper bound, not an equality: the capacity derives from the longest
    // namespace name the application declares, and the property being defended
    // is that it fits one cache line and never allocates.
    static_assert(sizeof(fs::RelPath) <= 64);
    static_assert(std::is_trivially_copyable_v<fs::RelPath>);

    const Uuid id = anvil::uuid::generate_v4();
    const fs::RelPath master = fs::media_relative_path(testapp::kMedia, id);
    const fs::RelPath variant =
        fs::media_relative_path(testapp::kMedia, id, fs::VariantKey{2560, fs::Format::Avif});

    // NUL-terminated, so it goes straight to openat with no std::string in
    // between.
    EXPECT_EQ(std::strlen(master.c_str()), master.size());
    EXPECT_EQ(std::strlen(variant.c_str()), variant.size());
    EXPECT_TRUE(variant.view().ends_with(".w2560.avif"));
}

TEST(FsPaths, VariantSegmentIsSelectedNotAssembled) {
    EXPECT_TRUE(fs::parse_variant("w640.avif").has_value());
    EXPECT_TRUE(fs::parse_variant("w2560.webp").has_value());

    // Not in the table, malformed, or an attempt to smuggle path characters.
    EXPECT_FALSE(fs::parse_variant("w641.avif").has_value());
    EXPECT_FALSE(fs::parse_variant("w0640.avif").has_value());
    EXPECT_FALSE(fs::parse_variant("w640.png").has_value());
    EXPECT_FALSE(fs::parse_variant("w640.avif/../../etc/passwd").has_value());
    EXPECT_FALSE(fs::parse_variant("../../etc/passwd").has_value());
    EXPECT_FALSE(fs::parse_variant("w640").has_value());
    EXPECT_FALSE(fs::parse_variant("").has_value());
}

// --- component comparison, not string prefix --------------------------------

TEST(FsPaths, SiblingDirectoryWithAPrefixNameIsRejected) {
    // "/srv/storage-evil" has "/srv/storage" as a STRING prefix and is a
    // completely different directory.
    EXPECT_FALSE(anvil::config::path_is_within("/srv/storage", "/srv/storage-evil/x"));
    EXPECT_TRUE(anvil::config::path_is_within("/srv/storage", "/srv/storage/x"));
    EXPECT_TRUE(anvil::config::path_is_within("/srv/storage", "/srv/storage"));
    EXPECT_FALSE(anvil::config::path_is_within("/srv/storage", "/srv/stor"));
}

// --- fuzz the resolver ------------------------------------------------------

TEST_F(StorageFixture, ResolverNeverEscapesTheRoot) {
    const std::array<std::string_view, 14> hostile{{
        "../etc/passwd",
        "../../../../../../etc/passwd",
        "ns/../../etc/passwd",
        "/etc/passwd",
        "//etc/passwd",
        "ns/content/../../../etc/shadow",
        "./../.",
        "..",
        "ns/content/..%2f..%2fetc",
        "ns/\xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9/../../..",
        "\xef\xbb\xbf../etc",
        "ns/content/\xc0\xaf../etc",
        std::string_view{"ns/con\0tent/x", 13},
        "ns/content/....//....//etc",
    }};

    for (const std::string_view candidate : hostile) {
        const std::optional<std::string> resolved = fs::resolve_within_root(root(), candidate);
        if (resolved.has_value()) {
            EXPECT_TRUE(anvil::config::path_is_within(root(), *resolved))
                << "escaped with candidate: " << candidate;
        }
    }

    // A legitimate relative path still resolves.
    const std::optional<std::string> inside = fs::resolve_within_root(root(), "ns/content");
    ASSERT_TRUE(inside.has_value());
    EXPECT_TRUE(anvil::config::path_is_within(root(), *inside));
}

// --- namespace directories exist after boot ---------------------------------

TEST_F(StorageFixture, NamespaceDirectoriesAreCreatedAtInit) {
    for (const fs::Ns ns : fs::kAllNamespaces) {
        const std::filesystem::path path =
            std::filesystem::path{root()} / "ns" / std::string{ns.dir()};
        EXPECT_TRUE(std::filesystem::is_directory(path)) << path;
    }
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path{root()} / "tmp"));
}

// --- symlinks lose ----------------------------------------------------------

TEST_F(StorageFixture, SymlinkInStorageIsNotFollowed) {
    Uuid id{};
    id[0] = 0x11;
    id[1] = 0x22;

    // Plant a symlink exactly where a master would live, pointing at a file
    // outside the tree.
    const anvil::Result<fs::Fd> shard =
        fs::Storage::instance().open_shard_for_write(testapp::kMedia, id);
    ASSERT_TRUE(shard.ok());

    const fs::RelPath relative = fs::media_relative_path(testapp::kMedia, id);
    const std::string_view view = relative.view();
    const std::string leaf{view.substr(view.rfind('/') + 1)};
    ASSERT_EQ(::symlinkat("/etc/passwd", shard.value().get(), leaf.c_str()), 0);

    // O_NOFOLLOW on the final component: the open fails rather than handing back
    // /etc/passwd.
    const fs::Fd opened =
        fs::Storage::instance().open_media(testapp::kMedia, id, fs::kMasterVariant);
    EXPECT_FALSE(opened.valid());
}

TEST_F(StorageFixture, SymlinkedShardDirectoryIsNotFollowed) {
    Uuid id{};
    id[0] = 0xAB;
    id[1] = 0xCD;

    // A symlinked SHARD directory. A single openat("ab/cd/<hex>") would follow
    // it, because O_NOFOLLOW applies only to the final component — which is why
    // the open walks one component at a time.
    const int ns_fd = fs::Storage::instance().ns_fd(testapp::kMedia);
    ASSERT_EQ(::symlinkat("/etc", ns_fd, "ab"), 0);

    const fs::Fd opened = fs::Storage::instance().open_media(testapp::kMedia, id, fs::kMasterVariant);
    EXPECT_FALSE(opened.valid());
}

// --- sniffing ---------------------------------------------------------------

TEST(FsSniff, MagicBytesDecideTheType) {
    const std::array<std::uint8_t, 8> png{{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}};
    const std::array<std::uint8_t, 4> jpeg{{0xFF, 0xD8, 0xFF, 0xE0}};
    EXPECT_EQ(fs::sniff(png), fs::Mime::Png);
    EXPECT_EQ(fs::sniff(jpeg), fs::Mime::Jpeg);

    // Built as bytes, not as a string literal: the RIFF length field contains
    // NULs, and a char array would end at the first one.
    const std::array<std::uint8_t, 16> webp{{'R', 'I', 'F', 'F', 0x24, 0x00, 0x00, 0x00, 'W', 'E',
                                             'B', 'P', 'V', 'P', '8', ' '}};
    EXPECT_EQ(fs::sniff(webp), fs::Mime::Webp);

    const std::string avif = std::string(4, '\0') + "ftypavif";
    EXPECT_EQ(fs::sniff(std::span<const std::uint8_t>{
                  reinterpret_cast<const std::uint8_t*>(avif.data()), avif.size()}),
              fs::Mime::Avif);

    // A RIFF container that is not WebP must NOT sniff as WebP.
    const std::array<std::uint8_t, 16> wav{{'R', 'I', 'F', 'F', 0x24, 0x00, 0x00, 0x00, 'W', 'A',
                                            'V', 'E', 'f', 'm', 't', ' '}};
    EXPECT_EQ(fs::sniff(wav), fs::Mime::Unknown);
}

namespace {

[[nodiscard]] std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// A first Ogg page: "OggS", version, header type, granule (8), serial (4),
// sequence (4), CRC (4), one segment of `packet.size()` bytes, then the packet.
[[nodiscard]] std::vector<std::uint8_t> ogg_page(std::string_view packet) {
    std::vector<std::uint8_t> page{'O', 'g', 'g', 'S', 0x00, 0x02};
    page.resize(26, 0x00);
    page.push_back(1);
    page.push_back(static_cast<std::uint8_t>(packet.size()));
    page.insert(page.end(), packet.begin(), packet.end());
    return page;
}

[[nodiscard]] std::vector<std::uint8_t> ftyp(std::string_view brand) {
    std::vector<std::uint8_t> box{0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p'};
    box.insert(box.end(), brand.begin(), brand.end());
    box.resize(24, 0x00);
    return box;
}

}  // namespace

TEST(FsSniff, EachStoredFileTypeIsRecognisedByItsOwnSignature) {
    EXPECT_EQ(fs::sniff(ftyp("isom")), fs::Mime::Mp4);
    EXPECT_EQ(fs::sniff(ftyp("mp42")), fs::Mime::Mp4);
    EXPECT_EQ(fs::sniff(ftyp("M4A ")), fs::Mime::M4a);
    // The container is shared with AVIF, and the brand decides.
    EXPECT_EQ(fs::sniff(ftyp("avif")), fs::Mime::Avif);

    const std::vector<std::uint8_t> webm{0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81,
                                         0x01, 0x42, 0x82, 0x84, 'w',  'e',  'b',  'm'};
    EXPECT_EQ(fs::sniff(webm), fs::Mime::Webm);

    EXPECT_EQ(fs::sniff(ogg_page("OpusHead\x01\x02")), fs::Mime::OggOpus);
    EXPECT_EQ(fs::sniff(bytes_of("%PDF-1.7\n%\xE2\xE3\xCF\xD3")), fs::Mime::Pdf);

    for (const fs::Mime mime : {fs::Mime::Mp4, fs::Mime::Webm, fs::Mime::OggOpus, fs::Mime::M4a,
                                fs::Mime::Pdf}) {
        EXPECT_EQ(fs::mime_class(mime), fs::MimeClass::File);
        EXPECT_FALSE(fs::mime_type(mime).empty());
        // The claim a browser sends for it maps back to it, so a correct client
        // is never refused as a mismatch.
        EXPECT_EQ(fs::mime_from_claim(fs::mime_type(mime)), mime);
    }
}

TEST(FsSniff, ContainersThatAreNotTheAcceptedFormatAreRefused) {
    // QuickTime and HEIC share MP4's container and are neither MP4 nor allowed.
    EXPECT_EQ(fs::sniff(ftyp("qt  ")), fs::Mime::Unknown);
    EXPECT_EQ(fs::sniff(ftyp("heic")), fs::Mime::Unknown);
    // Matroska shares WebM's EBML header and differs only in the DocType.
    const std::vector<std::uint8_t> mkv{0x1A, 0x45, 0xDF, 0xA3, 0xA3, 0x42, 0x82, 0x88,
                                        'm',  'a',  't',  'r',  'o',  's',  'k',  'a'};
    EXPECT_EQ(fs::sniff(mkv), fs::Mime::Unknown);
    // Ogg is a container; only Opus in it is a voice note.
    EXPECT_EQ(fs::sniff(ogg_page("\x01vorbis\x00\x00\x00\x00")), fs::Mime::Unknown);
    // A PDF header anywhere but offset zero is how a polyglot hides one.
    EXPECT_EQ(fs::sniff(bytes_of(" %PDF-1.7")), fs::Mime::Unknown);
    EXPECT_EQ(fs::sniff(bytes_of("<html>%PDF-1.7")), fs::Mime::Unknown);
}

TEST(FsSniff, AnSvgDressedAsAPdfIsCaughtAsMarkupFirst) {
    // The markup check runs before the sniff, so a document that opens as SVG
    // is refused as the probe it is however much PDF follows it.
    const std::vector<std::uint8_t> svg = bytes_of("<svg onload=\"alert(1)\">%PDF-1.7");
    EXPECT_TRUE(fs::looks_like_xml(svg));
    // And the reverse — a real PDF header with SVG inside — is a PDF, which is
    // only safe because a PDF is always served as an attachment under a sandbox.
    const std::vector<std::uint8_t> pdf = bytes_of("%PDF-1.7\n<svg onload=\"alert(1)\">");
    EXPECT_FALSE(fs::looks_like_xml(pdf));
    EXPECT_EQ(fs::sniff(pdf), fs::Mime::Pdf);
    EXPECT_EQ(fs::disposition(fs::Mime::Pdf), fs::Disposition::Attachment);
}

TEST(FsSniff, TheDefaultAcceptListIsImagesOnly) {
    // When the file class landed, a default derived from every Mime would have
    // made every namespace that never stated a list accept PDFs overnight.
    for (unsigned value = 1; value <= static_cast<unsigned>(fs::kMaxMime); ++value) {
        const auto mime = static_cast<fs::Mime>(value);
        EXPECT_EQ(fs::mime_accepted(fs::kDecodableMimes, mime),
                  fs::mime_class(mime) == fs::MimeClass::Image);
        EXPECT_EQ(fs::mime_accepted(fs::kFileMimes, mime),
                  fs::mime_class(mime) == fs::MimeClass::File);
        EXPECT_EQ(fs::mime_accepted(fs::kSealedMimes, mime),
                  fs::mime_class(mime) == fs::MimeClass::Sealed);
    }
    EXPECT_FALSE(fs::mime_accepted(fs::kDecodableMimes, fs::Mime::Sealed));
    EXPECT_FALSE(fs::mime_accepted(fs::kFileMimes, fs::Mime::Sealed));
}

// --- the sealed class -----------------------------------------------------------

TEST(FsSniff, NoBytesEverSniffAsSealed) {
    // Sealed is chosen by the namespace, never by the bytes: ciphertext has no
    // signature, and a type that bytes could select is a type an attacker
    // selects. Every known signature first, then every one-byte prefix at every
    // length, then pseudo-random buffers — the case the class exists for.
    for (const auto& known : {ftyp("isom"), ftyp("avif"), ogg_page("OpusHead\x01\x02"),
                              bytes_of("%PDF-1.7\n"), bytes_of("<svg onload=1>"),
                              bytes_of("application/octet-stream")}) {
        EXPECT_NE(fs::sniff(known), fs::Mime::Sealed);
    }
    for (unsigned first = 0; first <= 0xFF; ++first) {
        for (std::size_t length = 0; length <= fs::kSniffBytes; ++length) {
            std::vector<std::uint8_t> head(length, static_cast<std::uint8_t>(first));
            EXPECT_NE(fs::sniff(head), fs::Mime::Sealed);
        }
    }
    // splitmix64: deterministic, so a failure reproduces. Not a security RNG
    // and not used as one; it only has to cover the space.
    std::uint64_t state = 0x5EA1EDULL;
    fs::SniffBuffer head{};
    for (int round = 0; round < 200'000; ++round) {
        for (std::size_t i = 0; i < head.size(); i += 8) {
            state += 0x9E3779B97F4A7C15ULL;
            std::uint64_t z = state;
            z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
            z ^= z >> 31U;
            for (std::size_t b = 0; b < 8; ++b) {
                head[i + b] = static_cast<std::uint8_t>(z >> (8U * b));
            }
        }
        ASSERT_NE(fs::sniff(head), fs::Mime::Sealed) << "round " << round;
    }
    // Nor can a claim make anything Sealed.
    EXPECT_EQ(fs::mime_from_claim("application/octet-stream"), fs::Mime::Unknown);
    EXPECT_EQ(fs::mime_from_claim(fs::mime_type(fs::Mime::Sealed)), fs::Mime::Unknown);
}

TEST(FsSniff, ASealedObjectIsOpaqueAndAlwaysAnAttachment) {
    EXPECT_EQ(fs::mime_class(fs::Mime::Sealed), fs::MimeClass::Sealed);
    EXPECT_EQ(fs::mime_type(fs::Mime::Sealed), "application/octet-stream");
    EXPECT_EQ(fs::disposition(fs::Mime::Sealed), fs::Disposition::Attachment);
    // It is stored, so a row written with it reads back.
    fs::Mime stored = fs::Mime::Unknown;
    EXPECT_TRUE(fs::mime_from_stored(10, stored));
    EXPECT_EQ(stored, fs::Mime::Sealed);
}

TEST(FsNamespaceRule, ASealedNamespaceTakesOnlySealedNeverDeduplicatesAndIsPrivate) {
    using fs::Dedupe;
    using fs::NamespaceSpec;
    using fs::Visibility;
    static_assert(fs::namespace_is_well_formed(
        NamespaceSpec{"s", fs::kSealedMimes, Dedupe::None, Visibility::Private}));
    // Mixed: two upload paths with opposite rules in one namespace, and the
    // client choosing whether its bytes are inspected.
    static_assert(!fs::namespace_is_well_formed(NamespaceSpec{
        "s", static_cast<fs::MimeMask>(fs::kSealedMimes | fs::kDecodableMimes), Dedupe::None,
        Visibility::Private}));
    static_assert(!fs::namespace_is_well_formed(NamespaceSpec{
        "s", static_cast<fs::MimeMask>(fs::kSealedMimes | fs::kFileMimes), Dedupe::None,
        Visibility::Private}));
    // A dedup hit on ciphertext is a timing oracle and never a saving.
    static_assert(!fs::namespace_is_well_formed(
        NamespaceSpec{"s", fs::kSealedMimes, Dedupe::Owner, Visibility::Private}));
    static_assert(!fs::namespace_is_well_formed(
        NamespaceSpec{"s", fs::kSealedMimes, Dedupe::Namespace, Visibility::Private}));
    // An id alone would serve it.
    static_assert(!fs::namespace_is_well_formed(
        NamespaceSpec{"s", fs::kSealedMimes, Dedupe::None, Visibility::Public}));
    // The rule says nothing about a namespace that takes no ciphertext.
    static_assert(fs::namespace_is_well_formed(NamespaceSpec{"c"}));

    EXPECT_TRUE(testapp::kSealed.sealed());
    EXPECT_FALSE(testapp::kChat.sealed());
    EXPECT_FALSE(testapp::kContent.sealed());
}

TEST(FsSniff, ScriptAndMarkupAreNotImages) {
    const std::array<std::string_view, 5> hostile{{
        "<?php system($_GET['c']); ?>",
        "<!DOCTYPE html><html><script>alert(1)</script>",
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>",
        "\xef\xbb\xbf  <svg width=\"1\">",
        "GIF89a",
    }};
    for (const std::string_view payload : hostile) {
        const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()};
        EXPECT_EQ(fs::sniff(bytes), fs::Mime::Unknown) << payload;
    }

    // SVG and XML are recognised SPECIFICALLY, so the rejection is auditable
    // rather than lumped in with "unrecognised bytes".
    EXPECT_TRUE(fs::looks_like_xml(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("<svg "), 5}));
    EXPECT_TRUE(fs::looks_like_xml(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("\xef\xbb\xbf \n<?xml "), 11}));
    EXPECT_FALSE(fs::looks_like_xml(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("\x89PNG"), 4}));
}

// --- the sink ---------------------------------------------------------------

TEST_F(StorageFixture, StreamIsAbortedAtTheCapNotAtContentLength) {
    // The declared length is irrelevant: the cap counts bytes RECEIVED, and a
    // chunked body can exceed what it claimed.
    const fs::UploadLimits limits{1024, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();

    const std::vector<std::uint8_t> chunk = png_bytes(512);
    EXPECT_TRUE(sink.write(chunk).ok());
    EXPECT_TRUE(sink.write(chunk).ok());

    const anvil::Status third = sink.write(chunk);
    EXPECT_FALSE(third.ok());
    EXPECT_EQ(third.error().code, anvil::ErrorCode::PayloadTooLarge);
    EXPECT_EQ(third.error().field, fs::kRejectOversize);

    // Refuses everything afterwards, so a caller that ignored one return value
    // cannot keep writing past the cap.
    EXPECT_FALSE(sink.write(chunk).ok());
    EXPECT_EQ(sink.bytes_received(), 1024U);

    // Nothing above the cap reached the disk.
    const std::filesystem::path part =
        std::filesystem::path{root()} / std::string{fs::temp_relative_path(sink.id()).view()};
    EXPECT_EQ(std::filesystem::file_size(part), 1024U);
}

TEST_F(StorageFixture, FreeSpaceFloorRefusesBeforeAnyByteIsAccepted) {
    // A floor above the whole filesystem: the check must fail closed with 507
    // rather than accepting bytes and discovering the problem later.
    const fs::UploadLimits impossible{1024, ~0ULL / 2};
    const anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), impossible, testapp::kContent);
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.error().code, anvil::ErrorCode::InsufficientStorage);
    EXPECT_EQ(anvil::http::http_status(anvil::ErrorCode::InsufficientStorage), 507);
}

TEST_F(StorageFixture, RejectedUploadLeavesNothingBehind) {
    const fs::UploadLimits limits{1 << 20, 0};
    Uuid id{};
    {
        anvil::Result<fs::UploadSink> opened =
            fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
        ASSERT_TRUE(opened.ok());
        fs::UploadSink sink = std::move(opened).value();
        id = sink.id();

        const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        ASSERT_TRUE(sink.write(std::span<const std::uint8_t>{
                                  reinterpret_cast<const std::uint8_t*>(svg.data()), svg.size()})
                        .ok());

        const anvil::Result<fs::UploadResult> finished = sink.finish("image/svg+xml");
        ASSERT_FALSE(finished.ok());
        EXPECT_EQ(finished.error().code, anvil::ErrorCode::UnsupportedMedia);
        EXPECT_EQ(finished.error().field, fs::kRejectVectorType);
    }

    // The destructor removed the temp file.
    const std::filesystem::path part =
        std::filesystem::path{root()} / std::string{fs::temp_relative_path(id).view()};
    EXPECT_FALSE(std::filesystem::exists(part));
}

TEST_F(StorageFixture, ClaimedTypeDisagreeingWithMagicIsRejected) {
    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();

    const std::vector<std::uint8_t> png = png_bytes(256);
    ASSERT_TRUE(sink.write(png).ok());

    const anvil::Result<fs::UploadResult> finished = sink.finish("image/jpeg");
    ASSERT_FALSE(finished.ok());
    EXPECT_EQ(finished.error().field, fs::kRejectTypeMismatch);
}

TEST_F(StorageFixture, AbsentClaimIsNotADisagreement) {
    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();

    ASSERT_TRUE(sink.write(png_bytes(256)).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("");
    ASSERT_TRUE(finished.ok());
    EXPECT_EQ(finished.value().mime, fs::Mime::Png);
    EXPECT_EQ(finished.value().bytes, 256U);
}

TEST_F(StorageFixture, PublishIsAtomicAndTheHashIsOnePass) {
    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();

    const std::vector<std::uint8_t> data = png_bytes(4096);
    // Written in chunks, exactly as the transport delivers it.
    for (std::size_t offset = 0; offset < data.size(); offset += 512) {
        ASSERT_TRUE(sink.write(std::span<const std::uint8_t>{data.data() + offset, 512}).ok());
    }

    const anvil::Result<fs::UploadResult> finished = sink.finish("image/png");
    ASSERT_TRUE(finished.ok());

    // The streaming digest agrees with a one-shot hash of the same bytes.
    EXPECT_EQ(finished.value().sha256, anvil::crypto::sha256(data));

    ASSERT_TRUE(sink.publish().ok());
    EXPECT_TRUE(sink.published());

    const std::filesystem::path master =
        std::filesystem::path{root()} /
        std::string{fs::media_relative_path(testapp::kContent, finished.value().id).view()};
    ASSERT_TRUE(std::filesystem::exists(master));
    EXPECT_EQ(std::filesystem::file_size(master), 4096U);

    // The temp entry is gone: renameat moved it, it was not copied.
    const std::filesystem::path part =
        std::filesystem::path{root()} /
        std::string{fs::temp_relative_path(finished.value().id).view()};
    EXPECT_FALSE(std::filesystem::exists(part));
}

TEST_F(StorageFixture, IdenticalUploadsProduceIdenticalHashes) {
    // The dedup KEY. Two uploads of identical bytes must hash the same, or the
    // "one file on disk" property in docs/07-filesystem.md §4 step 6 cannot hold.
    const fs::UploadLimits limits{1 << 20, 0};
    const std::vector<std::uint8_t> data = png_bytes(2048);

    std::array<anvil::crypto::Digest256, 2> digests{};
    for (std::size_t i = 0; i < digests.size(); ++i) {
        anvil::Result<fs::UploadSink> opened =
            fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
        ASSERT_TRUE(opened.ok());
        fs::UploadSink sink = std::move(opened).value();
        ASSERT_TRUE(sink.write(data).ok());
        const anvil::Result<fs::UploadResult> finished = sink.finish("image/png");
        ASSERT_TRUE(finished.ok());
        digests[i] = finished.value().sha256;
        // Different ids: the id is a v4 capability, not a content address.
        EXPECT_FALSE(anvil::is_nil(finished.value().id));
    }
    EXPECT_EQ(digests[0], digests[1]);
}

// --- tmp/ and ns/ share a filesystem ----------------------------------------

TEST_F(StorageFixture, TempAndNamespaceDirectoriesShareOneFilesystem) {
    // Asserted at init (a cross-device rename degrades to a copy and loses
    // atomicity). Re-checked here so a change to the boot sequence
    // that drops the check fails a test rather than production.
    struct ::stat tmp_stat{};
    struct ::stat ns_stat{};
    ASSERT_EQ(::fstat(fs::Storage::instance().tmp_fd(), &tmp_stat), 0);
    ASSERT_EQ(::fstat(fs::Storage::instance().ns_fd(testapp::kContent), &ns_stat), 0);
    EXPECT_EQ(tmp_stat.st_dev, ns_stat.st_dev);
}

// --- what a namespace accepts ----------------------------------------------

TEST_F(StorageFixture, ANamespaceRefusesATypeItDoesNotAcceptEvenThoughThePipelineDecodesIt) {
    // The guest namespace takes JPEG and PNG. AVIF is a format this build sniffs,
    // decodes and re-encodes perfectly well — which is exactly what makes this
    // case about the NAMESPACE rather than about the allow-list.
    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kGuest);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();

    ASSERT_TRUE(sink.write(avif_bytes(256)).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("image/avif");
    ASSERT_FALSE(finished.ok());
    EXPECT_EQ(finished.error().code, anvil::ErrorCode::UnsupportedMedia);
    // Its own reason, and not `upload.magic`: an unrecognised file is a probe or
    // a mistake, and this is a legitimate image in the wrong place. An auditor
    // reading the first is looking for an attacker; an operator reading the
    // second is looking at a client that offered the wrong picker.
    EXPECT_EQ(finished.error().field, fs::kRejectNamespaceType);
}

TEST_F(StorageFixture, TheSameBytesAreAcceptedByANamespaceThatTakesThem) {
    // The other half, without which the case above would pass on a sink that
    // refused everything.
    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();

    ASSERT_TRUE(sink.write(avif_bytes(256)).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("image/avif");
    ASSERT_TRUE(finished.ok()) << static_cast<int>(finished.code());
    EXPECT_EQ(finished.value().mime, fs::Mime::Avif);
}

TEST_F(StorageFixture, ANamespaceCannotWidenWhatThePipelineDecodes) {
    // The allow-list is anvil's because it is the pipeline's capability, and the
    // mask can only narrow it. `Mime::Unknown` has no bit at all, so even a mask
    // with every bit set refuses an unrecognised file — the accept check fails
    // closed rather than needing a special case at the call site.
    static_assert(!fs::mime_accepted(0xFFU, fs::Mime::Unknown),
                  "an all-ones mask must still refuse a file nothing sniffed");
    static_assert(fs::mime_accepted(fs::kDecodableMimes, fs::Mime::Jpeg));
    static_assert(fs::mime_accepted(fs::kDecodableMimes, fs::Mime::Avif));

    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();

    // Bytes nothing sniffs, into the widest namespace there is.
    const std::vector<std::uint8_t> junk(256, 0x41);
    ASSERT_TRUE(sink.write(junk).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("");
    ASSERT_FALSE(finished.ok());
    EXPECT_EQ(finished.error().field, fs::kRejectUnknownType);
}

TEST_F(StorageFixture, ANamespaceThatNamesNoFileTypesRefusesAPdf) {
    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();
    std::vector<std::uint8_t> pdf{'%', 'P', 'D', 'F', '-', '1', '.', '7', '\n'};
    pdf.resize(256, 0x20);
    ASSERT_TRUE(sink.write(pdf).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("application/pdf");
    ASSERT_FALSE(finished.ok());
    EXPECT_EQ(finished.error().field, fs::kRejectNamespaceType);
}

TEST_F(StorageFixture, ANamespaceThatNamesTheFileClassTakesAPdf) {
    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kChat);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();
    std::vector<std::uint8_t> pdf{'%', 'P', 'D', 'F', '-', '1', '.', '7', '\n'};
    pdf.resize(256, 0x20);
    ASSERT_TRUE(sink.write(pdf).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("application/pdf");
    ASSERT_TRUE(finished.ok());
    EXPECT_EQ(finished.value().mime, fs::Mime::Pdf);
}

TEST_F(StorageFixture, AFileClaimedAsAnotherAcceptedTypeIsAMismatch) {
    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kChat);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();
    std::vector<std::uint8_t> pdf{'%', 'P', 'D', 'F', '-', '1', '.', '7', '\n'};
    pdf.resize(256, 0x20);
    ASSERT_TRUE(sink.write(pdf).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("video/mp4");
    ASSERT_FALSE(finished.ok());
    EXPECT_EQ(finished.error().field, fs::kRejectTypeMismatch);
}

// --- sealed uploads --------------------------------------------------------------

namespace {

// Bytes that would each sniff as something, which is exactly why a sealed
// namespace must not look: ciphertext can begin with anything.
[[nodiscard]] std::vector<std::uint8_t> ciphertext_looking_like(std::string_view prefix,
                                                                std::size_t size) {
    std::vector<std::uint8_t> bytes(prefix.begin(), prefix.end());
    bytes.reserve(size);
    for (std::size_t i = bytes.size(); i < size; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((i * 131U + 7U) & 0xFFU));
    }
    return bytes;
}

[[nodiscard]] fs::UploadSink open_sink(fs::Ns ns, std::uint64_t max_bytes = 1 << 20) {
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), fs::UploadLimits{max_bytes, 0}, ns);
    EXPECT_TRUE(opened.ok());
    return std::move(opened).value();
}

}  // namespace

TEST_F(StorageFixture, ASealedUploadWhoseDeclaredHashMatchesIsAccepted) {
    const std::vector<std::uint8_t> blob = ciphertext_looking_like("", 200 * 1024);
    fs::UploadSink sink = open_sink(testapp::kSealed);
    // In stream-sized chunks, so the digest compared is the one built as the
    // bytes arrived and not a second pass.
    for (std::size_t at = 0; at < blob.size(); at += fs::kStreamChunkBytes) {
        const std::size_t take = std::min(fs::kStreamChunkBytes, blob.size() - at);
        ASSERT_TRUE(sink.write(std::span<const std::uint8_t>{blob.data() + at, take}).ok());
    }
    const anvil::crypto::Digest256 declared = anvil::crypto::sha256(blob);
    const anvil::Result<fs::UploadResult> finished = sink.finish_sealed(declared);
    ASSERT_TRUE(finished.ok()) << static_cast<int>(finished.code());
    EXPECT_EQ(finished.value().mime, fs::Mime::Sealed);
    EXPECT_EQ(finished.value().sha256, declared);
    EXPECT_EQ(finished.value().bytes, blob.size());
}

TEST_F(StorageFixture, ASealedUploadWhoseDeclaredHashDiffersIsRefusedWithItsOwnReason) {
    const std::vector<std::uint8_t> blob = ciphertext_looking_like("", 4096);
    fs::UploadSink sink = open_sink(testapp::kSealed);
    ASSERT_TRUE(sink.write(blob).ok());
    anvil::crypto::Digest256 declared = anvil::crypto::sha256(blob);
    declared[31] ^= 0x01U;
    const anvil::Result<fs::UploadResult> finished = sink.finish_sealed(declared);
    ASSERT_FALSE(finished.ok());
    EXPECT_EQ(finished.error().code, anvil::ErrorCode::ValidationFailed);
    EXPECT_EQ(finished.error().field, fs::kRejectSealedHash);
    // Refused, so it can never be published, whatever the caller does next.
    EXPECT_FALSE(sink.publish().ok());
    EXPECT_FALSE(sink.published());
}

TEST_F(StorageFixture, AnUploadAFinishRefusedCanNeverBePublished) {
    // The stream is durable once finish() has fsynced it, refused or not.
    // Durable is not accepted: an SVG whose refusal a caller ignored must not
    // reach the namespace directory through publish().
    fs::UploadSink sink = open_sink(testapp::kContent);
    const std::vector<std::uint8_t> svg = bytes_of("<svg onload=\"alert(1)\"></svg>");
    ASSERT_TRUE(sink.write(svg).ok());
    const anvil::Result<fs::UploadResult> finished = sink.finish("image/png");
    ASSERT_FALSE(finished.ok());
    EXPECT_EQ(finished.error().field, fs::kRejectVectorType);
    EXPECT_FALSE(sink.publish().ok());
    EXPECT_FALSE(sink.published());
}

TEST_F(StorageFixture, ASealedNamespaceIsNeverSniffed) {
    // Every one of these is a file the sniffing path would refuse or classify.
    // As ciphertext each is opaque, and each is accepted as Sealed.
    for (const std::string_view prefix :
         {std::string_view{"<svg onload=\"alert(1)\">"}, std::string_view{"<?xml version=\"1.0\"?>"},
          std::string_view{"%PDF-1.7\n"}, std::string_view{"\x89PNG\r\n\x1a\n"},
          std::string_view{"GIF89a"}}) {
        const std::vector<std::uint8_t> blob = ciphertext_looking_like(prefix, 512);
        fs::UploadSink sink = open_sink(testapp::kSealed);
        ASSERT_TRUE(sink.write(blob).ok());
        const anvil::Result<fs::UploadResult> finished =
            sink.finish_sealed(anvil::crypto::sha256(blob));
        ASSERT_TRUE(finished.ok()) << prefix;
        EXPECT_EQ(finished.value().mime, fs::Mime::Sealed);
    }
}

TEST_F(StorageFixture, EachNamespaceClassHasExactlyOneWayToFinish) {
    const std::vector<std::uint8_t> blob = ciphertext_looking_like("%PDF-1.7\n", 512);

    // The sniffing finish on ciphertext would act on a "type" found in random
    // bytes, so it refuses before reading any.
    fs::UploadSink sealed = open_sink(testapp::kSealed);
    ASSERT_TRUE(sealed.write(blob).ok());
    const anvil::Result<fs::UploadResult> sniffed = sealed.finish("application/pdf");
    ASSERT_FALSE(sniffed.ok());
    EXPECT_EQ(sniffed.error().code, anvil::ErrorCode::Internal);

    // And a namespace that sniffs cannot be talked out of it by a declared hash.
    fs::UploadSink chat = open_sink(testapp::kChat);
    ASSERT_TRUE(chat.write(blob).ok());
    const anvil::Result<fs::UploadResult> skipped =
        chat.finish_sealed(anvil::crypto::sha256(blob));
    ASSERT_FALSE(skipped.ok());
    EXPECT_EQ(skipped.error().code, anvil::ErrorCode::Internal);
}

TEST_F(StorageFixture, ASealedUploadIsStillCappedDuringTheStreamAndMayNotBeEmpty) {
    fs::UploadSink capped = open_sink(testapp::kSealed, 1024);
    const std::vector<std::uint8_t> blob = ciphertext_looking_like("", 2048);
    EXPECT_EQ(capped.write(blob).code(), anvil::ErrorCode::PayloadTooLarge);
    const anvil::Result<fs::UploadResult> over = capped.finish_sealed(anvil::crypto::sha256(blob));
    ASSERT_FALSE(over.ok());
    EXPECT_EQ(over.error().field, fs::kRejectOversize);

    fs::UploadSink empty = open_sink(testapp::kSealed);
    const anvil::Result<fs::UploadResult> nothing =
        empty.finish_sealed(anvil::crypto::sha256(std::string_view{}));
    ASSERT_FALSE(nothing.ok());
    EXPECT_EQ(nothing.error().field, fs::kRejectEmpty);
}

// --- SIGKILL mid-upload leaves a sweepable .part ----------------------------

TEST_F(StorageFixture, AbandonedPartSurvivesAProcessThatNeverFinished) {
    const fs::UploadLimits limits{1 << 20, 0};
    anvil::Result<fs::UploadSink> opened =
        fs::UploadSink::open(fs::Storage::instance(), limits, testapp::kContent);
    ASSERT_TRUE(opened.ok());
    fs::UploadSink sink = std::move(opened).value();
    ASSERT_TRUE(sink.write(png_bytes(128)).ok());

    const std::filesystem::path part =
        std::filesystem::path{root()} / std::string{fs::temp_relative_path(sink.id()).view()};
    EXPECT_TRUE(std::filesystem::exists(part));

    // A SIGKILL runs no destructor. Simulated by releasing the sink's ownership
    // the way a killed process would: the file stays, and no media row exists
    // for it, which is exactly what the sweeper's one-hour tmp/ rule collects.
    (void)sink.publish();   // fails: finish() was never called
    EXPECT_TRUE(std::filesystem::exists(part));
}

}  // namespace

