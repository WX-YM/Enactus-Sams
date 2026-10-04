#pragma once

// Content type from magic bytes, never from the client (docs/07-filesystem.md §5).
//
// The client's Content-Type header and the filename extension are both
// attacker-controlled and both advisory. A PHP or HTML file renamed .jpg is the
// oldest upload attack there is, and it works whenever the server believes
// either of them.
//
// The allow-list is closed, and it is two lists with different treatment:
//
//   IMAGE  JPEG, PNG, WebP, AVIF. Decoded, normalised and re-encoded by the
//          pipeline, so what is served is pixels this process wrote.
//   FILE   MP4, WebM, Ogg Opus, M4A, PDF. Stored and served as the bytes that
//          arrived and NEVER decoded here: a container parser per format on the
//          upload path is a far larger surface than the four image decoders,
//          and transcoding video is minutes of CPU per upload
//          (docs/22-chat.md §6.4). What keeps these safe to serve is the
//          response — the stored type, nosniff, a sandbox and a disposition —
//          and not the bytes.
//
// And one class that is not on the list at all, because no bytes select it:
//
//   SEALED  Ciphertext (docs/22-chat.md §6.4, §7.7). Any bytes, never sniffed,
//           never decoded, served as an opaque attachment. A namespace CHOOSES
//           it (fs/namespace_spec.h), and sniff() never returns it: the server
//           cannot tell ciphertext from anything else of the same length, and
//           sniffing random bytes finds a "type" one time in a few hundred and
//           would act on it.
//
// Anything else — including a format this build could technically decode — is
// rejected.
//
// SVG is rejected EXPLICITLY rather than by falling off the allow-list, because
// the rejection needs to be auditable. It is XML, it executes script, and
// rasterising it pulls in external-entity and network-fetch behaviour. There is
// no safe way to accept user-uploaded SVG on an origin that matters; vector
// assets are committed by developers instead.
//
// Everything here is constexpr and works on a fixed 64-byte prefix, so sniffing
// costs a handful of compares against a stack array and never sees the rest of
// the stream.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace anvil::fs {

// Stored in `media.mime` as int32 and therefore permanent. NEVER renumber.
enum class Mime : std::uint8_t {
    Unknown = 0,
    Jpeg = 1,
    Png = 2,
    Webp = 3,
    Avif = 4,
    Mp4 = 5,
    Webm = 6,
    OggOpus = 7,
    M4a = 8,
    Pdf = 9,
    // Not a format: bytes this server must never interpret. Never produced by
    // sniff() or mime_from_claim(); only UploadSink::finish_sealed() assigns it.
    Sealed = 10,
};

inline constexpr Mime kMaxMime = Mime::Sealed;

enum class MimeClass : std::uint8_t { Image, File, Sealed };

// Which treatment a type gets. A function of the enum rather than a second
// table, so a new value cannot be added to one list and forgotten in the other:
// the switch below has no default, and a missing case is a warning.
[[nodiscard]] constexpr MimeClass mime_class(Mime mime) noexcept {
    switch (mime) {
        case Mime::Jpeg:
        case Mime::Png:
        case Mime::Webp:
        case Mime::Avif:
        case Mime::Unknown: return MimeClass::Image;
        case Mime::Mp4:
        case Mime::Webm:
        case Mime::OggOpus:
        case Mime::M4a:
        case Mime::Pdf: return MimeClass::File;
        case Mime::Sealed: break;
    }
    return MimeClass::Sealed;
}

// How a browser is told to treat a served object.
//
// Images, audio and video are `inline`: a picture in a page and a player for a
// voice note are the point. A PDF is ALWAYS an attachment, because a PDF viewer
// is a document engine with script in it, and opening one inside the browser on
// the media origin hands that engine a URL of ours. A SEALED object is ALWAYS an
// attachment too: its bytes are attacker-chosen by definition, and nothing the
// browser could render them as is something this server should be serving.
enum class Disposition : std::uint8_t { Inline, Attachment };

[[nodiscard]] constexpr Disposition disposition(Mime mime) noexcept {
    return mime == Mime::Pdf || mime == Mime::Sealed ? Disposition::Attachment
                                                     : Disposition::Inline;
}

// Enough for every signature below: AVIF needs the brand at offset 8..12, and
// the SVG probe needs room to skip a BOM and leading whitespace.
inline constexpr std::size_t kSniffBytes = 64;

using SniffBuffer = std::array<std::uint8_t, kSniffBytes>;

// The served Content-Type. It comes from the STORED enum, never from the
// upload, so a file cannot choose how a browser interprets it.
[[nodiscard]] constexpr std::string_view mime_type(Mime mime) noexcept {
    switch (mime) {
        case Mime::Jpeg: return "image/jpeg";
        case Mime::Png:  return "image/png";
        case Mime::Webp: return "image/webp";
        case Mime::Avif: return "image/avif";
        case Mime::Mp4:  return "video/mp4";
        case Mime::Webm: return "video/webm";
        case Mime::OggOpus: return "audio/ogg";
        case Mime::M4a:  return "audio/mp4";
        case Mime::Pdf:  return "application/pdf";
        // The one type that names no format, because the server knows none: a
        // recipient's client decrypts it and learns what it is.
        case Mime::Sealed: return "application/octet-stream";
        case Mime::Unknown: break;
    }
    // Deliberately not "application/octet-stream", which is Sealed's: an
    // Unknown mime never reaches a response, because such an upload was
    // rejected.
    return "";
}

[[nodiscard]] constexpr bool mime_from_stored(std::int32_t value, Mime& out) noexcept {
    if (value <= 0 || value > static_cast<std::int32_t>(kMaxMime)) { return false; }
    out = static_cast<Mime>(value);
    return true;
}

namespace detail {

[[nodiscard]] constexpr bool starts_with(std::span<const std::uint8_t> data,
                                         std::span<const std::uint8_t> prefix) noexcept {
    if (data.size() < prefix.size()) { return false; }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (data[i] != prefix[i]) { return false; }
    }
    return true;
}

[[nodiscard]] constexpr bool matches_at(std::span<const std::uint8_t> data, std::size_t offset,
                                        std::string_view literal) noexcept {
    if (data.size() < offset + literal.size()) { return false; }
    for (std::size_t i = 0; i < literal.size(); ++i) {
        if (data[offset + i] != static_cast<std::uint8_t>(literal[i])) { return false; }
    }
    return true;
}

// ISO-BMFF brands this build accepts. `avis` is an image SEQUENCE and is
// rejected downstream by the frame-count check, but it must sniff as AVIF
// rather than as Unknown so the rejection carries the right reason.
inline constexpr std::array<std::string_view, 3> kAvifBrands{{"avif", "avis", "av01"}};

// ISO-BMFF major brands that mean an MP4 VIDEO. Matched exactly. QuickTime's
// `qt  ` is deliberately absent: a .mov is not an MP4, browsers disagree about
// playing one, and the client is expected to record in a format the class takes
// rather than the server guessing which container it was handed. HEIF/HEIC
// brands are absent for the same reason the image list is closed.
inline constexpr std::array<std::string_view, 10> kMp4Brands{
    {"isom", "iso2", "iso4", "iso5", "iso6", "mp41", "mp42", "avc1", "dash", "M4V "}};

// ...and the two that mean AUDIO in the same container.
inline constexpr std::array<std::string_view, 2> kM4aBrands{{"M4A ", "M4B "}};

// The EBML DocType element (ID 0x4282), a one-byte size of 4, and "webm".
// Matroska shares the EBML header and differs only here, and is refused: its
// codec set is open-ended where WebM's is not.
inline constexpr std::array<std::uint8_t, 7> kWebmDocType{{0x42, 0x82, 0x84, 'w', 'e', 'b', 'm'}};

[[nodiscard]] constexpr bool contains(std::span<const std::uint8_t> data,
                                      std::span<const std::uint8_t> needle) noexcept {
    if (needle.empty() || data.size() < needle.size()) { return false; }
    for (std::size_t i = 0; i + needle.size() <= data.size(); ++i) {
        if (starts_with(data.subspan(i), needle)) { return true; }
    }
    return false;
}

// An Ogg stream whose first packet is an Opus identification header. The first
// page is a 27-byte header and a segment table whose length is byte 26, so the
// first packet starts right after that table. Ogg Vorbis and every other codec
// in an Ogg container are refused: a voice note is Opus.
[[nodiscard]] constexpr bool is_ogg_opus(std::span<const std::uint8_t> data) noexcept {
    constexpr std::size_t kSegmentCountAt = 26;
    if (!matches_at(data, 0, "OggS") || data.size() <= kSegmentCountAt) { return false; }
    const std::size_t packet = kSegmentCountAt + 1 + data[kSegmentCountAt];
    return matches_at(data, packet, "OpusHead");
}

}  // namespace detail

// Magic-byte match against the closed allow-list. `head` is the first
// kSniffBytes of the stream (or fewer, for a short file).
//
// NEVER returns Mime::Sealed. There is no signature for ciphertext, and a type
// that bytes could select would be a type an attacker selects.
[[nodiscard]] constexpr Mime sniff(std::span<const std::uint8_t> head) noexcept {
    constexpr std::array<std::uint8_t, 3> kJpeg{{0xFF, 0xD8, 0xFF}};
    constexpr std::array<std::uint8_t, 8> kPng{
        {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}};

    if (detail::starts_with(head, kJpeg)) { return Mime::Jpeg; }
    if (detail::starts_with(head, kPng)) { return Mime::Png; }
    // RIFF container with a WEBP form type. Checking only "RIFF" would accept a
    // WAV file.
    if (detail::matches_at(head, 0, "RIFF") && detail::matches_at(head, 8, "WEBP")) {
        return Mime::Webp;
    }
    if (detail::matches_at(head, 4, "ftyp")) {
        // The image brands first: AVIF and MP4 share a container, and the
        // container is not the type.
        for (const std::string_view brand : detail::kAvifBrands) {
            if (detail::matches_at(head, 8, brand)) { return Mime::Avif; }
        }
        for (const std::string_view brand : detail::kM4aBrands) {
            if (detail::matches_at(head, 8, brand)) { return Mime::M4a; }
        }
        for (const std::string_view brand : detail::kMp4Brands) {
            if (detail::matches_at(head, 8, brand)) { return Mime::Mp4; }
        }
        return Mime::Unknown;
    }
    constexpr std::array<std::uint8_t, 4> kEbml{{0x1A, 0x45, 0xDF, 0xA3}};
    if (detail::starts_with(head, kEbml)) {
        return detail::contains(head, detail::kWebmDocType) ? Mime::Webm : Mime::Unknown;
    }
    if (detail::is_ogg_opus(head)) { return Mime::OggOpus; }
    // At offset zero and nowhere else. Readers accept a PDF header up to a
    // kilobyte in, which is how a polyglot hides one behind something else; the
    // sniffer accepts only the spelling that is unambiguously a PDF.
    if (detail::matches_at(head, 0, "%PDF-")) { return Mime::Pdf; }
    return Mime::Unknown;
}

// True when the bytes look like SVG or any other XML document. Kept separate
// from sniff() so the upload path can audit "someone tried to upload an SVG" as
// the probe it is, rather than logging it as an unrecognised file.
[[nodiscard]] constexpr bool looks_like_xml(std::span<const std::uint8_t> head) noexcept {
    std::size_t i = 0;
    // A UTF-8 BOM before the root element is legal XML and would otherwise hide
    // the signature.
    if (head.size() >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) { i = 3; }
    while (i < head.size() && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' ||
                               head[i] == '\n')) {
        ++i;
    }
    const std::span<const std::uint8_t> rest = head.subspan(i);
    return detail::matches_at(rest, 0, "<?xml") || detail::matches_at(rest, 0, "<svg") ||
           detail::matches_at(rest, 0, "<!DOCTYPE") || detail::matches_at(rest, 0, "<!--");
}

// The CLAIMED type, mapped through the same closed table. Used only to compare
// against the sniffed type: a disagreement is rejected and audited, because it
// is a probe rather than a mistake. Never used to decide what a file IS.
[[nodiscard]] constexpr Mime mime_from_claim(std::string_view content_type) noexcept {
    // Parameters (";charset=…", ";boundary=…") are ignored, and so is trailing
    // whitespace: a browser sending "image/jpeg " is not making a claim about
    // anything different.
    std::size_t end = content_type.find(';');
    if (end == std::string_view::npos) { end = content_type.size(); }
    while (end > 0 && (content_type[end - 1] == ' ' || content_type[end - 1] == '\t')) { --end; }
    const std::string_view base = content_type.substr(0, end);

    if (base == "image/jpeg" || base == "image/jpg") { return Mime::Jpeg; }
    if (base == "image/png") { return Mime::Png; }
    if (base == "image/webp") { return Mime::Webp; }
    if (base == "image/avif") { return Mime::Avif; }
    if (base == "video/mp4") { return Mime::Mp4; }
    if (base == "video/webm" || base == "audio/webm") { return Mime::Webm; }
    if (base == "audio/ogg" || base == "audio/opus") { return Mime::OggOpus; }
    if (base == "audio/mp4" || base == "audio/m4a" || base == "audio/x-m4a") {
        return Mime::M4a;
    }
    if (base == "application/pdf") { return Mime::Pdf; }
    return Mime::Unknown;
}

static_assert(mime_type(Mime::Avif) == "image/avif");
static_assert(mime_from_claim("image/png; charset=binary") == Mime::Png);
static_assert(mime_class(Mime::Avif) == MimeClass::Image);
static_assert(mime_class(Mime::Pdf) == MimeClass::File);
static_assert(disposition(Mime::Pdf) == Disposition::Attachment);
static_assert(mime_class(Mime::Sealed) == MimeClass::Sealed);
static_assert(disposition(Mime::Sealed) == Disposition::Attachment);
static_assert(mime_from_claim("application/octet-stream") == Mime::Unknown,
              "a claim can never make an upload Sealed; only its namespace can");

}  // namespace anvil::fs
