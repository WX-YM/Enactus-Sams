#pragma once

// Core value types shared by every layer.
//
// What is NOT here is the point. In the system anvil was extracted from, this
// header also carried the application's 38-entry permission enum and its name
// table — and because result.h includes this file and nearly everything includes
// result.h, those 38 application-specific names were transitively visible from
// every file in the tree. Splitting them out was the single highest-leverage move
// in the extraction.
//
// So: identifiers, classifications and error codes live here. Permissions live in
// anvil/core/perm_catalogue.h and are declared by the application. Locales live in
// anvil/core/locale.h, likewise. See docs/01-seams.md.

#include <array>
#include <cstdint>

namespace anvil {

// --- Identifiers ----------------------------------------------------------
// 16 raw bytes, stored as BSON BinData subtype 4. Never a 36-character string:
// that is 2.25x the storage and turns every comparison into a string compare
// rather than a 16-byte memcmp. Across `_id` plus every index that references
// one, this is the largest single storage saving available in a typical schema.
using Uuid = std::array<std::uint8_t, 16>;

inline constexpr Uuid kNilUuid{};

[[nodiscard]] constexpr bool is_nil(const Uuid& id) noexcept {
    for (const std::uint8_t byte : id) {
        if (byte != 0U) { return false; }
    }
    return true;
}

// --- User classification --------------------------------------------------
// Stored as int32. APPEND ONLY — renumbering silently reinterprets every
// existing row.
//
// anvil owns these rather than leaving them to the application because the
// access-control decision itself reads them: `SuperAdmin` is checked explicitly
// rather than by testing for an all-ones permission mask, so that "holds every
// permission" and "is a superadmin" stay distinguishable in an audit record and
// no amount of bit-fiddling can synthesise the second from the first.
enum class UserType : std::uint8_t { Client = 0, Staff = 1, FullControl = 2, SuperAdmin = 3 };

inline constexpr UserType kMaxUserType = UserType::SuperAdmin;

// Stored as int32 in the user row. Anything other than Active fails
// authentication with the SAME response and the SAME timing as a wrong password:
// a disabled account that answers differently is an account-enumeration oracle.
//
// APPEND ONLY, for the reason above.
enum class UserStatus : std::uint8_t {
    Disabled = 0,
    Active = 1,
    Locked = 2,
    // Registered, but the contact address is unproven. It authenticates exactly
    // as Disabled and Locked do — that is, not at all, through the same
    // non-Active predicate and after the same password verify — so a pending
    // account is indistinguishable from a wrong password. Answering "please
    // verify your email" here would hand back the enumeration oracle the whole
    // signup path exists to deny.
    PendingVerification = 3,
};

inline constexpr UserStatus kMaxUserStatus = UserStatus::PendingVerification;

// --- Errors ---------------------------------------------------------------
// Exactly one code -> HTTP status table exists, in anvil/http/errors.h.
//
// Unauthenticated and Forbidden never reach the client on a Stealth route: the
// filter maps both to a byte-identical 404. They still exist internally so the
// audit log records the true reason — losing intrusion signal to stealth is a
// real cost of that design, and logging is what pays it back.
//
// Stored as int32 in audit rows that are read back long after the enum grew, so
// this list is APPEND ONLY and kMaxErrorCode exists to give a decoder a bound it
// can state rather than a belief about how long the list is.
enum class ErrorCode : std::uint16_t {
    Ok = 0,
    Unauthenticated,
    Forbidden,
    NotFound,
    CapabilityRequired,
    CapabilityInvalid,
    ValidationFailed,
    Conflict,
    VersionMismatch,
    RateLimited,
    PayloadTooLarge,
    UnsupportedMedia,
    // Reached no server, or was shed deliberately: the request may be retried.
    // Distinct from Internal so a circuit breaker and a client can tell a fault
    // from an outage.
    ServiceUnavailable,
    Internal,
    // 507. The upload path refuses BEFORE accepting bytes when free space is
    // below the configured floor.
    InsufficientStorage,
};

inline constexpr ErrorCode kMaxErrorCode = ErrorCode::InsufficientStorage;

}  // namespace anvil
