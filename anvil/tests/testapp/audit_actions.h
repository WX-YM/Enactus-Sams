#pragma once

// The reference application's audit vocabulary.
//
// Compiled by every build of the test suite, so the worked example in
// docs/01-seams.md is a file that must keep compiling rather than a snippet that
// can rot.
//
// STORED as int32 and read back by dashboards long after this build is gone.
// APPEND ONLY: never renumber, never reuse a retired value. Retention is
// measured in hundreds of days, which is longer than any deploy cycle, so a row
// written a year ago carries the numbering of the build that wrote it.

#include <array>
#include <cstdint>

#include "anvil/audit/action.h"

namespace testapp {

enum class Action : std::int32_t {
    LoginSucceeded = 1,
    LoginFailed = 2,
    SignupSucceeded = 3,
    SessionRotated = 4,
    // The single highest-signal row this collection holds: an old refresh token
    // presented after its grace window closed means a rotated credential was
    // replayed, which means it leaked.
    RefreshTokenReplayed = 5,
    // Every stealth drop, carrying the TRUE code rather than the 404 the client
    // saw. This is the row that makes stealth an acceptable trade at all.
    AccessDenied = 6,
    PermissionEpochBumped = 7,
    StaffPermissionChanged = 8,
    UserStatusChanged = 9,
    CapabilityConsumed = 10,
    MediaUploaded = 11,
    MediaDeleted = 12,
};

// The CLASSIFICATION is the load-bearing column, not a label. AccessDenied is
// the only Traffic row here, and that is what makes a flood compressible and a
// permission change undroppable — get it backwards and a burst of denials evicts
// the one row nothing else has a copy of (anvil/audit/buffer.h).
inline constexpr std::array<anvil::audit::AuditActionSpec, 12> kAuditActions{{
    {"LoginSucceeded", 1, anvil::audit::AuditClass::Change},
    {"LoginFailed", 2, anvil::audit::AuditClass::Change},
    {"SignupSucceeded", 3, anvil::audit::AuditClass::Change},
    {"SessionRotated", 4, anvil::audit::AuditClass::Change},
    {"RefreshTokenReplayed", 5, anvil::audit::AuditClass::Change},
    {"AccessDenied", 6, anvil::audit::AuditClass::Traffic},
    {"PermissionEpochBumped", 7, anvil::audit::AuditClass::Change},
    {"StaffPermissionChanged", 8, anvil::audit::AuditClass::Change},
    {"UserStatusChanged", 9, anvil::audit::AuditClass::Change},
    {"CapabilityConsumed", 10, anvil::audit::AuditClass::Change},
    {"MediaUploaded", 11, anvil::audit::AuditClass::Change},
    {"MediaDeleted", 12, anvil::audit::AuditClass::Change},
}};

static_assert(anvil::audit::audit_table_is_well_formed(kAuditActions),
              "duplicate action value or name, empty name, or a non-positive value");

static_assert(static_cast<std::int32_t>(Action::MediaDeleted) ==
                  static_cast<std::int32_t>(kAuditActions.size()),
              "the action values are 1..N with no gap; naming one is not optional");

// Which action anvil's own denial sink writes. anvil cannot guess it — the
// vocabulary is this table's — and a denial written under a value the table does
// not declare would be classified as a Change and would never coalesce, which is
// the flood case with the shedding policy switched off.
inline constexpr anvil::audit::AuditAction kDenialAction =
    anvil::audit::AuditAction::of(Action::AccessDenied);

static_assert(anvil::audit::audit_class_of(kAuditActions, kDenialAction) ==
                  anvil::audit::AuditClass::Traffic,
              "the denial action must be compressible, or a flood fills the buffer with it");

}  // namespace testapp
