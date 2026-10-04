#include "anvil/sections/payload.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "anvil/core/uuid.h"
#include "anvil/http/json_writer.h"
#include "anvil/i18n/normalize.h"
#include "anvil/input/html.h"

namespace anvil::sections {
namespace {

[[nodiscard]] input::TextRules rules_for(const FieldSpec& spec) noexcept {
    return input::TextRules{1, spec.max_cp, i18n::TextClass::Prose,
                            spec.type == FieldType::RichText};
}

[[nodiscard]] bool is_hex_color(std::string_view text) noexcept {
    if (text.size() != 7 || text[0] != '#') { return false; }
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
    }
    return true;
}

// NFC, once, at the trust boundary. NEVER on read: that would be per-request ICU
// work on the hottest path in the system.
[[nodiscard]] std::string normalise(std::string_view text) {
    if (i18n::is_normalized(text, i18n::NormalizeMode::Nfc)) { return std::string{text}; }
    std::optional<std::string> normalised = i18n::normalize(text, i18n::NormalizeMode::Nfc);
    return normalised.has_value() ? std::move(*normalised) : std::string{text};
}

// One scalar string value against one FieldSpec. `out` receives the STORED form
// — normalised, and for RichText, sanitised.
[[nodiscard]] input::Reason bind_string(const FieldSpec& spec, std::string_view raw,
                                        const BindPolicy& policy, std::string& out) {
    // Ahead of check_text, because for a Choice field the allow-list IS the
    // check and it is a strictly stronger one: every member is a known literal,
    // so there is nothing a length bound or a bidi scan can add. Running
    // check_text first would only mean a blank value — the "none" choice — came
    // back as TooShort.
    if (spec.type == FieldType::Choice) {
        if (raw.empty() || is_choice(spec.choices, raw)) {
            out.assign(raw);
            return input::Reason::Ok;
        }
        return input::Reason::NotAllowed;
    }

    if (const input::Reason reason = input::check_text(raw, rules_for(spec));
        !input::is_ok(reason)) {
        return reason;
    }

    switch (spec.type) {
        case FieldType::Url:
            // Relative, or absolute against the scheme allow-list. An
            // unvalidated URL here is a stored open redirect, and with
            // `javascript:` it is stored XSS on every page that renders the
            // section.
            if (const input::Reason reason = input::check_url(raw, input::UrlUse::Link);
                !input::is_ok(reason)) {
                return reason;
            }
            out.assign(raw);
            return input::Reason::Ok;

        case FieldType::Color:
            if (!is_hex_color(raw)) { return input::Reason::BadFormat; }
            out.assign(raw);
            return input::Reason::Ok;

        case FieldType::RichText: {
            input::SanitizedHtml sanitized = input::sanitize_rich_text(
                normalise(raw), input::HtmlPolicy{policy.content_origin, spec.max_cp, 16});
            switch (sanitized.verdict()) {
                case input::HtmlVerdict::Ok:
                    out = std::move(sanitized).into_html();
                    return input::Reason::Ok;
                case input::HtmlVerdict::TooLong:
                    return input::Reason::TooLong;
                case input::HtmlVerdict::TooDeep:
                case input::HtmlVerdict::Hostile:
                    // REJECTED, not cleaned. A staff account submitting a script
                    // construct is a signal worth an audit record, and a cleaned
                    // version silently tells the submitter it worked.
                    return input::Reason::NotAllowed;
            }
            return input::Reason::NotAllowed;
        }

        case FieldType::Text:
            out = normalise(raw);
            return input::Reason::Ok;

        case FieldType::Number:
        case FieldType::Bool:
        case FieldType::Image:
        // Returned above, before check_text ran.
        case FieldType::Choice:
            break;
    }
    return input::Reason::BadFormat;
}

// The localised arm: an object carrying EVERY declared locale and nothing else.
[[nodiscard]] std::optional<BindError> bind_localized(const FieldSpec& spec,
                                                      const input::JsonValue& value,
                                                      const BindPolicy& policy,
                                                      SectionValue& stored) {
    if (!value.is_object()) { return BindError{spec.key, input::Reason::BadFormat}; }
    // An exact match on the declared set. Extra members are rejected rather than
    // ignored, for the same reason an unknown field is: a client that can send a
    // key nobody reads is a client discovering what this endpoint accepts.
    if (value.members().size() != kLocaleCount) {
        return BindError{spec.key, input::Reason::NotAllowed};
    }

    std::array<std::string_view, kLocaleCount> raw{};
    for (const Locale locale : kAllLocales) {
        const input::JsonValue* half = value.find(locale.tag());
        if (half == nullptr) { return BindError{spec.key, input::Reason::Required}; }
        const std::optional<std::string_view> text = half->as_string();
        if (!text.has_value()) { return BindError{spec.key, input::Reason::BadFormat}; }
        raw[locale.index()] = *text;
    }

    // EVERY half blank clears an OPTIONAL field. Without this a staff member can
    // fill a box but never empty one: check_text's minimum is one code point, so
    // "" comes back as TooShort and the only way to take a line off the site is
    // a deploy. SOME halves blank is not a clear — it is a half-translated
    // field, and it falls through to the loop below that refuses it.
    //
    // Required fields are unaffected: check_required is what enforces them, and
    // it runs against the MERGED document rather than against this patch, so
    // clearing one is still refused there.
    bool all_blank = true;
    for (const std::string_view half : raw) {
        if (!half.empty()) { all_blank = false; }
    }
    if (all_blank && !spec.required) { return std::nullopt; }

    for (const Locale locale : kAllLocales) {
        if (const input::Reason reason =
                bind_string(spec, raw[locale.index()], policy, stored.text[locale.index()]);
            !input::is_ok(reason)) {
            return BindError{spec.key, reason};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<BindError> bind_field(const FieldSpec& spec,
                                                  const input::JsonValue& value,
                                                  const BindPolicy& policy,
                                                  SectionContent& out) {
    SectionValue stored{};

    if (spec.type == FieldType::Number) {
        const std::optional<std::int64_t> number = value.as_int64();
        if (!number.has_value()) { return BindError{spec.key, input::Reason::BadFormat}; }
        stored.number = *number;
        out.set(std::string{spec.key}, std::move(stored));
        return std::nullopt;
    }
    if (spec.type == FieldType::Bool) {
        const std::optional<bool> boolean = value.as_bool();
        if (!boolean.has_value()) { return BindError{spec.key, input::Reason::BadFormat}; }
        stored.boolean = *boolean;
        out.set(std::string{spec.key}, std::move(stored));
        return std::nullopt;
    }

    if (spec.localized) {
        if (std::optional<BindError> bad = bind_localized(spec, value, policy, stored)) {
            return bad;
        }
        out.set(std::string{spec.key}, std::move(stored));
        return std::nullopt;
    }

    const std::optional<std::string_view> text = value.as_string();
    if (!text.has_value()) { return BindError{spec.key, input::Reason::BadFormat}; }
    // The same clear, for the single-valued half of the registry.
    if (text->empty() && !spec.required) {
        out.set(std::string{spec.key}, std::move(stored));
        return std::nullopt;
    }
    if (const input::Reason reason = bind_string(spec, *text, policy, stored.primary());
        !input::is_ok(reason)) {
        return BindError{spec.key, reason};
    }
    out.set(std::string{spec.key}, std::move(stored));
    return std::nullopt;
}

void append_value(std::string& out, const FieldSpec& spec, const SectionValue& value,
                  Locale locale) {
    switch (spec.type) {
        case FieldType::Number:
            http::append_json_int(out, value.number);
            return;
        case FieldType::Bool:
            out.append(value.boolean ? "true" : "false");
            return;
        default:
            break;
    }
    http::append_json_string(out, value.for_locale(spec, locale));
}

[[nodiscard]] std::int64_t parse_number(std::string_view text) noexcept {
    std::int64_t value = 0;
    // The literal was proved to be an integer at compile time by
    // ct::is_integer_literal, so this cannot fail — it is a conversion, not a
    // validation.
    (void)input::parse_int(text, INT64_MIN, INT64_MAX, value);
    return value;
}

// The image half of BOTH endpoints, and it is one function rather than two
// because a consumer that had to parse two spellings of the same requirement
// would pick one and drift on the other.
//
// `aspect` is NULL, not `{"num":0,"den":0}`, when the slot constrains no ratio.
// Zero is how the registry says "any shape" and verify_images reads it that way,
// but a reader testing the object's truthiness — `im.aspect ? … : null` — finds
// an object, which is always truthy, and prints "shaped 0:0" beside a slot. A
// staff member can neither satisfy 0:0 nor tell that nothing is being asked.
void append_image_spec(std::string& json, const ImageSpec& image) {
    json.push_back('{');
    http::append_json_key(json, "slot");
    http::append_json_string(json, image.slot);
    json.append(",\"label\":{");
    bool first = true;
    for (const Locale locale : kAllLocales) {
        if (!first) { json.push_back(','); }
        first = false;
        http::append_json_key(json, locale.tag());
        http::append_json_string(json, image.label.get(locale));
    }
    json.append("},\"min_width\":");
    http::append_json_int(json, image.min_width);
    json.append(",\"min_height\":");
    http::append_json_int(json, image.min_height);
    // Either half being zero is the whole ratio being absent: a `num` without a
    // `den` is not half a constraint, it is a typo, and neither the check nor
    // the reader can do anything with it.
    if (image.aspect_num == 0 || image.aspect_den == 0) {
        json.append(",\"aspect\":null}");
        return;
    }
    json.append(",\"aspect\":{\"num\":");
    http::append_json_int(json, image.aspect_num);
    json.append(",\"den\":");
    http::append_json_int(json, image.aspect_den);
    json.append("}}");
}

}  // namespace

SectionContent default_content(const SectionSpec& spec, const SectionDefaults& defaults) {
    SectionContent content;
    content.fields.reserve(spec.fields.size());
    for (const FieldSpec& field : spec.fields) {
        const DefaultField* value = nullptr;
        for (const DefaultField& candidate : defaults.fields) {
            if (candidate.key == field.key) { value = &candidate; }
        }
        // defaults_match_registry() proved every field has one at compile time.
        if (value == nullptr) { continue; }

        SectionValue stored{};
        switch (field.type) {
            case FieldType::Number:
                stored.number = parse_number(value->values.values[config::kDefaultLocale]);
                break;
            case FieldType::Bool:
                stored.boolean = value->values.values[config::kDefaultLocale] == "true";
                break;
            default:
                if (field.localized) {
                    for (const Locale locale : kAllLocales) {
                        stored.text[locale.index()].assign(value->values.get(locale));
                    }
                } else {
                    stored.primary().assign(value->values.values[config::kDefaultLocale]);
                }
                break;
        }
        content.fields.push_back(SectionField{std::string{field.key}, std::move(stored)});
    }
    return content;
}

std::optional<BindError> bind_data(const SectionSpec& spec, const input::JsonValue* data,
                                   const BindPolicy& policy, SectionContent& out) {
    if (data == nullptr) { return std::nullopt; }
    if (!data->is_object()) { return BindError{"data", input::Reason::BadFormat}; }

    for (const input::JsonMember& member : data->members()) {
        const FieldSpec* field = find_field(spec, member.key);
        if (field == nullptr) {
            // Unknown key: an error, never a silent drop. Reported with an EMPTY
            // field name so the response cannot echo the client's bytes.
            return BindError{{}, input::Reason::NotAllowed};
        }
        if (std::optional<BindError> bad = bind_field(*field, member.value, policy, out)) {
            return bad;
        }
    }
    return std::nullopt;
}

std::optional<BindError> bind_images(const SectionSpec& spec, const input::JsonValue* images,
                                     SectionContent& out) {
    if (images == nullptr) { return std::nullopt; }
    if (!images->is_object()) { return BindError{"images", input::Reason::BadFormat}; }

    for (const input::JsonMember& member : images->members()) {
        const ImageSpec* slot = find_image(spec, member.key);
        if (slot == nullptr) { return BindError{{}, input::Reason::NotAllowed}; }
        const std::optional<std::string_view> text = member.value.as_string();
        if (!text.has_value()) { return BindError{slot->slot, input::Reason::BadFormat}; }

        Uuid media_id{};
        if (const input::Reason reason = input::parse_uuid(*text, media_id);
            !input::is_ok(reason)) {
            return BindError{slot->slot, reason};
        }
        out.set_image(std::string{slot->slot}, media_id);
    }
    return std::nullopt;
}

std::optional<BindError> check_required(const SectionSpec& spec, const SectionContent& merged) {
    const FieldIndex index = index_content(spec, merged);
    for (std::size_t i = 0; i < spec.fields.size(); ++i) {
        const FieldSpec& field = spec.fields[i];
        if (!field.required) { continue; }
        const SectionField* stored = index.fields[i];
        if (stored == nullptr) { return BindError{field.key, input::Reason::Required}; }
        // A Number or a Bool is present or absent; there is no blank one.
        if (field.type == FieldType::Number || field.type == FieldType::Bool) { continue; }
        if (!field.localized) {
            if (stored->value.primary().empty()) {
                return BindError{field.key, input::Reason::Required};
            }
            continue;
        }
        for (const Locale locale : kAllLocales) {
            if (stored->value.text[locale.index()].empty()) {
                return BindError{field.key, input::Reason::Required};
            }
        }
    }
    return std::nullopt;
}

SerializedSection serialize(const SectionSpec& spec, const SectionContent& content,
                            std::int64_t version, Locale locale,
                            std::string_view image_url_base) {
    const FieldIndex index = index_content(spec, content);

    std::string json;
    // One allocation for the whole document. Sections are a few hundred bytes,
    // and a growth reallocation is an O(n) copy plus fragmentation
    // (CLAUDE.md §2.2).
    json.reserve(1024);

    json.push_back('{');
    http::append_json_key(json, "key");
    http::append_json_string(json, spec.key);
    json.push_back(',');
    http::append_json_key(json, "v");
    http::append_json_int(json, version);
    json.push_back(',');
    http::append_json_key(json, "lang");
    http::append_json_string(json, locale.tag());

    json.append(",\"data\":{");
    bool first = true;
    // REGISTRY order, not document order, so two instances that received the
    // same content in a different key order emit identical bytes — which is what
    // makes the etag a function of the content (see canonicalise()).
    for (std::size_t i = 0; i < spec.fields.size(); ++i) {
        if (index.fields[i] == nullptr) { continue; }
        if (!first) { json.push_back(','); }
        first = false;
        http::append_json_key(json, spec.fields[i].key);
        append_value(json, spec.fields[i], index.fields[i]->value, locale);
    }

    json.append("},\"images\":{");
    first = true;
    for (std::size_t i = 0; i < spec.images.size(); ++i) {
        if (index.images[i] == nullptr) { continue; }
        if (!first) { json.push_back(','); }
        first = false;
        http::append_json_key(json, spec.images[i].slot);
        json.append("{\"id\":");
        const std::string id = uuid::to_string(index.images[i]->media_id);
        http::append_json_string(json, id);
        json.append(",\"src\":");
        std::string src;
        src.reserve(image_url_base.size() + id.size() + 1);
        src.append(image_url_base);
        src.push_back('/');
        src.append(id);
        http::append_json_string(json, src);
        json.push_back('}');
    }
    json.append("}}");

    const crypto::FastDigest etag = crypto::xxh3_64(json);
    return SerializedSection{std::move(json), etag, crypto::etag_of(etag), version};
}

crypto::FastDigest content_etag(const SectionSpec& spec, const SectionContent& content) {
    // Hashed over a canonical rendering rather than over the struct: the struct
    // contains std::string, whose bytes are not contiguous with each other, and
    // hashing it member-by-member would need its own stable ordering anyway.
    // Walking the registry gives that ordering for free.
    const FieldIndex index = index_content(spec, content);
    std::string canonical;
    canonical.reserve(1024);
    for (std::size_t i = 0; i < spec.fields.size(); ++i) {
        if (index.fields[i] == nullptr) { continue; }
        const FieldSpec& field = spec.fields[i];
        const SectionValue& value = index.fields[i]->value;
        canonical.append(field.key);
        canonical.push_back('\x1f');
        switch (field.type) {
            case FieldType::Number:
                http::append_json_int(canonical, value.number);
                break;
            case FieldType::Bool:
                canonical.append(value.boolean ? "true" : "false");
                break;
            default:
                // Every locale, so a change confined to one of them still moves
                // the digest. A per-locale etag would not: it is the CONTENT
                // that has to be compared here, not one rendering of it.
                for (const Locale locale : kAllLocales) {
                    canonical.append(value.for_locale(field, locale));
                    canonical.push_back('\x1f');
                    if (!field.localized) { break; }
                }
                break;
        }
        canonical.push_back('\x1f');
    }
    for (std::size_t i = 0; i < spec.images.size(); ++i) {
        if (index.images[i] == nullptr) { continue; }
        canonical.append(spec.images[i].slot);
        canonical.push_back('\x1f');
        const Uuid& id = index.images[i]->media_id;
        canonical.append(reinterpret_cast<const char*>(id.data()), id.size());
        canonical.push_back('\x1f');
    }
    return crypto::xxh3_64(canonical);
}

std::string serialize_image_specs(const SectionSpec& spec) {
    std::string json;
    json.reserve(256 + (spec.images.size() * 192));
    json.push_back('{');
    http::append_json_key(json, "key");
    http::append_json_string(json, spec.key);
    json.append(",\"images\":[");

    bool first = true;
    for (const ImageSpec& image : spec.images) {
        if (!first) { json.push_back(','); }
        first = false;
        append_image_spec(json, image);
    }
    json.append("]}");
    return json;
}

void append_shape_json(std::string& out, const SectionSpec& spec) {
    out.push_back('{');
    http::append_json_key(out, "key");
    http::append_json_string(out, spec.key);
    out.push_back(',');
    http::append_json_key(out, "path");
    http::append_json_string(out, spec.site_path);
    out.append(",\"fields\":[");

    bool first_field = true;
    for (const FieldSpec& field : spec.fields) {
        if (!first_field) { out.push_back(','); }
        first_field = false;
        out.push_back('{');
        http::append_json_key(out, "key");
        http::append_json_string(out, field.key);
        out.append(",\"label\":{");
        bool first_label = true;
        for (const Locale locale : kAllLocales) {
            if (!first_label) { out.push_back(','); }
            first_label = false;
            http::append_json_key(out, locale.tag());
            http::append_json_string(out, field.label.get(locale));
        }
        out.append("},\"type\":");
        http::append_json_string(out, field_type_name(field.type));
        out.append(",\"max_cp\":");
        http::append_json_int(out, field.max_cp);
        out.append(field.localized ? ",\"localized\":true" : ",\"localized\":false");
        out.append(field.required ? ",\"required\":true" : ",\"required\":false");
        // The choices travel WITH the field, so an editor renders a picker
        // without holding a second copy of the allow-list. A copy there
        // would drift the first time a value is added here, and the failure
        // would be a staff member choosing a value the server refuses —
        // exactly the class of bug the registry endpoint exists to remove.
        if (field.type == FieldType::Choice) {
            out.append(",\"options\":[");
            bool first_option = true;
            for (const std::string_view option : field.choices) {
                if (!first_option) { out.push_back(','); }
                first_option = false;
                http::append_json_string(out, option);
            }
            out.push_back(']');
        }
        out.push_back('}');
    }
    out.append("],\"images\":[");

    bool first_image = true;
    for (const ImageSpec& image : spec.images) {
        if (!first_image) { out.push_back(','); }
        first_image = false;
        append_image_spec(out, image);
    }
    out.append("]}");
}

std::string serialize_registry(std::span<const SectionSpec> registry) {
    std::string json;
    // Reserved from the registry's own size rather than from a literal, so
    // adding a section does not silently start reallocating.
    json.reserve(128 + (registry.size() * 1024));
    json.append("{\"sections\":[");

    bool first_section = true;
    for (const SectionSpec& spec : registry) {
        if (!first_section) { json.push_back(','); }
        first_section = false;
        append_shape_json(json, spec);
    }
    json.append("]}");
    return json;
}

}  // namespace anvil::sections
