#include "anvil/entries/payload.h"

#include <array>
#include <bit>
#include <cstdint>

#include "anvil/core/locale.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/fast_hash.h"
#include "anvil/http/json_writer.h"
#include "anvil/input/fields.h"

namespace anvil::entries {
namespace {

[[nodiscard]] std::string_view workflow_name(Workflow workflow) noexcept {
    switch (workflow) {
        case Workflow::Editorial: return "editorial";
        case Workflow::Immediate: return "immediate";
    }
    return "editorial";
}

[[nodiscard]] std::string_view ordering_name(Ordering ordering) noexcept {
    switch (ordering) {
        case Ordering::Manual: return "manual";
        case Ordering::Newest: return "newest";
        case Ordering::Oldest: return "oldest";
    }
    return "manual";
}

[[nodiscard]] std::string_view slug_rule_name(SlugRule rule) noexcept {
    switch (rule) {
        case SlugRule::None:   return "none";
        case SlugRule::Unique: return "unique";
    }
    return "none";
}

void append_image_src(std::string& out, const ImageLinks& links, const Uuid& id) {
    std::string src;
    src.reserve(links.base.size() + 37 + links.suffix.size());
    src.append(links.base);
    src.push_back('/');
    src.append(uuid::to_string(id));
    src.append(links.suffix);
    http::append_json_string(out, src);
}

// Every field of the shape, in REGISTRY order, with `null` for one the copy
// does not hold — an editor then renders one control per field without a
// second pass to find out which are missing.
void append_copy(std::string& out, const sections::SectionSpec& shape, const EntryContent& copy,
                 const ImageLinks& links) {
    const sections::FieldIndex index = sections::index_content(shape, copy.content);
    out.append("{\"etag\":");
    const std::array<char, 18> etag = crypto::etag_of(copy.etag);
    http::append_json_string(out, std::string_view{etag.data(), etag.size()});
    out.append(",\"updated_at\":");
    http::append_json_time(out, copy.updated_at.time_since_epoch().count());
    out.append(",\"updated_by\":");
    http::append_json_uuid(out, copy.updated_by);
    out.append(",\"data\":{");
    for (std::size_t i = 0; i < shape.fields.size(); ++i) {
        const sections::FieldSpec& field = shape.fields[i];
        if (i != 0) { out.push_back(','); }
        http::append_json_key(out, field.key);
        const sections::SectionField* stored = index.fields[i];
        if (stored == nullptr) {
            out.append("null");
            continue;
        }
        switch (field.type) {
            case sections::FieldType::Number:
                http::append_json_int(out, stored->value.number);
                break;
            case sections::FieldType::Bool:
                out.append(stored->value.boolean ? "true" : "false");
                break;
            default:
                if (!field.localized) {
                    http::append_json_string(out, stored->value.primary());
                    break;
                }
                out.push_back('{');
                for (const Locale locale : kAllLocales) {
                    if (locale.index() != 0) { out.push_back(','); }
                    http::append_json_key(out, locale.tag());
                    http::append_json_string(out, stored->value.text[locale.index()]);
                }
                out.push_back('}');
                break;
        }
    }
    out.append("},\"images\":{");
    for (std::size_t i = 0; i < shape.images.size(); ++i) {
        if (i != 0) { out.push_back(','); }
        http::append_json_key(out, shape.images[i].slot);
        const sections::SectionImage* stored = index.images[i];
        if (stored == nullptr) {
            out.append("null");
            continue;
        }
        out.append("{\"id\":");
        http::append_json_uuid(out, stored->media_id);
        out.append(",\"src\":");
        append_image_src(out, links, stored->media_id);
        out.push_back('}');
    }
    out.append("}}");
}

}  // namespace

std::optional<BindError> bind_slug(const input::JsonValue* value, std::string& out) {
    if (value == nullptr) { return std::nullopt; }
    const std::optional<std::string_view> text = value->as_string();
    if (!text.has_value()) { return BindError{"slug", input::Reason::BadFormat}; }
    if (!is_wellformed_slug(*text)) { return BindError{"slug", input::Reason::BadFormat}; }
    out.assign(*text);
    return std::nullopt;
}

std::optional<BindError> bind_flags(const KindSpec& kind, const input::JsonValue* value,
                                    FlagSet& set, FlagSet& clear) {
    set = 0;
    clear = 0;
    if (value == nullptr) { return std::nullopt; }
    if (!value->is_object()) { return BindError{"flags", input::Reason::BadFormat}; }
    for (const input::JsonMember& member : value->members()) {
        const FlagSet bit = flag_bit(kind, member.key);
        if (bit == 0) { return BindError{"", input::Reason::NotAllowed}; }
        const std::optional<bool> on = member.value.as_bool();
        // The REGISTRY's spelling of the name, never the request's.
        const std::string_view name = kind.flags[static_cast<std::size_t>(std::countr_zero(
                                                     static_cast<unsigned>(bit)))]
                                          .key;
        // A flag named twice never arrives: the parser refuses a duplicate key
        // outright (anvil/input/json.h).
        if (!on.has_value()) { return BindError{name, input::Reason::BadFormat}; }
        if (*on) {
            set = static_cast<FlagSet>(set | bit);
        } else {
            clear = static_cast<FlagSet>(clear | bit);
        }
    }
    return std::nullopt;
}

std::optional<BindError> bind_order(const input::JsonValue* value, std::size_t max,
                                    std::vector<Uuid>& out) {
    if (value == nullptr) { return BindError{"order", input::Reason::Required}; }
    if (!value->is_array()) { return BindError{"order", input::Reason::BadFormat}; }
    const std::span<const input::JsonValue> ids = value->elements();
    if (ids.size() > max) { return BindError{"order", input::Reason::TooLong}; }
    out.clear();
    out.reserve(ids.size());
    for (const input::JsonValue& element : ids) {
        const std::optional<std::string_view> text = element.as_string();
        if (!text.has_value()) { return BindError{"order", input::Reason::BadFormat}; }
        Uuid id{};
        if (const input::Reason reason = input::parse_uuid(*text, id); !input::is_ok(reason)) {
            return BindError{"order", reason};
        }
        out.push_back(id);
    }
    return std::nullopt;
}

std::string serialize_kinds(std::span<const KindSpec> kinds) {
    std::string json;
    json.reserve(128 + (kinds.size() * 2048));
    json.append("{\"kinds\":[");
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const KindSpec& kind = kinds[i];
        if (i != 0) { json.push_back(','); }
        json.push_back('{');
        http::append_json_key(json, "key");
        http::append_json_string(json, kind.shape.key);
        json.append(",\"parent\":");
        if (kind.parent.empty()) {
            json.append("null");
        } else {
            http::append_json_string(json, kind.parent);
        }
        json.append(",\"capacity\":");
        http::append_json_int(json, kind.capacity);
        json.append(",\"workflow\":");
        http::append_json_string(json, workflow_name(kind.workflow));
        json.append(",\"ordering\":");
        http::append_json_string(json, ordering_name(kind.ordering));
        json.append(",\"slug\":");
        http::append_json_string(json, slug_rule_name(kind.slug));
        json.append(",\"flags\":[");
        for (std::size_t f = 0; f < kind.flags.size(); ++f) {
            if (f != 0) { json.push_back(','); }
            json.append("{\"key\":");
            http::append_json_string(json, kind.flags[f].key);
            json.append(",\"label\":{");
            for (const Locale locale : kAllLocales) {
                if (locale.index() != 0) { json.push_back(','); }
                http::append_json_key(json, locale.tag());
                http::append_json_string(json, kind.flags[f].label.get(locale));
            }
            json.append("}}");
        }
        json.append("],\"shape\":");
        sections::append_shape_json(json, kind.shape);
        json.push_back('}');
    }
    json.append("]}");
    return json;
}

void append_entry_json(std::string& out, const EntryDocument& entry, const ImageLinks& links) {
    const KindSpec& kind = *entry.kind;
    out.append("{\"id\":");
    http::append_json_uuid(out, entry.id);
    out.append(",\"kind\":");
    http::append_json_string(out, kind.shape.key);
    out.append(",\"slug\":");
    if (entry.slug.empty()) {
        out.append("null");
    } else {
        http::append_json_string(out, entry.slug);
    }
    out.append(",\"parent\":");
    if (entry.parent.has_value()) {
        http::append_json_uuid(out, *entry.parent);
    } else {
        out.append("null");
    }
    out.append(",\"position\":");
    http::append_json_int(out, entry.position);
    out.append(",\"children\":");
    http::append_json_int(out, entry.children);
    // By name, every declared flag, true or false: an editor draws a switch per
    // flag without knowing the bit layout, which is this library's business.
    out.append(",\"flags\":{");
    for (std::size_t f = 0; f < kind.flags.size(); ++f) {
        if (f != 0) { out.push_back(','); }
        http::append_json_key(out, kind.flags[f].key);
        out.append(entry.has(static_cast<FlagSet>(1U << f)) ? "true" : "false");
    }
    out.append("},\"live\":");
    out.append(entry.live() ? "true" : "false");
    out.append(",\"version\":");
    http::append_json_int(out, entry.version);
    out.append(",\"created_at\":");
    http::append_json_time(out, entry.created_at.time_since_epoch().count());
    out.append(",\"created_by\":");
    http::append_json_uuid(out, entry.created_by);
    out.append(",\"updated_at\":");
    http::append_json_time(out, entry.updated_at.time_since_epoch().count());
    out.append(",\"published\":");
    if (entry.published.has_value()) {
        append_copy(out, kind.shape, *entry.published, links);
    } else {
        out.append("null");
    }
    out.append(",\"draft\":");
    if (entry.draft.has_value()) {
        append_copy(out, kind.shape, *entry.draft, links);
    } else {
        out.append("null");
    }
    out.push_back('}');
}

std::string serialize_entry(const EntryDocument& entry, const ImageLinks& links) {
    std::string json;
    json.reserve(4096);
    append_entry_json(json, entry, links);
    return json;
}

std::string serialize_page(const EntryPage& page, const ImageLinks& links) {
    std::string json;
    json.reserve(64 + (page.entries.size() * 4096));
    json.append("{\"entries\":[");
    for (std::size_t i = 0; i < page.entries.size(); ++i) {
        if (i != 0) { json.push_back(','); }
        append_entry_json(json, page.entries[i], links);
    }
    json.append("],\"next\":");
    if (page.next.has_value()) {
        http::append_json_uuid(json, page.next->after);
    } else {
        json.append("null");
    }
    json.push_back('}');
    return json;
}

}  // namespace anvil::entries
