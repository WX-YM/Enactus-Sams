#pragma once

// The one-shot import from the pre-anvil backend's collections (`users`,
// `teams`, `applications`, `content`, `form_schema`, `form_submissions` in the
// same database), run once by `enactus_migrate --legacy` on the deployment
// server. The legacy collections are read, never written or dropped: the
// operator removes them after checking the result.
//
// Passwords are never seen. A legacy `$argon2id$` record is rewritten offline
// as an anvil prehash record over the same salt and parameters
// (PrehashHasher::wrap_legacy), so every existing user signs in with the
// password they already have and the browser-side prehash computes exactly the
// key the wrapped record expects.
//
// Idempotent: an account whose email exists is skipped, an application whose
// email has a row is skipped, a team that exists is merged rather than
// duplicated, and a form imported once is recognised by its title.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/perm_set.h"
#include "anvil/core/types.h"

namespace enactus::legacy {

// --- the pure mappings, unit-tested in tests/legacy_test.cc -----------------

// The legacy permission names ("dashboard", "applications", "form_maker",
// "teams", "content", "gallery", "users") to the grantable bits. Unknown names
// are dropped; implied bits are not added here (the caller applies
// with_implied when it stores the set).
[[nodiscard]] anvil::PermSet permissions_from(const std::vector<std::string>& names);

// The legacy role label to one Access Control offers. `vice_manager` was a
// spelling of `vice manager`; `superadmin` and `admin` become `high board`;
// anything unrecognised becomes `member`.
[[nodiscard]] std::string role_from(std::string_view legacy_role);

// The legacy superadmin rule: role `superadmin`, or the built-in admin address.
[[nodiscard]] bool is_superadmin(std::string_view role, std::string_view email);

// A legacy single `name` split at the first run of spaces. The last name is
// required by the application kind, so a one-word name keeps a `-`.
struct PersonName final {
    std::string first;
    std::string last;
};
[[nodiscard]] PersonName split_name(std::string_view name);

// A legacy status, or `pending` for anything the new table does not know.
[[nodiscard]] std::string status_from(std::string_view legacy_status);

// A form option label as a stored option value: [A-Za-z0-9_-], at most 64
// bytes, never empty (`opt<n>` when the label has no usable characters).
[[nodiscard]] std::string option_value(std::string_view label, std::size_t index);

// The legacy field type to a field-type wire name (field_types.h).
[[nodiscard]] std::string_view field_type_from(std::string_view legacy_type);

// Where a legacy content key goes: a section and one of its fields. nullopt for
// a key that has no home (images, team lists — those are handled separately).
struct SectionTarget final {
    std::string_view section;
    std::string_view field;
};
[[nodiscard]] std::optional<SectionTarget> section_target(std::string_view legacy_key);

// A legacy image URL to a (directory, file) pair under one of the roots, or
// nullopt when the URL is remote, malformed, or tries to leave its root. Only
// the shapes the old backend wrote are accepted: `/assets/...` under DOC_ROOT
// and `/uploads/...` under the legacy uploads directory.
struct LocalFile final {
    std::string directory;
    std::string file;
};
[[nodiscard]] std::optional<LocalFile> local_file_for(std::string_view url, std::string_view doc_root,
                                                      std::string_view uploads_dir);

// --- the import -------------------------------------------------------------

struct Report final {
    std::size_t users_imported = 0;
    std::size_t users_skipped = 0;      // already present
    std::size_t users_refused = 0;      // no usable password record
    std::size_t teams_created = 0;
    std::size_t teams_merged = 0;
    std::size_t members_imported = 0;
    std::size_t applications_imported = 0;
    std::size_t applications_skipped = 0;
    std::size_t section_fields = 0;
    std::size_t section_fields_refused = 0;
    std::size_t gallery_photos = 0;
    std::size_t images_missing = 0;
    std::size_t forms_imported = 0;
    std::size_t responses_imported = 0;
    std::vector<std::string> notes;     // one line per refused or skipped item
};

struct Options final {
    std::string doc_root;
    std::string uploads_dir;
};

// Requires install_services(). Blocking; run from the migration tool only.
[[nodiscard]] Report import_all(mongocxx::client& client, const Options& options);

}  // namespace enactus::legacy
