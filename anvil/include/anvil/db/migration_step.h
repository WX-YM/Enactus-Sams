#pragma once

// The vocabulary an application needs to declare its data migrations.
//
// anvil ships the runner, the cursor, the batching, the ledger, the lock and the
// dry run. WHAT moves is a list of the application's own transforms, and this is
// the header those are spelled in (docs/01-seams.md §13). It is the second half
// of the index catalogue in anvil/db/migrations.h: that one declares the indexes,
// this one declares the documents.
//
// --- what this header is allowed to include ---------------------------------
//
// <mongocxx/client-fwd.hpp>, never <mongocxx/client.hpp>: StepContext hands a
// step a client REFERENCE, which needs no complete type, and the same trick for
// the same reason as anvil/timer/job_spec.h.
//
// bsoncxx is a different matter and the doc that asked for "client-fwd and
// nothing more" could not have it: StepFn's own signature names
// bsoncxx::document::view, and std::span requires a complete element type. So
// the bsoncxx value headers are here by necessity. What stays out is the
// DRIVER — nothing here opens a connection, and that is the boundary
// anvil::foundation is protected by.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types/bson_value/value.hpp>
#include <mongocxx/client-fwd.hpp>

#include "anvil/db/collections.h"

namespace anvil::db {

// How the runner walks the collection. Both shapes are `{_id: {$gt: last}}` in
// `_id` order — never skip(n), which is O(n) server-side so the last batch of a
// large collection costs the most at the point the run is most likely to be
// interrupted (ENGINEERING_RULES.md §7).
//
// Two enumerators because two `_id` SHAPES exist, and the difference is what the
// cursor value is allowed to be:
//
//   IdRange          `_id` is a 16-byte UUID. The runner REFUSES a step whose
//                    collection holds anything else, because a cursor proved
//                    against one `_id` shape is a cursor that breaks on the
//                    other — and it breaks by walking a prefix and reporting
//                    success. The ledger also records the millisecond a UUIDv7
//                    cursor has reached, which is the only progress figure an
//                    operator can read without knowing the collection.
//   WholeCollection  `_id` is anything else — a compound document is the case
//                    this exists for. Ordering is still total, so the walk is
//                    identical; there is simply no time bound to derive.
enum class Cursor : std::uint8_t { IdRange = 0, WholeCollection = 1 };

// What a step reports about the batch it was handed. A step never throws: it is
// noexcept, because it runs inside a loop that owns a lease, and an exception
// escaping it would leave the lease held by a process that is no longer running
// the step.
enum class StepOutcome : std::uint8_t {
    // The batch was transformed. Any writes the step asked for are in the
    // context; a step that decided this batch needs no change asks for none.
    Ok = 0,
    // The step cannot proceed. The runner records the step as failed and stops:
    // steps are ordered, and step N+1 was written by somebody who assumed N ran
    // (docs/18-data-migrations.md §8).
    Failed = 1,
};

enum class StepWriteKind : std::uint8_t { Set = 0, Unset = 1 };

// One write a step asked for, addressed by the `_id` of the document it was
// handed.
//
// It carries FIELDS and not an update document, which is the whole idempotence
// argument in one type: the runner is what wraps them in `$set` or `$unset`, so
// a step cannot express a delta and `{$inc: {attempts: 1}}` is not a thing a
// step is able to say. A lease can expire against a process that is alive but
// stalled, so two runners can overlap — with `$set` the double application is a
// no-op, and with `$inc` it is a wrong number nobody can reconstruct
// (docs/18-data-migrations.md §4, §5).
//
// Ordered largest first. bson_value::value and document::value are both
// heap-owning handles; the kind is the tail byte.
struct StepWrite final {
    bsoncxx::types::bson_value::value id;
    bsoncxx::document::value          fields;
    StepWriteKind                     kind;
};

// What a step is handed: a client for READS, and an accumulator for writes.
//
// It never hands out a collection handle to write through. That is what makes
// --dry-run a property of anvil rather than a promise each step makes
// individually: the runner decides whether to execute what was accumulated, so a
// step that cannot write in dry-run mode cannot be made to write by a mistake in
// a step (docs/18-data-migrations.md §7).
//
// A step that needs a second read — a lookup against another collection — takes
// it from client(). A step that filters on anything other than `_id` needs an
// index and a QuerySpec entry like any other query; the runner's own `_id` walk
// is the exception, which is why it is worth naming.
class StepContext final {
public:
    StepContext(mongocxx::client& client, std::string_view database,
                std::string_view collection) noexcept;

    // Reads only. The client is not thread-safe and is valid for the duration of
    // the step call.
    [[nodiscard]] mongocxx::client& client() const noexcept { return *client_; }
    [[nodiscard]] std::string_view database() const noexcept { return database_; }
    [[nodiscard]] std::string_view collection() const noexcept { return collection_; }

    // `fields` is what the document SHOULD BE, never a delta from what was
    // found. Applied as `{$set: fields}` against `{_id: id}`, so running it
    // twice is a no-op.
    void set(bsoncxx::types::bson_value::value id, bsoncxx::document::value fields);

    // Removing a field twice is a no-op for the same reason, which is what makes
    // this the other half of a re-runnable transform. `fields` names the keys;
    // their values are ignored by the server.
    void unset(bsoncxx::types::bson_value::value id, bsoncxx::document::value fields);

    [[nodiscard]] std::span<const StepWrite> writes() const noexcept { return writes_; }
    void clear() noexcept { writes_.clear(); }

private:
    std::vector<StepWrite> writes_;
    mongocxx::client*      client_;      // never null; owned by the runner
    std::string_view       database_;
    std::string_view       collection_;
};

// A plain function pointer: no capture, nothing address-space dependent, and
// usable in the constexpr table below. The same shape as JobHandler, PartialFilterFn
// and FilterFn, and for the same reason.
//
// The views borrow the runner's batch buffer and are valid only for the duration
// of the call. A step that needs them afterwards copies them (ENGINEERING_RULES.md §2.2).
using StepFn = StepOutcome (*)(StepContext& context,
                               std::span<const bsoncxx::document::view> batch) noexcept;

// One data migration. Ordered largest-alignment-first, so the table packs with
// no interior padding and stays in .rodata.
struct MigrationStep final {
    // STORED, as the ledger's `_id`. There is nothing else to match a run
    // against, so a RENAMED step re-runs from scratch on every cluster that
    // already applied it. Date-prefix the name and then leave it alone,
    // including when the step's code is later corrected.
    std::string_view name;
    std::string_view collection;
    StepFn           apply;
    // How many documents are handed to one call. It bounds the batch buffer and
    // it bounds how much work is re-applied after an interruption, because
    // resumption re-applies the batch that was in flight.
    std::uint32_t    batch_size;
    // The step's PRECONDITION, not its position. A step that reads through an
    // index declares the schema version that created it, and the runner refuses
    // rather than collection-scanning production because the deploy order
    // slipped.
    std::int32_t     min_schema_version;
    Cursor           cursor;
};

// The longest a step name may be. It is a BSON `_id` and it appears in every
// --status line; a bound keeps both readable and keeps the ledger's key small.
inline constexpr std::size_t kMaxStepNameLength = 64;

// A grammar, because the name is a stored key rather than a label.
//
// Lowercase ASCII, digits and single underscores, starting and ending on an
// alphanumeric. Anything else — a space, a capital, a hyphen, a trailing
// underscore — is a name that will be typed back wrong into `--only` at the
// moment somebody is trying to recover a half-applied migration.
[[nodiscard]] constexpr bool step_name_is_well_formed(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxStepNameLength) { return false; }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alphanumeric) { continue; }
        if (c != '_') { return false; }
        // Not at either end, and never doubled: `a__b` and `a_b_` are the two
        // spellings a reader reproduces as `a_b`.
        if (i == 0 || i + 1 == name.size()) { return false; }
        if (name[i - 1] == '_') { return false; }
    }
    return true;
}

// A malformed table is a build error when an application static_asserts it.
//
// `apply` is deliberately NOT checked against nullptr here, for the reason
// anvil/db/query_catalogue.h states about FilterFn: comparing a function pointer
// against nullptr is not foldable in a constant expression on every compiler,
// and a well_formed() that does not compile is worse than one that checks less.
// steps_are_present() below is the runtime half, and the runner refuses a null
// `apply` as a catalogue inconsistency rather than dereferencing it.
[[nodiscard]] constexpr bool step_table_is_well_formed(
    std::span<const MigrationStep> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        const MigrationStep& step = table[i];
        if (!step_name_is_well_formed(step.name)) { return false; }
        // A collection that is not in the application's table would resolve to
        // database index 0 — the first one declared — which is a migration that
        // walks the wrong collection and reports that it finished.
        if (!collection_is_declared(step.collection)) { return false; }
        if (step.batch_size == 0) { return false; }
        if (step.min_schema_version < 0) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            // The name is the ledger's `_id`. Two steps sharing one would each
            // read the other's progress as their own.
            if (table[j].name == step.name) { return false; }
        }
    }
    return true;
}

// The runtime half of the check above: every step has a body to call.
[[nodiscard]] inline bool steps_are_present(std::span<const MigrationStep> table) noexcept {
    for (const MigrationStep& step : table) {
        if (step.apply == nullptr) { return false; }
    }
    return true;
}

}  // namespace anvil::db
