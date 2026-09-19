#pragma once

// Bootstrap: a fresh database renders every page with no manual steps.
//
// --- insert-if-absent, never overwrite --------------------------------------
//
// Bootstrap runs at every boot, from every instance, concurrently. It must
// therefore be idempotent AND must never replace existing content: a boot path
// that resets sections turns every deploy into a content wipe. The guard is
// `insert_one` plus tolerance of the duplicate-key error, NOT a read followed by
// a write — a read-then-write between N booting instances is a race whose loser
// silently overwrites the winner.
//
// --- default images arrive through a hook -----------------------------------
//
// Defaults cannot reference uploaded media ids: on a fresh database there are
// none. They ship as FILES and have to be registered through the media pipeline,
// which needs an image encoder — and the section row lifecycle must not depend
// on whether this build has one. A deployment serving already-stored objects
// with no encoder still has to be able to create and read its sections.
//
// So the file-to-id step is a callable the caller supplies.
// anvil/sections/default_images.h ships one built over MediaService and the
// upload sink, in a translation unit compiled only when the image subsystem is
// on; an application with no images at all passes nothing.

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/sections/content.h"
#include "anvil/sections/defaults.h"
#include "anvil/sections/registry.h"
#include "anvil/sections/repository.h"

namespace anvil::sections {

// Turns one default-image FILE NAME into a stored media id, registering it if it
// is not stored yet. nullopt means the file was missing or could not be
// registered, which is NOT fatal — see BootstrapReport::images_missing.
//
// BLOCKING and filesystem-touching. It must be called before a transaction is
// opened, never inside one: the database can roll back and an image transcode
// cannot.
using DefaultImageResolver = std::function<std::optional<Uuid>(std::string_view file)>;

struct BootstrapReport final {
    std::size_t sections_created;
    std::size_t sections_present;
    std::size_t images_registered;
    // Slots whose file was missing or unreadable ON THIS BOOT. Not fatal — the
    // section is still created, with text and without that image — but LOUD,
    // because the page will render with a gap until somebody fixes the
    // deployment.
    //
    // This counts RESOLVER FAILURES and nothing else. Read
    // `image_slots_unbound` for the state of the site.
    std::size_t images_missing;
    // Declared slots with nothing bound in STORAGE once this boot is finished —
    // the question an operator actually has, which the field above cannot
    // answer.
    //
    // The two disagree whenever a section already existed. Bootstrap binds an
    // image only into a section it CREATES, so a database seeded before the
    // defaults tree existed resolves every file successfully on every later boot
    // — `images_missing == 0`, `images_registered` climbing — while every one of
    // its slots stays empty. The resolver did its work, the files were
    // registered and pinned, nothing was wrong, and no section points at any of
    // them.
    //
    // Fixing that by having bootstrap UPDATE an existing section is the one
    // thing it must not do: a boot path that overwrote stored content would make
    // every deploy a content wipe, which is the whole reason insert-if-absent is
    // the rule. Attaching defaults to a populated database stays a deliberate,
    // out-of-band act. What changes is that the boot log stops saying it already
    // happened.
    //
    // Non-zero is not always wrong — a slot a staff member deliberately left
    // empty counts here too, and there is no stored difference between that and
    // one nobody ever filled. It is the number to LOOK at, not the number to
    // alert on.
    //
    // Found by the first application built on this library, which added its
    // defaults tree one phase after its sections and spent a while believing the
    // report.
    std::size_t image_slots_unbound;
};

// `default_content` plus the media ids for its image slots, canonicalised.
//
// Exposed separately from the loop below because anything that RESTORES a
// section to its shipped state needs the same content, and "what a restore
// produces" and "what a fresh database gets" must be the same by construction
// rather than by review.
//
// `resolve` may be empty, in which case every slot counts as missing and the
// section is created with its text only.
[[nodiscard]] SectionContent resolve_default_content(const SectionSpec& spec,
                                                     const SectionDefaults& defaults,
                                                     const DefaultImageResolver& resolve,
                                                     BootstrapReport& report);

// Creates the Published document of every registry section that does not have
// one. `registry` and `defaults` are positionally identical, which
// defaults_match_registry() proved at compile time, so this walks both by index
// and looks nothing up.
//
// Finishes by READING BACK what the registry declares image slots for, which is
// where BootstrapReport::image_slots_unbound comes from. One indexed query, at
// boot, and a failure of it fails the whole call: a bootstrap that cannot see
// the state it just wrote has not finished, and returning a report whose newest
// number silently reads zero is the exact defect that number exists to close.
//
// BLOCKING. Boot only, before any listener starts; never a pool task and never a
// request.
[[nodiscard]] Result<BootstrapReport> bootstrap_sections(
    mongocxx::client& client, const SectionRepository& sections,
    std::span<const SectionSpec> registry, std::span<const SectionDefaults> defaults,
    const DefaultImageResolver& resolve, const Uuid& actor);

}  // namespace anvil::sections
