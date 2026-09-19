#pragma once

// Content type from magic bytes, never from the client (docs/07-filesystem.md §5).
//
// The client's Content-Type header and the filename extension are both
// attacker-controlled and both advisory. A PHP or HTML file renamed .jpg is the
// oldest upload attack there is, and it works whenever the server believes
// either of them.
//
// The allow-list is closed: JPEG, PNG, WebP, AVIF. Anything else — including a
// format this build could technically decode — is rejected.
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
};

inline constexpr Mime kMaxMime = Mime::Avif;

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
        case Mime::Unknown: break;
    }
    // Deliberately not "application/octet-stream": an Unknown mime never
    // reaches a response, because such an upload was rejected.
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

}  // namespace detail

// Magic-byte match against the closed allow-list. `head` is the first
// kSniffBytes of the stream (or fewer, for a short file).
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
        for (const std::string_view brand : detail::kAvifBrands) {
            if (detail::matches_at(head, 8, brand)) { return Mime::Avif; }
        }
    }
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
    return Mime::Unknown;
}

static_assert(mime_type(Mime::Avif) == "image/avif");
static_assert(mime_from_claim("image/png; charset=binary") == Mime::Png);

}  // namespace anvil::fs
