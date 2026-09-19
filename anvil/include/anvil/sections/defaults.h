#pragma once

// Compile-time default content for every registry section.
//
// "Static" here means `constexpr` data in `.rodata`, not a `static std::string`
// of JSON parsed at start-up. The difference is not stylistic:
//
//   start-up   a parsed blob costs a heap allocation and a JSON parse per
//              process; this costs nothing at all
//   memory     `.rodata` is shared across threads AND across forked processes
//   errors     a default that violates its own FieldSpec is a BUILD FAILURE
//              here, and a first-boot-in-production failure there
//
// That last line is the whole point. `defaults_match_registry()` is evaluated in
// a `static_assert`, so an 80-code-point limit with a 90-code-point default, a
// missing locale, or a literal that is not valid UTF-8 all stop the build.
//
// Source and execution charsets must both be UTF-8 for non-Latin literals to
// mean what they look like. anvil's hardening module pins that, and
// ct::is_valid_utf8 proves it rather than assuming it.
//
// anvil ships the three structs and the conformance check. The CONTENT is the
// application's: a default is what a fresh deployment publishes, and anvil has
// no words to put on somebody else's page (docs/01-seams.md §6).

#include <cstddef>
#include <span>
#include <string_view>

#include "anvil/core/locale.h"
#include "anvil/sections/registry.h"

namespace anvil::sections {

struct DefaultField final {
    std::string_view key;

    // One value per declared locale, in kLocales order.
    //
    // A NON-localised field carries its single value at the default locale's
    // index and leaves every other one EMPTY. Repeating the value in each slot
    // would make "the two disagree" a representable state that nothing checks;
    // leaving them empty makes the localisation of a field a property of the
    // registry alone, and the check below enforces it.
    Localized<>      values;
};

static_assert(sizeof(DefaultField) == (1 + kLocaleCount) * sizeof(std::string_view),
              "DefaultField must not grow padding");

struct DefaultImage final {
    std::string_view slot;
    // A file in the read-only defaults tree installed beside the binary.
    //
    // Defaults cannot reference uploaded media ids — those do not exist on a
    // fresh database — so they ship as FILES and are registered through the
    // ordinary media pipeline at boot, which probes, normalises and derives
    // variants for them exactly as it would for an upload. A default image
    // therefore cannot be a format the serving path refuses.
    std::string_view file;
};

static_assert(sizeof(DefaultImage) == 2 * sizeof(std::string_view));

struct SectionDefaults final {
    std::string_view              key;
    std::span<const DefaultField> fields;
    std::span<const DefaultImage> images;
};

// --- compile-time conformance ----------------------------------------------
//
// Every one of these runs at constant-evaluation time only. None of it exists in
// the binary.

namespace ct {

[[nodiscard]] constexpr bool is_integer_literal(std::string_view text) noexcept {
    if (text.empty() || text.size() > 19) { return false; }
    std::size_t i = (text[0] == '-') ? 1 : 0;
    if (i == text.size()) { return false; }
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') { return false; }
    }
    return true;
}

[[nodiscard]] constexpr bool is_bool_literal(std::string_view text) noexcept {
    return text == "true" || text == "false";
}

[[nodiscard]] constexpr bool is_hex_color(std::string_view text) noexcept {
    if (text.size() != 7 || text[0] != '#') { return false; }
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        const bool digit = c >= '0' && c <= '9';
        const bool hex = c >= 'a' && c <= 'f';
        if (!digit && !hex) { return false; }
    }
    return true;
}

// The constant-evaluation twin of input::check_url(UrlUse::Link). Deliberately
// STRICTER than the runtime check: a compiled-in default has no excuse to be
// anything but a fragment reference, a site-relative path, or an https URL.
[[nodiscard]] constexpr bool is_safe_default_url(std::string_view url) noexcept {
    if (url.empty() || url.size() > 256) { return false; }
    // Protocol-relative. It inherits the scheme of whatever page renders it,
    // which on a plaintext page is a downgrade nobody chose.
    if (url.size() >= 2 && url[0] == '/' && url[1] == '/') { return false; }
    // A fragment reference, which is what a page of a hash-routed client is
    // called from inside that client. Narrower than the site-relative form
    // below: no scheme, no host, no path, and nothing to redirect to.
    if (url[0] == '#') { return true; }
    if (url[0] == '/') { return true; }
    constexpr std::string_view kHttps = "https://";
    return url.size() > kHttps.size() && url.compare(0, kHttps.size(), kHttps) == 0;
}

[[nodiscard]] constexpr bool value_matches_spec(const FieldSpec& spec,
                                                const DefaultField& value) noexcept {
    // The single value of a non-localised field, and the default locale's half
    // of a localised one. Every type check below runs against it, because a
    // Number, Bool, Url, Colour or Choice is not translated.
    const std::string_view primary = value.values.values[config::kDefaultLocale];
    if (!is_non_empty_utf8(primary)) { return false; }
    if (spec.type != FieldType::Bool && count_code_points(primary) > spec.max_cp) {
        return false;
    }

    for (std::size_t i = 0; i < kLocaleCount; ++i) {
        if (i == config::kDefaultLocale) { continue; }
        const std::string_view other = value.values.values[i];
        if (!spec.localized) {
            // A non-localised field with a value in a second slot is a value
            // nothing will ever read, and the pair is free to disagree.
            if (!other.empty()) { return false; }
            continue;
        }
        // EVERY locale, non-empty and valid UTF-8. This is the check that
        // catches a forgotten translation at build time rather than at the
        // first boot.
        if (!is_non_empty_utf8(other)) { return false; }
        if (count_code_points(other) > spec.max_cp) { return false; }
    }

    switch (spec.type) {
        case FieldType::Number: return is_integer_literal(primary);
        case FieldType::Bool:   return is_bool_literal(primary);
        case FieldType::Url:    return is_safe_default_url(primary);
        case FieldType::Color:  return is_hex_color(primary);
        // A default choice is a real member, never blank: "none" is a choice a
        // staff member makes, not a state a fresh deployment ships in.
        case FieldType::Choice: return is_choice(spec.choices, primary);
        case FieldType::Image:  return false;   // never a data field
        case FieldType::Text:
        case FieldType::RichText:
            return true;
    }
    return false;
}

}  // namespace ct

// Every failure mode this covers is one that would otherwise surface as a broken
// public page on a fresh deployment.
//
// POSITIONAL correspondence with the registry, so bootstrap walks both by index
// and never looks anything up.
[[nodiscard]] constexpr bool defaults_match_registry(
    std::span<const SectionSpec> registry, std::span<const SectionDefaults> defaults) noexcept {
    if (registry.size() != defaults.size()) { return false; }

    for (std::size_t i = 0; i < registry.size(); ++i) {
        const SectionSpec& section = registry[i];
        const SectionDefaults& supplied = defaults[i];
        if (section.key != supplied.key) { return false; }

        // EVERY field needs a default, not only the required ones: a fresh
        // deployment must produce a complete, renderable section, and a section
        // missing an optional string renders a gap rather than an error.
        //
        // Searched by value rather than by returning a pointer: some compilers
        // refuse to constant-evaluate a comparison against a pointer into a
        // namespace-scope object under -fsanitize=undefined, and this must hold
        // in the sanitiser build too — that is the build CI gates on.
        if (supplied.fields.size() != section.fields.size()) { return false; }
        for (const FieldSpec& field : section.fields) {
            bool matched = false;
            for (const DefaultField& value : supplied.fields) {
                if (value.key != field.key) { continue; }
                if (!ct::value_matches_spec(field, value)) { return false; }
                matched = true;
            }
            if (!matched) { return false; }
        }

        // A deployment that leaves a section pointing at a nonexistent image is
        // a broken page, which is worse than the state it booted from.
        if (supplied.images.size() != section.images.size()) { return false; }
        for (const ImageSpec& image : section.images) {
            bool found = false;
            for (const DefaultImage& candidate : supplied.images) {
                if (candidate.slot != image.slot) { continue; }
                if (candidate.file.empty()) { return false; }
                found = true;
            }
            if (!found) { return false; }
        }
    }
    return true;
}

[[nodiscard]] constexpr const SectionDefaults* find_defaults(
    std::span<const SectionDefaults> defaults, std::string_view key) noexcept {
    for (const SectionDefaults& candidate : defaults) {
        if (candidate.key == key) { return &candidate; }
    }
    return nullptr;
}

}  // namespace anvil::sections
