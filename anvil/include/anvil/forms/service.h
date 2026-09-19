#pragma once

// Form definitions: create, edit, load, list, drop.
//
// --- editing a form that already has submissions ----------------------------
//
// This is the sharp edge of the whole feature. Changing a field's type or
// removing a field makes every existing answer unreadable against the new schema
// — not "wrong", UNREADABLE, because the decoder asserts an answer's BSON type
// against its declared field type and refuses to coerce. So the rules are
// enforced here rather than left to judgement:
//
//   adding an OPTIONAL field ............................. allowed
//   changing a label, adding an option ................... allowed
//   removing a field ..................................... blocked once submitted
//   changing a type ...................................... blocked once submitted
//   removing an option ................................... blocked once submitted
//   making an optional field required .................... blocked once submitted
//
// "Blocked" is Conflict, and the product answer is "duplicate this form". Every
// accepted change bumps the version, and each submission records the version it
// was validated against, so old rows stay interpretable.
//
// --- the definition cache is a PARSED STRUCT --------------------------------
//
// The obvious cache is a Redis key holding the serialised definition, and it
// costs a round trip AND a parse on every submission. Validation has to be a loop
// over a `std::span<const FieldSpec>` with no allocation, so the cache here is
// process-local and holds `shared_ptr<const FormDefinition>` — already parsed,
// shared by every thread, read with one atomic load.
//
// The cost of dropping Redis from this path is cross-instance invalidation: an
// edit on instance A is invisible to instance B until B's entry expires. That is
// bounded by kDefinitionCacheTtl and it is SAFE, because the edit rules above
// mean a stale definition can only be MORE restrictive than the current one — a
// submission validated against it is still valid against the new one.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/forms/attachment.h"
#include "anvil/forms/definition.h"
#include "anvil/forms/field_type.h"
#include "anvil/forms/repository.h"

namespace anvil::forms {

// Short enough that an edit propagates across instances within a page reload,
// long enough that a public form under load reaches MongoDB once a minute per
// instance rather than once per submission.
inline constexpr std::chrono::seconds kDefinitionCacheTtl{60};

// Power of two, so the slot index is a mask rather than a modulo. Direct-mapped
// and fixed-capacity for the same reason the epoch cache is: the key is chosen by
// whoever is submitting, so growth is a memory-exhaustion vector and eviction is
// the correct failure. A thousand forms must cost what ten forms cost.
inline constexpr std::size_t kDefinitionCacheSlots = 256;

inline constexpr std::int32_t kMinSubmissionPageSize = 1;
inline constexpr std::int32_t kMaxSubmissionPageSize = 50;

// Dropping a form destroys submitted data — potentially including identities —
// inside ONE transaction, and a transaction has a size and a time budget. Rather
// than discover that budget as a 16 MB oplog error halfway through, the drop
// refuses above this count and says so: the operator exports first, and the limit
// is a number in a config review rather than an outage.
inline constexpr std::int64_t kMaxDropSubmissions = 10000;

inline constexpr std::int32_t kMinFormListLimit = 1;
inline constexpr std::int32_t kMaxFormListLimit = 50;
inline constexpr std::int32_t kDefaultFormListLimit = 20;

// What a create or an edit binds from a request body.
//
// Deliberately NOT a FormDefinition: `has_pii`, the version, the submission count
// and the creator are all server-derived, and a struct that could carry them is a
// struct a future binder could fill from JSON.
struct FormSchema final {
    LocalizedText             title;
    std::vector<FieldSpec>    fields;
    std::optional<db::TimeMs> closes_at;
    std::int64_t              max_submissions;
    FormStatus                status;
    bool                      one_per_user;
};

struct DropReport final {
    std::int64_t submissions_destroyed;
    std::int64_t attachments_released;
};

class FormService final {
public:
    // `types` and `attachments` must outlive the service: the first is a view of
    // the application's `constexpr` table, the second is owned by whatever wired
    // the application together.
    FormService(std::string database, std::string_view definitions,
                std::string_view submissions, std::span<const FieldTypeSpec> types,
                AttachmentHooks attachments);

    [[nodiscard]] Result<Uuid> create(mongocxx::client& client, const FormSchema& schema,
                                      const Uuid& creator);

    // `expected_version` is the version the editor read. A concurrent edit wins
    // and this one gets VersionMismatch rather than silently overwriting it.
    [[nodiscard]] Result<std::int64_t> edit(mongocxx::client& client, const Uuid& id,
                                            std::int64_t expected_version,
                                            const FormSchema& schema);

    // The submission hot path's read. A hit is one atomic load and a shared_ptr
    // copy; a miss is one indexed find plus a decode. BLOCKING: db_pool only.
    [[nodiscard]] Result<std::shared_ptr<const FormDefinition>> definition(
        mongocxx::client& client, const Uuid& id, db::TimeMs now);

    // Cache only. Returns nullptr on a miss, so a caller on an event-loop thread
    // can answer without ever touching db_pool. nullptr means "not cached here",
    // NEVER "does not exist".
    [[nodiscard]] std::shared_ptr<const FormDefinition> cached(const Uuid& id,
                                                               db::TimeMs now) const noexcept;

    void invalidate(const Uuid& id) noexcept;

    // The staff listing. A thin pass-through, deliberately: there is no policy to
    // add — the states a public read hides are exactly the ones this shows — and
    // a service method that only clamps a limit is still where the clamp belongs.
    [[nodiscard]] Result<FormListPage> list(mongocxx::client& client,
                                            const std::optional<Uuid>& after,
                                            std::int32_t limit, Locale locale) const;

    // The destructive one. Capability-gated at the application's route; this is
    // the transaction:
    //
    //   delete_many({form: id})   no DDL, no locks, no orphans
    //   delete_one({_id: id})
    //   release every bound attachment
    //
    // All three or none: a crash between them is what leaves a form pointing at
    // nothing, or objects pinned forever by rows that no longer exist.
    [[nodiscard]] Result<DropReport> drop(mongocxx::client& client, const Uuid& id);

    [[nodiscard]] Result<SubmissionPage> submissions(
        mongocxx::client& client, const Uuid& form,
        const std::optional<SubmissionCursor>& after, std::int32_t limit) const;

    [[nodiscard]] const FormRepository& repository() const noexcept { return forms_; }
    [[nodiscard]] std::span<const FieldTypeSpec> field_types() const noexcept { return types_; }

    FormService(const FormService&) = delete;
    FormService& operator=(const FormService&) = delete;

private:
    struct CacheSlot final {
        std::atomic<std::shared_ptr<const FormDefinition>> definition;
        std::atomic<std::int64_t>                          expires_at_ms;
    };

    [[nodiscard]] static std::size_t slot_of(const Uuid& id) noexcept;

    // Removes submissions that committed while `drop` was committing, and
    // releases the objects they held. Runs AFTER the drop has already answered,
    // so it reports failure to the log rather than to the caller — the drop
    // itself succeeded, and its report is not made wrong by a straggler.
    void reap_orphaned_submissions(mongocxx::client& client, const Uuid& id, DropReport& report);

    // Declaration order is construction order: forms_ is built from database_ and
    // types_.
    const std::string              database_;
    std::span<const FieldTypeSpec> types_;
    const AttachmentHooks          attachments_;
    FormRepository                 forms_;
    mutable std::array<CacheSlot, kDefinitionCacheSlots> cache_;
};

// --- the rules, exposed because they are the interesting part ---------------

// Shape, per-type constraints, and every declared limit. Pure: no database, no
// clock, no allocation beyond what the caller already holds.
//
// On success every field's `flags` and `max_code_points` have been RESOLVED
// against the table, so the schema that comes out is the one that gets stored.
// That is why it takes a non-const reference: resolving is part of validating,
// and a separate "now bind it" step is a step somebody forgets.
//
// The Failure names the RULE that was broken as a compile-time string, never the
// offending `fid` and never the submitted value.
[[nodiscard]] Status validate_schema(std::span<const FieldTypeSpec> types, FormSchema& schema);

// True when any field's type carries the Pii flag. This is where `has_pii` comes
// from — DERIVED, never bound from a request body. A form that under-declares its
// PII gets the wrong storage treatment and nothing downstream notices.
[[nodiscard]] bool derive_has_pii(std::span<const FieldSpec> fields) noexcept;

// Whether `next` may replace `current` on a form that already has submissions.
// Returns Conflict naming the rule that was broken; the caller turns that into
// "duplicate this form instead".
[[nodiscard]] Status check_edit_compatible(const FormDefinition& current,
                                           const FormSchema& next);

}  // namespace anvil::forms
