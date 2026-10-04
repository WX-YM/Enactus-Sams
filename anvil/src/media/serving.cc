#include "anvil/media/serving.h"

#include <optional>

#include <drogon/HttpTypes.h>

#include "anvil/accesscontrol/stealth.h"

namespace anvil::media {

std::string encode_accel_path(std::string_view path) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    // Worst case is three bytes out per byte in; reserving up front keeps this
    // to one allocation on a path that runs once per media request.
    out.reserve(path.size() * 3);
    for (const char raw : path) {
        const auto byte = static_cast<std::uint8_t>(raw);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                                byte == '.' || byte == '~' || byte == '/';
        if (unreserved) {
            out.push_back(raw);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4U]);
            out.push_back(kHex[byte & 0x0FU]);
        }
    }
    return out;
}

namespace {

// The format a client that asked for `format` is GUARANTEED to also decode, or
// nullopt when there is none.
//
// Directional, and the direction is the whole of it. Every client advertising
// AVIF decodes WebP — WebP predates it by years and is universal — so an AVIF
// request may be answered with a WebP variant. The reverse is NOT true:
// negotiate_format answers Webp precisely for the clients that did not say they
// decode AVIF, so handing one an AVIF is a broken image on a public page. A
// symmetric "try the other format" would have been the obvious spelling and is
// wrong in exactly one direction, which is the direction that reaches the
// oldest clients.
[[nodiscard]] constexpr std::optional<fs::Format> degrade_to(fs::Format format) noexcept {
    if (format == fs::Format::Avif) { return fs::Format::Webp; }
    return std::nullopt;
}

}  // namespace

fs::Format negotiate_format(std::string_view accept) noexcept {
    // WebP is the answer to everything else, including an absent header and a
    // bare `*/*`: every browser in service decodes it, and the fallback that
    // matters is the width ladder rather than the format.
    return accept.find("image/avif") != std::string_view::npos ? fs::Format::Avif
                                                               : fs::Format::Webp;
}

fs::VariantKey resolve_role(const std::vector<images::VariantRecord>& variants,
                            std::uint16_t role_width, fs::Format format) noexcept {
    // Four candidates, in preference order, computed in ONE pass. The list is
    // short and a second pass over it would be a second place to get the
    // predicate wrong.
    std::uint16_t fit_negotiated = 0;   // <= role, negotiated format, WIDEST
    std::uint16_t fit_degraded = 0;     // <= role, degraded format, WIDEST
    std::uint16_t over_negotiated = 0;  // >  role, negotiated format, NARROWEST
    std::uint16_t over_degraded = 0;    // >  role, degraded format, NARROWEST

    const std::optional<fs::Format> degraded = degrade_to(format);

    for (const images::VariantRecord& variant : variants) {
        const bool negotiated = variant.format == format;
        if (!negotiated && (!degraded.has_value() || variant.format != *degraded)) {
            // A format this client never said it could decode. Skipped, not
            // ranked: see degrade_to.
            continue;
        }
        if (variant.width == 0) { continue; }

        if (variant.width <= role_width) {
            std::uint16_t& best = negotiated ? fit_negotiated : fit_degraded;
            if (variant.width > best) { best = variant.width; }
        } else {
            std::uint16_t& best = negotiated ? over_negotiated : over_degraded;
            if (best == 0 || variant.width < best) { best = variant.width; }
        }
    }

    if (fit_negotiated != 0) { return fs::VariantKey{fit_negotiated, format}; }
    // A variant that FITS in a format the client can decode beats one that does
    // not fit in the format it asked for: fewer bytes and the right dimensions.
    if (fit_degraded != 0) { return fs::VariantKey{fit_degraded, *degraded}; }
    if (over_negotiated != 0) { return fs::VariantKey{over_negotiated, format}; }
    if (over_degraded != 0) { return fs::VariantKey{over_degraded, *degraded}; }

    // The MASTER, and only now: when the object has no variant this client can
    // be served at all. It is the normalised original — the widest and least
    // compressed thing on disk — so reaching it is the worst outcome available
    // rather than a neutral default.
    return fs::kMasterVariant;
}

namespace {

[[nodiscard]] drogon::HttpResponsePtr redirect_to(fs::Ns ns, const Uuid& id, fs::VariantKey key,
                                                  fs::Mime mime);

}  // namespace

drogon::HttpResponsePtr accel_redirect_response(fs::Ns ns, const Uuid& id, fs::VariantKey key,
                                                fs::Mime mime) {
    // Byte-identical to a missing object, because to a caller holding only an
    // id that is exactly what a private object is.
    if (ns.visibility() == fs::Visibility::Private) { return accesscontrol::not_found_response(); }
    return redirect_to(ns, id, key, mime);
}

drogon::HttpResponsePtr accel_redirect_response(const MediaGrant& grant, fs::VariantKey key,
                                                fs::Mime mime) {
    return redirect_to(grant.ns(), grant.id(), key, mime);
}

namespace {

drogon::HttpResponsePtr redirect_to(fs::Ns ns, const Uuid& id, fs::VariantKey requested_key,
                                    fs::Mime stored_mime) {
    // Forced rather than trusted for a sealed namespace (serving.h): the bytes
    // there are attacker-chosen by definition, so the response is decided by
    // the namespace and not by a row that could be wrong.
    const bool sealed = ns.sealed();
    const fs::Mime mime = sealed ? fs::Mime::Sealed : stored_mime;
    const fs::VariantKey key = sealed ? fs::kMasterVariant : requested_key;

    // Resolved against what the row says was WRITTEN, so the redirect can only
    // ever name a file Nginx will find. A redirect into a path that does not
    // exist turns a clean 404 into a failure inside the proxy.
    const fs::RelPath relative = fs::media_relative_path(ns, id, key);
    std::string target{kAccelPrefix};
    target.append(relative.view());

    const std::string_view content_type =
        key.is_master() ? fs::mime_type(mime) : fs::format_mime(key.format);

    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k200OK);
    // EMPTY body: Nginx replaces it with the file. Every byte of the object is
    // copied disk-to-socket inside the kernel and never enters this heap.
    response->setBody("");
    response->addHeader("X-Accel-Redirect", encode_accel_path(target));
    // From the STORED enum, never from the upload. A file cannot be allowed to
    // choose how a browser interprets it.
    response->setContentTypeString(std::string{content_type});
    // Without nosniff, a browser may sniff a crafted file as HTML and execute it
    // on the origin serving media — which is the one origin where an executable
    // response is most likely to be reachable without a session in front of it.
    response->addHeader("X-Content-Type-Options", "nosniff");
    response->addHeader("Cache-Control", std::string{kMediaCacheControl});
    if (fs::mime_class(mime) == fs::MimeClass::Image) {
        // The body depends on the request's Accept and on nothing else a cache
        // can see. Without this a shared cache would serve one client's AVIF to
        // the next, which may not decode one.
        response->addHeader("Vary", "Accept");
        return response;
    }
    // A stored file is the bytes a user sent, not pixels this process wrote, so
    // the response is what makes it safe to serve. The sandbox gives a document
    // opened from this URL no script, no origin and no forms, whatever the bytes
    // turn out to be; the disposition keeps a PDF out of the browser's own
    // viewer entirely (fs/sniff.h). Nothing varies with Accept: a file has one
    // representation. A sealed blob drops even `media-src`: nothing plays it.
    response->addHeader("Content-Security-Policy",
                        std::string{fs::mime_class(mime) == fs::MimeClass::Sealed
                                        ? kSealedContentSecurityPolicy
                                        : kFileContentSecurityPolicy});
    response->addHeader("Content-Disposition",
                        fs::disposition(mime) == fs::Disposition::Attachment ? "attachment"
                                                                             : "inline");
    return response;
}

}  // namespace

}  // namespace anvil::media
