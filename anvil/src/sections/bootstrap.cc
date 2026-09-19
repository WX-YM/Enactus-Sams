#include "anvil/sections/bootstrap.h"

#include <cstddef>
#include <string>
#include <utility>

#include "anvil/db/versioned.h"
#include "anvil/sections/payload.h"

namespace anvil::sections {

SectionContent resolve_default_content(const SectionSpec& spec,
                                       const SectionDefaults& defaults,
                                       const DefaultImageResolver& resolve,
                                       BootstrapReport& report) {
    SectionContent content = default_content(spec, defaults);
    for (const DefaultImage& image : defaults.images) {
        const std::optional<Uuid> id = resolve ? resolve(image.file) : std::nullopt;
        if (!id.has_value()) {
            ++report.images_missing;
            continue;
        }
        ++report.images_registered;
        content.set_image(std::string{image.slot}, *id);
    }
    return canonicalise(spec, content);
}

Result<BootstrapReport> bootstrap_sections(mongocxx::client& client,
                                           const SectionRepository& sections,
                                           std::span<const SectionSpec> registry,
                                           std::span<const SectionDefaults> defaults,
                                           const DefaultImageResolver& resolve,
                                           const Uuid& actor) {
    BootstrapReport report{};
    // Positional correspondence is a compile-time property of the two tables
    // (defaults_match_registry), but this function takes spans and a caller can
    // hand it two that were never asserted together. Refusing is the only safe
    // answer: walking by index across mismatched tables would write one
    // section's defaults into another section's document.
    if (registry.size() != defaults.size()) { return fail(ErrorCode::Internal); }

    for (std::size_t i = 0; i < registry.size(); ++i) {
        const SectionSpec& spec = registry[i];
        const SectionDefaults& supplied = defaults[i];
        if (spec.key != supplied.key) { return fail(ErrorCode::Internal, spec.key); }

        SectionDocument document{};
        document.content = resolve_default_content(spec, supplied, resolve, report);
        document.etag = content_etag(spec, document.content);
        document.updated_by = actor;
        document.version = repo::kInitialVersion;

        // PUBLISHED only. A fresh database has no drafts, and creating an empty
        // one would put a document in the draft state that nobody authored.
        const Result<bool> created =
            sections.insert_if_absent(client, spec, SectionState::Published, document, actor);
        if (!created) { return created.error(); }
        if (created.value()) {
            ++report.sections_created;
        } else {
            ++report.sections_present;
        }
    }

    // Everything above this line describes what THIS BOOT did. The one number an
    // operator reads to decide whether the site is whole has to come from the
    // documents instead — a section that already existed kept whatever it was
    // created with, and the resolver's success says nothing about it.
    //
    // After the loop, never inside it: the count has to see the rows this call
    // just inserted as well as the ones it found.
    const Result<std::size_t> unbound =
        sections.count_unbound_image_slots(client, registry, SectionState::Published);
    if (!unbound) { return unbound.error(); }
    report.image_slots_unbound = unbound.value();

    return report;
}

}  // namespace anvil::sections
