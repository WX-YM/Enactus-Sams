#pragma once

// The index catalogue (anvil docs/09-mongodb.md, docs/18-data-migrations.md).
// Applied by `enactus_migrate`, never at boot: an index build holds its
// collection, and every db_pool thread would queue behind it.
//
// Field names come from anvil's own field-constant headers, so an index and the
// repository that queries through it cannot spell a field two ways.

#include <array>
#include <cstdint>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/analytics/event.h"
#include "anvil/audit/record.h"
#include "anvil/db/migrations.h"
#include "anvil/entries/document.h"
#include "anvil/forms/repository.h"
#include "anvil/identity/sessions.h"
#include "anvil/identity/user_fields.h"
#include "anvil/identity/verification.h"
#include "anvil/media/record.h"

namespace enactus {

namespace uf = anvil::identity::fields;
namespace sf = anvil::identity::session_fields;
namespace vf = anvil::identity::verification_fields;
namespace af = anvil::audit::audit_fields;
namespace mf = anvil::media::media_fields;
namespace ff = anvil::forms::form_fields;
namespace aef = anvil::analytics::event_fields;
namespace asf = anvil::analytics::session_fields;
namespace enf = anvil::entries::entry_fields;

[[nodiscard]] inline bsoncxx::document::value field_present(std::string_view field) {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(field),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

[[nodiscard]] inline bsoncxx::document::value entry_slug_present_only() {
    return field_present(enf::kSlug);
}
[[nodiscard]] inline bsoncxx::document::value one_per_user_only() {
    return field_present(ff::kUnique);
}
[[nodiscard]] inline bsoncxx::document::value bound_media_only() {
    return field_present(ff::kMedia);
}
[[nodiscard]] inline bsoncxx::document::value pii_present_only() {
    return field_present(ff::kPiiIndex);
}
[[nodiscard]] inline bsoncxx::document::value media_edits_only() {
    return field_present(mf::kSource);
}
[[nodiscard]] inline bsoncxx::document::value analytics_subject_present_only() {
    return field_present(aef::kSubject);
}

inline constexpr std::array<anvil::db::IndexSpec, 24> kIndexes{{
    // accounts: one account per email. Staff have no usernames or phones, so
    // neither carries an index — a unique index over an absent field would be
    // a null every account shares.
    {{{{uf::kEmailNormalised, 1}}}, "accounts", "accounts_email_unique", nullptr, -1, 1, true,
     false},
    {{{{uf::kUserType, 1}, {uf::kId, 1}}}, "accounts", "accounts_type_id", nullptr, -1, 2,
     false, false},

    {{{{sf::kRefreshHash, 1}}}, "user_sessions", "sessions_refresh", nullptr, -1, 1, false,
     false},
    {{{{sf::kPreviousHash, 1}}}, "user_sessions", "sessions_previous", nullptr, -1, 1, false,
     true},
    {{{{sf::kUserId, 1}, {sf::kLastSeen, -1}}}, "user_sessions", "sessions_user_seen", nullptr,
     -1, 2, false, false},
    {{{{sf::kExpiresAt, 1}}}, "user_sessions", "sessions_ttl", nullptr, 0, 1, false, false},

    {{{{vf::kAddressHash, 1}}}, "email_verifications", "verifications_address", nullptr, -1, 1,
     true, false},
    {{{{vf::kExpiresAt, 1}}}, "email_verifications", "verifications_ttl", nullptr, 0, 1, false,
     false},

    {{{{mf::kNamespace, 1}, {mf::kSha256, 1}}}, "media", "media_ns_hash", nullptr, -1, 2, false,
     false},
    {{{{mf::kNamespace, 1}, {mf::kCreatedAt, -1}, {mf::kId, -1}}}, "media", "media_ns_created",
     nullptr, -1, 3, false, false},
    {{{{mf::kNamespace, 1}, {mf::kSource, 1}, {mf::kEditSha, 1}}}, "media", "media_ns_edit",
     &media_edits_only, -1, 3, true, false},

    {{{{af::kActor, 1}, {af::kAt, -1}, {af::kId, -1}}}, "audit_log", "audit_actor_at", nullptr,
     -1, 3, false, false},
    // The dashboard's activity feed reads newest-first with no actor filter.
    {{{{af::kAt, -1}, {af::kId, -1}}}, "audit_log", "audit_at", nullptr, -1, 2, false, false},

    {{{{enf::kScope, 1}, {enf::kLive, 1}, {enf::kPosition, 1}, {enf::kId, 1}}}, "entries",
     "entries_listing", nullptr, -1, 4, false, false},
    {{{{enf::kKind, 1}, {enf::kSlug, 1}}}, "entries", "entries_slug_unique",
     &entry_slug_present_only, -1, 2, true, false},

    {{{{ff::kCreator, 1}, {ff::kStatus, 1}}}, "form_definitions", "forms_creator_status",
     nullptr, -1, 2, false, false},
    {{{{ff::kForm, 1}, {ff::kSubmittedAt, -1}, {ff::kId, -1}}}, "form_responses",
     "responses_form_time", nullptr, -1, 3, false, false},
    {{{{ff::kForm, 1}, {ff::kPiiIndex, 1}}}, "form_responses", "responses_form_pii",
     &pii_present_only, -1, 2, false, false},
    {{{{ff::kForm, 1}, {ff::kUnique, 1}}}, "form_responses", "responses_one_per_user",
     &one_per_user_only, -1, 2, true, false},
    {{{{ff::kMedia, 1}}}, "form_responses", "responses_media_unique", &bound_media_only, -1, 1,
     true, false},

    {{{{aef::kAt, 1}, {aef::kId, 1}}}, "analytics_events", "events_at_id", nullptr, -1, 2,
     false, false},
    {{{{aef::kExpiresAt, 1}}}, "analytics_events", "events_ttl", nullptr, 0, 1, false, false},
    {{{{asf::kExpiresAt, 1}}}, "analytics_sessions", "sessions_ttl", nullptr, 0, 1, false,
     false},
    {{{{"_id.code", 1}, {"_id.gran", 1}, {"_id.bucket", 1}}}, "analytics_rollups",
     "rollups_code_gran_bucket", nullptr, -1, 3, false, false},
}};

static_assert(anvil::db::catalogue_is_well_formed(kIndexes),
              "empty name or collection, a zero or over-long key count, a direction that is "
              "neither 1 nor -1, or two indexes sharing a name on one collection");

inline constexpr std::array<anvil::db::RetiredIndex, 0> kRetiredIndexes{};

// Bumped whenever kIndexes changes.
inline constexpr std::int32_t kSchemaVersion = 1;

}  // namespace enactus
