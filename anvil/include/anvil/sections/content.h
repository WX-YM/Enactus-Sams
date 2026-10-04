#pragma once

// The in-memory shape of a section's CONTENT — the values staff own, as opposed
// to the shape developers own in registry.h.
//
// It lives beside the registry rather than in the repository because three
// layers need the same type and none of them should have to know about the
// others: a controller binds a request into one, a draft or preview service
// validates into one, and the repository encodes one into BSON. Putting it in
// the repository would make the validation layer depend upward.
//
// Which member of SectionValue is live is decided by the registry's FieldType
// for that key, NEVER by inspecting the value. That is the same "type before
// value" rule the JSON binder enforces at the edge, applied one layer in.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "anvil/core/locale.h"
#include "anvil/core/types.h"
#include "anvil/sections/registry.h"

namespace anvil::sections {

struct SectionValue final {
    // One per declared locale for a localised field. A NON-localised field
    // carries its single value at the default locale's index and leaves the rest
    // empty — the same convention DefaultField uses, and for the same reason:
    // one value stored twice is two values that can disagree.
    //
    // Owned strings rather than views. Content is decoded out of a BSON document
    // that dies with the query, and it then crosses into a cache that outlives
    // every request that reads it (CLAUDE.md §2.2).
    std::array<std::string, kLocaleCount> text;
    std::int64_t                          number{0};
    bool                                  boolean{false};

    // The slot a non-localised value lives in, and the one every type check runs
    // against. Named rather than spelled `text[0]` at each site, because the
    // default locale is the application's choice and need not be index zero.
    [[nodiscard]] std::string& primary() noexcept { return text[config::kDefaultLocale]; }
    [[nodiscard]] const std::string& primary() const noexcept {
        return text[config::kDefaultLocale];
    }

    // The value to render for one locale, given the field that owns it. A
    // non-localised field answers with its single value whatever the locale.
    [[nodiscard]] std::string_view for_locale(const FieldSpec& spec,
                                              Locale locale) const noexcept {
        return spec.localized ? std::string_view{text[locale.index()]}
                              : std::string_view{primary()};
    }
};

struct SectionField final {
    std::string  key;
    SectionValue value;
};

struct SectionImage final {
    std::string slot;
    Uuid        media_id;
};

// A whole section's values, or a PATCH containing only the fields a request
// supplied. The two are the same type on purpose: merging is then one function
// rather than a conversion, and there is no shape in which "a patch" and "a
// document" can drift apart.
struct SectionContent final {
    std::vector<SectionField> fields;
    std::vector<SectionImage> images;

    [[nodiscard]] const SectionField* find(std::string_view key) const noexcept {
        for (const SectionField& field : fields) {
            if (field.key == key) { return &field; }
        }
        return nullptr;
    }

    [[nodiscard]] const SectionImage* find_image(std::string_view slot) const noexcept {
        for (const SectionImage& image : images) {
            if (image.slot == slot) { return &image; }
        }
        return nullptr;
    }

    void set(std::string key, SectionValue value) {
        for (SectionField& field : fields) {
            if (field.key == key) {
                field.value = std::move(value);
                return;
            }
        }
        fields.push_back(SectionField{std::move(key), std::move(value)});
    }

    void set_image(std::string slot, const Uuid& media_id) {
        for (SectionImage& image : images) {
            if (image.slot == slot) {
                image.media_id = media_id;
                return;
            }
        }
        images.push_back(SectionImage{std::move(slot), media_id});
    }
};

// One pointer per DECLARED field and per declared slot, in registry order, with
// nullptr where the content carries nothing.
//
// Built once and walked, rather than calling SectionContent::find per field:
// find is a linear scan, so serialising a 36-field section without this is
// 36 × 36 string compares for what one pass answers. The arrays are stack
// scratch, which is what kMaxFieldsPerSection bounds and what fields_fit_buffers
// turns into a build failure rather than an overflow.
struct FieldIndex final {
    std::array<const SectionField*, kMaxFieldsPerSection> fields;
    std::array<const SectionImage*, kMaxImagesPerSection> images;
};

[[nodiscard]] inline FieldIndex index_content(const SectionSpec& spec,
                                              const SectionContent& content) noexcept {
    FieldIndex index{};
    for (const SectionField& stored : content.fields) {
        for (std::size_t i = 0; i < spec.fields.size(); ++i) {
            if (spec.fields[i].key == stored.key) {
                index.fields[i] = &stored;
                break;
            }
        }
    }
    for (const SectionImage& stored : content.images) {
        for (std::size_t i = 0; i < spec.images.size(); ++i) {
            if (spec.images[i].slot == stored.slot) {
                index.images[i] = &stored;
                break;
            }
        }
    }
    return index;
}

// `patch` wins where it supplies a value; everything else survives. This is what
// makes a partial update that omits an already-stored `required` field legal,
// and a partial update to a never-written section missing one illegal —
// `required` is checked against the RESULT of this, never against the patch.
[[nodiscard]] inline SectionContent merge(const SectionContent& base,
                                          const SectionContent& patch) {
    SectionContent merged = base;
    for (const SectionField& field : patch.fields) { merged.set(field.key, field.value); }
    for (const SectionImage& image : patch.images) {
        merged.set_image(image.slot, image.media_id);
    }
    return merged;
}

// Reorders `content` into registry order and drops anything the registry no
// longer declares. Called before serialisation and before storage, so two
// instances that received the same patch in a different key order produce
// BYTE-IDENTICAL output — which is what makes the etag a function of the content
// rather than of the request that happened to write it.
[[nodiscard]] inline SectionContent canonicalise(const SectionSpec& spec,
                                                 const SectionContent& content) {
    const FieldIndex index = index_content(spec, content);
    SectionContent out;
    out.fields.reserve(spec.fields.size());
    out.images.reserve(spec.images.size());
    for (std::size_t i = 0; i < spec.fields.size(); ++i) {
        if (index.fields[i] != nullptr) { out.fields.push_back(*index.fields[i]); }
    }
    for (std::size_t i = 0; i < spec.images.size(); ++i) {
        if (index.images[i] != nullptr) { out.images.push_back(*index.images[i]); }
    }
    return out;
}

}  // namespace anvil::sections
