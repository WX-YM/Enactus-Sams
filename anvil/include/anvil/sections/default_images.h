#pragma once

// Registering a section's shipped default images: the batteries behind
// sections::DefaultImageResolver.
//
// Available only when ANVIL_WITH_VIPS is on, and its own translation unit for
// the reason the media pipeline is: registering a file means probing,
// normalising and deriving variants for it, and the section ROW lifecycle must
// not depend on whether this build can encode. A deployment with no image
// subsystem still creates, reads and writes its sections; it simply has no
// default pictures, and `bootstrap_sections` reports those slots as missing
// rather than failing.
//
// --- idempotence comes from the content hash, not from a marker -------------
//
// Re-running bootstrap against a populated database finds the identical bytes
// already stored and reuses that row rather than storing a second copy. There is
// no "have I run before" flag to get wrong.
//
// --- the rows are PINNED ----------------------------------------------------
//
// A default image's reference count is bumped by media::kPinnedRefs once, so the
// count can never reach zero and the collector can never reclaim it. A
// deployment that leaves a section pointing at a nonexistent image is a broken
// page, which is worse than the state it booted from.

#ifdef ANVIL_HAS_VIPS

#include <cstdint>
#include <optional>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/core/types.h"
#include "anvil/fs/namespace.h"
#include "anvil/media/service.h"
#include "anvil/sections/bootstrap.h"

namespace anvil::sections {

// Default images are ordinary photographs, not a special format. The cap is
// generous for a hero banner and far below the image subsystem's own limits, so
// a mis-sized deployment file is rejected before a transcode rather than after
// one.
inline constexpr std::uint64_t kMaxDefaultImageBytes = 8ULL * 1024 * 1024;

// Registers one file from the read-only defaults tree and returns its media id,
// or nullopt when the file is absent or cannot be registered.
//
// NOT an error the caller propagates: a missing decorative image must not stop a
// deployment from booting with correct text.
//
// The file is streamed through the ordinary upload sink, so it passes exactly
// the same magic-number sniffing, size cap and hashing an uploaded one does. The
// claimed content type is deliberately empty: there is no client header to
// compare against, and the sniffed type is the only one that ever mattered.
//
// BLOCKING and filesystem-touching. Boot only, and never inside a transaction:
// the database can roll back and an image transcode cannot.
[[nodiscard]] std::optional<Uuid> register_default_image(mongocxx::client& client,
                                                         media::MediaService& media, fs::Ns ns,
                                                         std::string_view defaults_dir,
                                                         std::string_view file,
                                                         const Uuid& actor);

// `register_default_image` bound into the shape bootstrap_sections takes.
//
// Every captured reference must outlive the returned callable — which at boot
// they all do, but the resolver is a std::function and will happily outlive a
// temporary, so this is the one place to say so.
[[nodiscard]] DefaultImageResolver default_image_resolver(mongocxx::client& client,
                                                          media::MediaService& media, fs::Ns ns,
                                                          std::string_view defaults_dir,
                                                          const Uuid& actor);

}  // namespace anvil::sections

#endif  // ANVIL_HAS_VIPS
