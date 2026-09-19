#pragma once

// What an audit row is, and what it deliberately is not.
//
// With stealth 404s hiding failures from clients and hard deletes destroying
// evidence, an append-only audit log is the only remaining forensic surface —
// and losing the 401/403 signal from client-visible responses is only an
// acceptable trade because this preserves it server-side.
//
// Rows carry IDENTIFIERS and CODES. Never a payload, never a token, never a
// submitted value, never personal data (ENGINEERING_RULES.md §5). That bound is structural
// rather than a rule somebody follows: there is no free-form field on the row,
// so there is nowhere for one to be put.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "anvil/audit/action.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"

namespace anvil::audit {

namespace audit_fields {
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kAt = "at";
inline constexpr std::string_view kActor = "actor";
inline constexpr std::string_view kSubject = "sub";
inline constexpr std::string_view kAction = "act";
inline constexpr std::string_view kCode = "code";
inline constexpr std::string_view kSucceeded = "ok";
inline constexpr std::string_view kIp = "ip";
inline constexpr std::string_view kFromState = "from";
inline constexpr std::string_view kToState = "to";
// How many times this event occurred inside one flush window. OMITTED on disk at
// one, so the ordinary row costs nothing to store.
inline constexpr std::string_view kRepeats = "n";
}  // namespace audit_fields

struct AuditEntry final {
    std::optional<Uuid>          actor;
    std::optional<Uuid>          subject;
    // What the thing was before, and what it became. Absent for every action
    // with no before and no after, and OMITTED on disk rather than written null,
    // so a row with nothing to say costs nothing to store.
    //
    // Two small integers rather than a free-form detail string, and that bound
    // is the point: a string is where somebody eventually puts a submitted value
    // or a token, and this collection is read widely. Their MEANING is decided
    // by `action` — which is why they are named for their role rather than for
    // any one enumeration.
    std::optional<std::int32_t>  from_state;
    std::optional<std::int32_t>  to_state;
    std::array<std::uint8_t, 16> ip;
    AuditAction                  action;
    // The INTERNAL code, even when the client saw a 404. This field is the
    // entire point of the collection.
    ErrorCode                    code;
    bool                         succeeded;
    // NOT STORED, and the only field here that is not. It exists so two
    // consecutive denials can be told apart before they are folded into one row:
    // "the client saw a 404" and "the client saw a 403" are different facts
    // about the same `code`.
    //
    // Carried on the entry rather than passed beside it because a fold key
    // assembled by the caller is a fold key one caller assembles differently.
    // Meaningless on any action that is not a denial, where it is ignored.
    bool                         stealthed = false;
};

// An entry paired with the instant it HAPPENED.
//
// The time cannot be taken at write time once writes are batched: a row would
// then be stamped with the moment the flush reached the database, which under
// exactly the load an auditor cares about is the moment furthest from the truth.
// It is stamped when the event occurs and carried through.
struct AuditRow final {
    AuditEntry entry;
    db::TimeMs at;
    // One for every row that was not folded. A rate per source is what an
    // intrusion view wants to read back, and a count IS the rate — the millionth
    // identical denial adds nothing an auditor can use, at the cost of the one
    // row that would have.
    std::uint32_t repeats = 1;
};

// The compound cursor, `(at, _id)` descending. The PAIR and not `at` alone: rows
// are written in batches and a whole batch can share one millisecond, so an
// instant-only cursor would serve part of a flush twice and skip the rest.
//
// `_id` alone would not do either, however time-ordered a v7 uuid is. The id is
// minted when the batch is ENCODED, which is flush time; `at` is stamped when
// the event happened. Under the burst this collection exists to describe, those
// are the two instants furthest apart.
struct AuditCursor final {
    Uuid       id;
    db::TimeMs at;
};

// What an investigator may narrow by. Every member is optional and absent means
// "no bound", so the empty query is the whole log newest-first.
//
// There is deliberately no filter on the code, on success, or on the network.
// The first two are answered by the action a caller already filtered on, and the
// third is a GROUPING a screen does over a column each row carries — making it a
// filter would mean an index over an address prefix, which is a scan dressed as
// a lookup.
// Every member carries a default, so a caller may name only what it is
// narrowing by — see AccountQuery in anvil/identity/users.h for why that matters
// beyond convenience.
struct AuditQuery final {
    std::optional<AuditCursor> after = std::nullopt;
    // `at >= from` and `at < to`. Half-open, so two consecutive windows tile
    // without serving a row twice.
    std::optional<db::TimeMs>  from = std::nullopt;
    std::optional<db::TimeMs>  to = std::nullopt;
    std::optional<Uuid>        actor = std::nullopt;
    // The row's subject: who or what the action was done TO. Named for the
    // question a screen asks ("everything that happened to this account") rather
    // than for the field.
    std::optional<Uuid>        target = std::nullopt;
    std::optional<AuditAction> action = std::nullopt;
    std::int32_t               limit = 0;
};

// One row as it leaves the database. Structurally an AuditEntry plus the two
// things a stored row has that an unstored one does not — its id and its instant
// — and NOTHING else, which is the point: there is no field here for a payload,
// a token or a submitted value, because there is no such field on disk. An
// endpoint cannot leak what was never written.
struct AuditView final {
    std::optional<Uuid>          actor;
    std::optional<Uuid>          subject;
    std::optional<std::int32_t>  from_state;
    std::optional<std::int32_t>  to_state;
    // The FULL sixteen bytes, coarsened by the caller rather than here. The
    // repository returns what is stored; which prefix a screen may see is a
    // presentation rule, and identity::coarse_network_of is the one place it is
    // decided — shared with the session listing so a log line and a screen agree
    // on what a source is.
    std::array<std::uint8_t, 16> ip;
    Uuid                         id;
    db::TimeMs                   at;
    // ONE for an absent field, which is every row written before the count
    // existed. Retention here outlives any deploy cycle, so a reader that
    // treated absence as zero would report that whole history as having never
    // happened.
    std::uint32_t                repeats;
    AuditAction                  action;
    ErrorCode                    code;
    bool                         succeeded;
};

struct AuditPage final {
    std::vector<AuditView>     rows;
    std::optional<AuditCursor> next;
};

}  // namespace anvil::audit
