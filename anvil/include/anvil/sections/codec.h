#pragma once

// The BSON encoding of a section's content.
//
// It is its own header rather than a private part of the repository because
// anything that stores a section's values stores this exact shape — the live
// document, and whatever draft, preview or snapshot collection an application
// builds beside it. Two independent encoders would be two places to change when
// a FieldType is added, and the failure mode of forgetting one is a draft that
// cannot be committed.
//
// The registry drives both directions. A stored field with no registry entry is
// dropped on read, and a value with no spec is skipped on write: the allow-list
// is not a check applied to the codec, it IS the codec.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/sections/content.h"
#include "anvil/sections/registry.h"

namespace anvil::sections::codec {

inline constexpr std::string_view kDataField = "data";
inline constexpr std::string_view kMediaField = "media";

inline void append_value(bsoncxx::builder::basic::sub_document& data, const FieldSpec& spec,
                         const SectionValue& value) {
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::sub_document;

    switch (spec.type) {
        case FieldType::Number:
            data.append(kvp(db::codec::key_of(spec.key), bsoncxx::types::b_int64{value.number}));
            return;
        case FieldType::Bool:
            data.append(kvp(db::codec::key_of(spec.key), bsoncxx::types::b_bool{value.boolean}));
            return;
        default:
            break;
    }
    if (spec.localized) {
        // Keyed by locale TAG, which is the same string the wire and the `?lang=`
        // query use. Three spellings of one locale is three places for them to
        // disagree (anvil/core/locale_spec.h).
        data.append(kvp(db::codec::key_of(spec.key), [&value](sub_document sub) {
            for (const Locale locale : kAllLocales) {
                sub.append(kvp(db::codec::key_of(locale.tag()),
                               bsoncxx::types::b_string{
                                   db::codec::key_of(value.text[locale.index()])}));
            }
        }));
        return;
    }
    data.append(kvp(db::codec::key_of(spec.key),
                    bsoncxx::types::b_string{db::codec::key_of(value.primary())}));
}

// Appends `data: {...}` and `media: {...}` as WHOLE subdocuments.
//
// Whole rather than dotted `$set` paths deliberately: a section is a few hundred
// bytes and is written a handful of times a day, so a partial update saves
// nothing — while a whole-subdocument write is what makes a field REMOVED from
// the registry actually disappear from storage instead of lingering forever
// behind an update that never mentions it.
inline void append_content(bsoncxx::builder::basic::document& doc, const SectionSpec& spec,
                           const SectionContent& content) {
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::sub_document;

    doc.append(kvp(db::codec::key_of(kDataField), [&spec, &content](sub_document data) {
        for (const SectionField& field : content.fields) {
            const FieldSpec* field_spec = find_field(spec, field.key);
            if (field_spec == nullptr) { continue; }
            append_value(data, *field_spec, field.value);
        }
    }));
    doc.append(kvp(db::codec::key_of(kMediaField), [&spec, &content](sub_document media) {
        for (const SectionImage& image : content.images) {
            if (find_image(spec, image.slot) == nullptr) { continue; }
            media.append(kvp(db::codec::key_of(image.slot), db::codec::uuid_bin(image.media_id)));
        }
    }));
}

// A section's per-locale subdocument, read WITHOUT the non-empty requirement
// that db::codec::read_localized enforces.
//
// That function is right for the types it was written for: a category with a
// blank name in one locale renders a blank heading there, silently, and refusing
// to decode it is the correct answer. A SECTION field is a different thing.
// Whether it may be blank is a property of its FieldSpec — `required` — and
// check_required is where that is enforced, against the MERGED document. A codec
// that also has an opinion about it is a second rule, and the two disagree the
// moment clearing an optional field becomes possible: the writer stores a pair
// of empty strings happily, and the reader then refuses the whole document,
// taking every section read as one prefix off the site at once.
//
// So: this checks SHAPE — a subdocument carrying one string per declared locale
// — and the registry checks CONTENT. A field missing a locale entirely is still
// corruption and still fails, because that is a shape error, not an empty value.
[[nodiscard]] inline Status read_section_localized(const bsoncxx::document::view& data,
                                                   std::string_view field,
                                                   SectionValue& out) {
    const bsoncxx::document::element element = data[db::codec::key_of(field)];
    if (!element || element.type() != bsoncxx::type::k_document) {
        return fail(ErrorCode::Internal, field);
    }
    const bsoncxx::document::view pair = element.get_document().value;
    for (const Locale locale : kAllLocales) {
        const Result<std::string_view> text = db::codec::read_text(pair, locale.tag());
        if (!text) { return fail(ErrorCode::Internal, field); }
        out.text[locale.index()].assign(text.value());
    }
    return ok();
}

[[nodiscard]] inline Result<SectionValue> decode_value(const bsoncxx::document::view& data,
                                                       const FieldSpec& spec) {
    SectionValue value{};
    switch (spec.type) {
        case FieldType::Number: {
            const Result<std::int64_t> number = db::codec::read_int64(data, spec.key);
            if (!number) { return number.error(); }
            value.number = number.value();
            return value;
        }
        case FieldType::Bool: {
            const Result<bool> boolean = db::codec::read_bool(data, spec.key);
            if (!boolean) { return boolean.error(); }
            value.boolean = boolean.value();
            return value;
        }
        default:
            break;
    }
    if (spec.localized) {
        if (const Status read = read_section_localized(data, spec.key, value); !read) {
            return read.error();
        }
        return value;
    }
    const Result<std::string_view> text = db::codec::read_text(data, spec.key);
    if (!text) { return text.error(); }
    value.primary().assign(text.value());
    return value;
}

// `doc` is the document holding `data` and `media`, not the subdocument itself.
//
// A field the registry declares but the document does not carry is ABSENT, not
// an error: partial updates are the normal case, and `required` is enforced
// against the merged result by the service.
[[nodiscard]] inline Result<SectionContent> decode_content(const bsoncxx::document::view& doc,
                                                           const SectionSpec& spec) {
    const bsoncxx::document::element data = doc[db::codec::key_of(kDataField)];
    if (!data || data.type() != bsoncxx::type::k_document) {
        return fail(ErrorCode::Internal, kDataField);
    }
    const bsoncxx::document::view data_view = data.get_document().value;

    SectionContent content{};
    content.fields.reserve(spec.fields.size());
    for (const FieldSpec& field : spec.fields) {
        if (!data_view[db::codec::key_of(field.key)]) { continue; }
        Result<SectionValue> value = decode_value(data_view, field);
        if (!value) { return value.error(); }
        content.fields.push_back(SectionField{std::string{field.key}, std::move(value).value()});
    }

    const bsoncxx::document::element media = doc[db::codec::key_of(kMediaField)];
    if (media && media.type() == bsoncxx::type::k_document) {
        const bsoncxx::document::view media_view = media.get_document().value;
        content.images.reserve(spec.images.size());
        for (const ImageSpec& image : spec.images) {
            if (!media_view[db::codec::key_of(image.slot)]) { continue; }
            const Result<Uuid> id = db::codec::read_uuid(media_view, image.slot);
            if (!id) { return id.error(); }
            content.images.push_back(SectionImage{std::string{image.slot}, id.value()});
        }
    }
    return content;
}

}  // namespace anvil::sections::codec
