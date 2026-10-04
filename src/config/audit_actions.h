#pragma once

// The audit actions this application records (anvil docs/01-seams.md §9).
//
// The value is STORED in every audit row and read back long after the deploy
// that wrote it: values are 1..N with no gap, append only, never renumbered.
// The name is what the dashboard's activity log shows.

#include <array>
#include <cstdint>

#include "anvil/audit/action.h"

namespace enactus {

enum class Action : std::int32_t {
    AccessDenied          = 1,
    StaffCreated          = 2,
    StaffUpdated          = 3,
    StaffDisabled         = 4,
    ApplicationReceived   = 5,
    ApplicationReviewed   = 6,
    ApplicationDeleted    = 7,
    TeamCreated           = 8,
    TeamUpdated           = 9,
    TeamDeleted           = 10,
    RosterUpdated         = 11,
    SectionPublished      = 12,
    GalleryChanged        = 13,
    MediaUploaded         = 14,
    FormCreated           = 15,
    FormUpdated           = 16,
    FormDeleted           = 17,
    FormResponseDeleted   = 18,
    FormResponsesExported = 19,
};

inline constexpr std::array<anvil::audit::AuditActionSpec, 19> kAuditActions{{
    {"AccessDenied",          1,  anvil::audit::AuditClass::Traffic},
    {"StaffCreated",          2,  anvil::audit::AuditClass::Change},
    {"StaffUpdated",          3,  anvil::audit::AuditClass::Change},
    {"StaffDisabled",         4,  anvil::audit::AuditClass::Change},
    {"ApplicationReceived",   5,  anvil::audit::AuditClass::Traffic},
    {"ApplicationReviewed",   6,  anvil::audit::AuditClass::Change},
    {"ApplicationDeleted",    7,  anvil::audit::AuditClass::Change},
    {"TeamCreated",           8,  anvil::audit::AuditClass::Change},
    {"TeamUpdated",           9,  anvil::audit::AuditClass::Change},
    {"TeamDeleted",           10, anvil::audit::AuditClass::Change},
    {"RosterUpdated",         11, anvil::audit::AuditClass::Change},
    {"SectionPublished",      12, anvil::audit::AuditClass::Change},
    {"GalleryChanged",        13, anvil::audit::AuditClass::Change},
    {"MediaUploaded",         14, anvil::audit::AuditClass::Change},
    {"FormCreated",           15, anvil::audit::AuditClass::Change},
    {"FormUpdated",           16, anvil::audit::AuditClass::Change},
    {"FormDeleted",           17, anvil::audit::AuditClass::Change},
    {"FormResponseDeleted",   18, anvil::audit::AuditClass::Change},
    {"FormResponsesExported", 19, anvil::audit::AuditClass::Change},
}};

static_assert(anvil::audit::audit_table_is_well_formed(kAuditActions),
              "duplicate action value or name, empty name, or a non-positive value");
static_assert(static_cast<std::int32_t>(Action::FormResponsesExported) ==
                  static_cast<std::int32_t>(kAuditActions.size()),
              "the action values are 1..N with no gap; naming one is not optional");

[[nodiscard]] constexpr anvil::audit::AuditAction audit(Action action) noexcept {
    return anvil::audit::AuditAction::of(action);
}

}  // namespace enactus
