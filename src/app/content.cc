#include "app/content.h"

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "anvil/accesscontrol/stealth.h"
#include "anvil/core/uuid.h"
#include "anvil/db/codec.h"
#include "anvil/entries/payload.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/paths.h"
#include "anvil/fs/upload.h"
#include "anvil/http/json_writer.h"
#include "anvil/images/probe.h"
#include "anvil/input/schema.h"
#include "anvil/media/pipeline.h"
#include "anvil/media/serving.h"
#include "anvil/sections/payload.h"
#include "app/services.h"
#include "entries.h"
#include "rate_limits.h"
#include "sections.h"

namespace enactus {

namespace {

namespace sec = anvil::sections;
namespace ent = anvil::entries;
namespace input = anvil::input;
using anvil::ErrorCode;
using anvil::Uuid;

constexpr input::TextRules kCaptionRules{0, 200, anvil::i18n::TextClass::Prose, false};

// Section keys arrive in the URL; only a registry entry is ever acted on.
[[nodiscard]] const sec::SectionSpec* section_of(std::string_view key) noexcept {
    return sec::find_section(kSections, key);
}

[[nodiscard]] bool spend(const http::HttpRequestPtr& req, const http::Responder& respond,
                         const anvil::UserContext& ctx, std::string_view bucket) {
    const anvil::http::RateLimitRule& rule = rate_rule(bucket);
    const auto verdict =
        services().limiter.check_account(anvil::uuid::to_string(ctx.user_id), rule);
    if (!verdict.allowed) {
        respond(http::rate_limited(req, verdict, rule));
        return false;
    }
    return true;
}

void append_gallery_item(std::string& out, const ent::EntryDocument& entry) {
    const ent::EntryContent* copy = entry.working();
    out += '{';
    http::append_key(out, "id");
    anvil::http::append_json_uuid(out, entry.id);
    out += ',';
    http::append_key(out, "version");
    anvil::http::append_json_int(out, entry.version);
    const sec::SectionField* caption = copy != nullptr ? copy->content.find("caption") : nullptr;
    http::append_string_field(out, "caption", caption != nullptr ? caption->value.primary() : "");
    out += ',';
    http::append_key(out, "image");
    if (copy != nullptr) {
        append_image(out, copy->content, "photo");
    } else {
        out += "null";
    }
    out += '}';
}

}  // namespace

const ent::KindSpec* gallery_kind(std::string_view short_name) noexcept {
    for (const std::string_view key : kGalleryKinds) {
        if (key.substr(key.find('.') + 1) == short_name) { return ent::find_kind(kKinds, key); }
    }
    return nullptr;
}

void append_image(std::string& out, const sec::SectionContent& content, std::string_view slot) {
    const sec::SectionImage* image = content.find_image(slot);
    if (image == nullptr) {
        out += "null";
        return;
    }
    const std::string id = anvil::uuid::to_string(image->media_id);
    out += '{';
    http::append_key(out, "id");
    anvil::http::append_json_string(out, id);
    out += ',';
    http::append_key(out, "src");
    anvil::http::append_json_string(out, std::string{kImageBase} + "/" + id);
    out += '}';
}

void append_data(std::string& out, const sec::SectionSpec& shape, const sec::SectionContent& content) {
    out += '{';
    bool first = true;
    for (const sec::FieldSpec& field : shape.fields) {
        const sec::SectionField* stored = content.find(field.key);
        if (stored == nullptr) { continue; }
        if (!first) { out += ','; }
        first = false;
        http::append_key(out, field.key);
        switch (field.type) {
            case sec::FieldType::Bool: out += stored->value.boolean ? "true" : "false"; break;
            case sec::FieldType::Number: anvil::http::append_json_int(out, stored->value.number); break;
            default: anvil::http::append_json_string(out, stored->value.primary()); break;
        }
    }
    out += '}';
}

namespace routes {

// The editor's view: the registry (so the editor is generated from the table
// rather than written twice, anvil docs/12 §7) and every section's published
// content with its version.
void sections_list(const http::HttpRequestPtr& req, http::Responder&& respond) {
    http::db_or_shed(req, respond, [req, respond](mongocxx::client& client) {
        std::string body{R"({"registry":)"};
        body += sec::serialize_registry(kSections);
        body += R"(,"sections":[)";
        bool first = true;
        for (const sec::SectionSpec& spec : kSections) {
            const auto document =
                services().sections.read_document(client, spec, sec::SectionState::Published);
            if (!document) {
                respond(http::failure(req, document.error()));
                return;
            }
            if (!document.value().has_value()) { continue; }
            if (!first) { body += ','; }
            first = false;
            body += '{';
            http::append_key(body, "key");
            anvil::http::append_json_string(body, spec.key);
            body += ',';
            http::append_key(body, "version");
            anvil::http::append_json_int(body, document.value()->version);
            body += ',';
            http::append_key(body, "data");
            append_data(body, spec, document.value()->content);
            body += ',';
            http::append_key(body, "images");
            body += '{';
            bool first_image = true;
            for (const sec::ImageSpec& image : spec.images) {
                if (!first_image) { body += ','; }
                first_image = false;
                http::append_key(body, image.slot);
                append_image(body, document.value()->content, image.slot);
            }
            body += "}}";
        }
        body += "]}";
        respond(http::json(200, std::move(body)));
    });
}

// Publishing a section: the patch is bound against the registry (an unknown key
// is refused, never dropped), merged onto the published copy by the service
// under the version the editor read, and checked for required fields against
// the merged result.
void sections_publish(const http::HttpRequestPtr& req, http::Responder&& respond,
                      const std::string& key) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const sec::SectionSpec* spec = section_of(key);
    if (ctx == nullptr || spec == nullptr) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }

    const auto body = std::make_shared<http::Body>(req);
    if (!body->ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    input::ObjectBinder bind{body->root()};
    std::int64_t version = 0;
    if (const input::Reason reason = bind.integer("version", 1, INT64_MAX, version);
        !input::is_ok(reason)) {
        respond(http::invalid(req, "version", reason));
        return;
    }
    const input::JsonValue* data = bind.object("data");
    const input::JsonValue* images = bind.object("images");
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    auto patch = std::make_shared<sec::SectionContent>();
    const sec::BindPolicy policy{services().config.site_origin};
    if (auto bad = sec::bind_data(*spec, data, policy, *patch); bad.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*bad, 1}));
        return;
    }
    if (auto bad = sec::bind_images(*spec, images, *patch); bad.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*bad, 1}));
        return;
    }

    http::db_or_shed(req, respond, [req, respond, ctx, spec, version, patch](mongocxx::client& client) {
        if (!spend(req, respond, *ctx, "staff-write")) { return; }
        const auto written = services().sections.write(client, *spec, sec::SectionState::Published,
                                                       version, *patch, ctx->user_id);
        if (!written) {
            respond(http::failure(req, written.error()));
            return;
        }
        http::audit(req, Action::SectionPublished, std::nullopt);
        std::string out{"{"};
        http::append_key(out, "version");
        anvil::http::append_json_int(out, written.value().version);
        out += '}';
        respond(http::json(200, std::move(out)));
    });
}

void gallery_list(const http::HttpRequestPtr& req, http::Responder&& respond,
                  const std::string& kind_name) {
    const ent::KindSpec* kind = gallery_kind(kind_name);
    if (kind == nullptr) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    http::db_or_shed(req, respond, [req, respond, kind](mongocxx::client& client) {
        ent::EntryQuery query{};
        query.limit = ent::kMaxPage;
        query.stage = ent::Stage::Working;
        const auto page = services().entries.list(client, *kind, query);
        if (!page) {
            respond(http::failure(req, page.error()));
            return;
        }
        std::string body{R"({"items":[)"};
        for (std::size_t i = 0; i < page.value().entries.size(); ++i) {
            if (i != 0) { body += ','; }
            append_gallery_item(body, page.value().entries[i]);
        }
        body += "]}";
        respond(http::json(200, std::move(body)));
    });
}

// Adding a photo: the image was uploaded first (media_upload), and the entry
// references it by id. The entry service verifies the media exists, is in the
// site namespace and meets the slot's minimum size, inside the transaction that
// takes its reference.
void gallery_add(const http::HttpRequestPtr& req, http::Responder&& respond,
                 const std::string& kind_name) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const ent::KindSpec* kind = gallery_kind(kind_name);
    if (ctx == nullptr || kind == nullptr) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    http::Body body{req};
    if (!body.ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    input::ObjectBinder bind{body.root()};
    Uuid media{};
    std::optional<std::string_view> caption;
    if (const input::Reason reason = bind.uuid("media", media); !input::is_ok(reason)) {
        respond(http::invalid(req, "media", reason));
        return;
    }
    if (const input::Reason reason = bind.optional_text("caption", kCaptionRules, caption);
        !input::is_ok(reason)) {
        respond(http::invalid(req, "caption", reason));
        return;
    }
    if (const auto unknown = bind.finish(); unknown.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*unknown, 1}));
        return;
    }
    auto entry = std::make_shared<ent::NewEntry>();
    entry->content.set_image("photo", media);
    sec::SectionValue text{};
    text.primary() = caption.has_value() ? std::string{*caption} : std::string{};
    entry->content.set("caption", std::move(text));

    http::db_or_shed(req, respond, [req, respond, ctx, kind, entry](mongocxx::client& client) {
        if (!spend(req, respond, *ctx, "staff-write")) { return; }
        const auto created = services().entries.create(client, *kind, *entry, ctx->user_id);
        if (!created) {
            respond(http::failure(req, created.error()));
            return;
        }
        http::audit(req, Action::GalleryChanged, created.value().id);
        std::string out{"{"};
        http::append_key(out, "id");
        anvil::http::append_json_uuid(out, created.value().id);
        out += '}';
        respond(http::json(201, std::move(out)));
    });
}

void gallery_remove(const http::HttpRequestPtr& req, http::Responder&& respond,
                    const std::string& kind_name, const std::string& id_text) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const ent::KindSpec* kind = gallery_kind(kind_name);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    if (ctx == nullptr || kind == nullptr || !id.has_value()) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    const Uuid target = *id;
    http::db_or_shed(req, respond, [req, respond, ctx, kind, target](mongocxx::client& client) {
        if (!spend(req, respond, *ctx, "staff-write")) { return; }
        const auto found = services().entries.find(client, *kind, target, ent::Stage::Working);
        if (!found) {
            respond(http::failure(req, found.error()));
            return;
        }
        if (!found.value().has_value()) {
            respond(http::failure(req, ErrorCode::NotFound));
            return;
        }
        // Versioned on the copy just read; the media reference is released in
        // the same transaction as the delete.
        const anvil::Status removed =
            services().entries.remove(client, *kind, target, found.value()->version);
        if (!removed) {
            respond(http::failure(req, removed.error()));
            return;
        }
        http::audit(req, Action::GalleryChanged, target);
        respond(http::json(200, R"({"removed":true})"));
    });
}

void gallery_reorder(const http::HttpRequestPtr& req, http::Responder&& respond,
                     const std::string& kind_name) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    const ent::KindSpec* kind = gallery_kind(kind_name);
    if (ctx == nullptr || kind == nullptr) {
        respond(http::failure(req, ErrorCode::NotFound));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    http::Body body{req};
    if (!body.ok()) {
        respond(http::failure(req, ErrorCode::ValidationFailed));
        return;
    }
    auto order = std::make_shared<std::vector<Uuid>>();
    if (auto bad = ent::bind_order(body.root().find("order"), kind->capacity, *order); bad.has_value()) {
        respond(http::failure(req, ErrorCode::ValidationFailed, std::span{&*bad, 1}));
        return;
    }
    http::db_or_shed(req, respond, [req, respond, ctx, kind, order](mongocxx::client& client) {
        if (!spend(req, respond, *ctx, "staff-write")) { return; }
        const anvil::Status reordered = services().entries.reorder(client, *kind, std::nullopt, *order);
        if (!reordered) {
            respond(http::failure(req, reordered.error()));
            return;
        }
        http::audit(req, Action::GalleryChanged, std::nullopt);
        respond(http::json(200, R"({"reordered":true})"));
    });
}

// An image upload, in anvil's order (docs/07-filesystem.md §4): refuse on size
// before touching disk, stream into a temp file while hashing, sniff the type
// from the bytes, look for an identical object, and only then decode,
// normalise, strip and derive variants on cpu_pool. The row is inserted last.
void media_upload(const http::HttpRequestPtr& req, http::Responder&& respond) {
    const std::shared_ptr<const anvil::UserContext> ctx = http::context(req);
    if (ctx == nullptr) {
        respond(http::failure(req, ErrorCode::Unauthenticated));
        return;
    }
    if (!http::origin_ok(req, respond)) { return; }
    const std::string_view bytes = req->body();
    if (bytes.empty()) {
        respond(http::invalid(req, "file", input::Reason::Required));
        return;
    }
    if (bytes.size() > anvil::images::kMaxBytes) {
        respond(http::failure(req, ErrorCode::PayloadTooLarge));
        return;
    }
    const std::string claimed{req->getHeader("content-type")};
    const anvil::fs::Ns ns = site_namespace();

    // Budget first: a refused upload costs one Redis round trip, not a decode.
    const bool posted = http::on_db([req, respond, ctx, claimed, ns](mongocxx::client&) {
        if (!spend(req, respond, *ctx, "media")) { return; }
        const bool on_cpu = http::on_cpu([req, respond, ctx, claimed, ns]() {
            auto opened = anvil::fs::UploadSink::open(
                anvil::fs::Storage::instance(), anvil::fs::UploadLimits{anvil::images::kMaxBytes, 0}, ns);
            if (!opened) {
                respond(http::failure(req, opened.error()));
                return;
            }
            auto sink = std::make_shared<anvil::fs::UploadSink>(std::move(opened).value());
            const std::string_view data = req->body();
            for (std::size_t at = 0; at < data.size(); at += anvil::fs::kStreamChunkBytes) {
                const std::size_t n = std::min(anvil::fs::kStreamChunkBytes, data.size() - at);
                const anvil::Status written = sink->write(std::span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t*>(data.data() + at), n});
                if (!written) {
                    respond(http::failure(req, ErrorCode::UnsupportedMedia));
                    return;
                }
            }
            auto finished = sink->finish(claimed);
            if (!finished) {
                // A refused file (SVG, an unknown type, a type the namespace does
                // not take, or a claim that disagrees with the bytes) is 415.
                respond(http::failure(req, finished.error().code == ErrorCode::ValidationFailed
                                               ? ErrorCode::UnsupportedMedia
                                               : finished.error().code));
                return;
            }
            const anvil::fs::UploadResult upload = finished.value();

            const bool on_db = http::on_db([req, respond, ctx, ns, sink, upload](mongocxx::client& client) {
                const auto duplicate = services().media.find_duplicate(client, ns, ctx->user_id, upload.sha256);
                if (duplicate && duplicate.value().has_value()) {
                    sink->discard();
                    std::string out{"{"};
                    http::append_key(out, "id");
                    anvil::http::append_json_uuid(out, duplicate.value()->id);
                    out += '}';
                    respond(http::json(200, std::move(out)));
                    return;
                }
                const bool decoding = http::on_cpu([req, respond, ctx, ns, sink, upload]() {
                    auto processed = anvil::media::process(ns, upload);
                    if (!processed) {
                        respond(http::failure(req, processed.error().code == ErrorCode::ValidationFailed
                                                       ? ErrorCode::UnsupportedMedia
                                                       : processed.error().code));
                        return;
                    }
                    auto media = std::make_shared<anvil::media::ProcessedMedia>(std::move(processed).value());
                    const bool recording = http::on_db([req, respond, ctx, ns, sink, upload, media](mongocxx::client& db) {
                        const anvil::Status recorded = services().media.record(
                            db, ns, ctx->user_id, *media, upload.sha256, std::nullopt);
                        if (!recorded) {
                            respond(http::failure(req, recorded.error()));
                            return;
                        }
                        http::audit(req, Action::MediaUploaded, media->id);
                        std::string out{"{"};
                        http::append_key(out, "id");
                        anvil::http::append_json_uuid(out, media->id);
                        out += ',';
                        http::append_key(out, "width");
                        anvil::http::append_json_int(out, media->width);
                        out += ',';
                        http::append_key(out, "height");
                        anvil::http::append_json_int(out, media->height);
                        out += '}';
                        respond(http::json(201, std::move(out)));
                    });
                    if (!recording) { respond(http::shed(req)); }
                });
                if (!decoding) { respond(http::shed(req)); }
            });
            if (!on_db) { respond(http::shed(req)); }
        });
        if (!on_cpu) { respond(http::shed(req)); }
    });
    if (!posted) { respond(http::shed(req)); }
}

// Every stored image is public website furniture, so this route is Public; the
// bytes never pass through this process — Nginx serves them from the internal
// location this response redirects to (anvil media/serving.h).
void media_object(const http::HttpRequestPtr& req, http::Responder&& respond,
                  const std::string& ns_text, const std::string& id_text,
                  const std::string& role_text) {
    const std::optional<anvil::fs::Ns> ns = anvil::fs::Ns::from_dir(ns_text);
    const std::optional<Uuid> id = http::uuid_param(id_text);
    anvil::fs::MediaRole role{};
    if (!ns.has_value() || !id.has_value() || !anvil::fs::role_from_segment(role_text, role)) {
        respond(anvil::accesscontrol::not_found_response());
        return;
    }
    const std::string accept{req->getHeader("accept")};
    const anvil::fs::Ns space = *ns;
    const Uuid target = *id;
    http::db_or_shed(req, respond, [respond, space, target, role, accept](mongocxx::client& client) {
        const auto found = services().media.find(client, space, target);
        if (!found || !found.value().has_value()) {
            respond(anvil::accesscontrol::not_found_response());
            return;
        }
        const anvil::media::MediaRecord& row = *found.value();
        const anvil::fs::VariantKey key =
            anvil::fs::mime_class(row.mime) == anvil::fs::MimeClass::Image
                ? anvil::media::resolve_role(row.variants, anvil::fs::role_width(space, role),
                                             anvil::media::negotiate_format(accept))
                : anvil::fs::kMasterVariant;
        drogon::HttpResponsePtr response = anvil::media::accel_redirect_response(space, target, key, row.mime);
        // Public website imagery: an object id names immutable bytes, so any
        // cache may keep it.
        response->removeHeader("Cache-Control");
        response->addHeader("Cache-Control", "public, max-age=31536000, immutable");
        respond(response);
    });
}

}  // namespace routes
}  // namespace enactus
