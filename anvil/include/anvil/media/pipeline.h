#pragma once

// The image stage of an upload: probe, normalise, publish the master, derive
// every variant.
//
// Its own translation unit rather than a method on MediaService, because it is
// the ONLY part of the media subsystem that needs libvips. A deployment built
// without the image subsystem still stores, serves and deletes objects; it
// simply cannot produce new derived sizes. Putting this on the service would
// make the row lifecycle — including deletion — depend on whether this build can
// encode anything.
//
// Available only when ANVIL_WITH_VIPS is on. The header is guarded so a caller
// that forgets gets a missing declaration at the call site rather than a link
// error with no line number.

#ifdef ANVIL_HAS_VIPS

#include "anvil/core/result.h"
#include "anvil/fs/namespace.h"
#include "anvil/fs/upload.h"
#include "anvil/media/service.h"

namespace anvil::media {

// Touches no database at all. The caller must keep `sink`'s temporary file alive
// across this call: it owns the raw upload being read, and its destructor
// removes it.
//
// BLOCKING and CPU-bound. cpu_pool only — one of these on a db_pool thread makes
// every query in the process wait behind a transcode, and one on a loop thread
// stalls every connection that loop owns (ENGINEERING_RULES.md §4).
[[nodiscard]] Result<ProcessedMedia> process(fs::Ns ns, const fs::UploadResult& upload);

}  // namespace anvil::media

#endif  // ANVIL_HAS_VIPS
