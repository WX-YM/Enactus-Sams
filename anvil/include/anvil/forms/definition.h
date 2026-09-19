#pragma once

// A form definition and a submission row: the two documents this subsystem
// stores, as C++ rather than as BSON.
//
// --- ONE submissions collection, not one per form ---------------------------
//
// The obvious design gives each form its own `answers_<id>` collection and adds a
// guardrail at some thousand forms. Both halves are wrong, and the reasons are
// worth stating because the design they rule out is the one everybody reaches
// for first:
//
//   * every collection is at least one WiredTiger file plus one per index, so
//     1 000 forms is 3 000 descriptors against a common `ulimit -n` of 1 024.
//     The wall arrives around form 300, and it arrives as `Too many open files`
//     — an outage, not a slowdown;
//   * each table carries persistent in-memory metadata, so thousands of them
//     consume cache that should hold hot documents and slow every startup;
//   * dropping a form would be DDL, which takes locks and is NOT transactional
//     with the definition delete — so a crash between them leaves a form
//     pointing at nothing, or an orphan collection nothing references;
//   * a collection name built from a request-supplied id is collection-name
//     injection the moment that id is anything but a server-generated UUID, and
//     removing the pattern removes the entire class.
//
// One collection with a `form` discriminator costs 16 bytes and one compound
// index per document and removes all four. Dropping a form becomes `delete_many`
// plus `delete_one` in one transaction: no DDL, no locks, no orphans.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "anvil/core/types.h"
#include "anvil/crypto/secret.h"
#include "anvil/db/codec.h"
#include "anvil/forms/answer.h"
#include "anvil/forms/fid.h"

namespace anvil::forms {

// Stored as int32. APPEND ONLY.
//
// anvil owns this rather than leaving it to the application because the
// submission path READS it: only an Active form accepts a write, and a Draft one
// answers a public read with the same 404 a missing one does. A lifecycle the
// library enforces cannot be a vocabulary the library does not know.
enum class FormStatus : std::int32_t { Draft = 0, Active = 1, Closed = 2 };

inline constexpr FormStatus kMaxFormStatus = FormStatus::Closed;

struct FormDefinition final {
    LocalizedText           title;
    std::vector<FieldSpec>  fields;
    Uuid                    id;
    Uuid                    creator;
    // Absent means "open until the status changes". Written as null rather than
    // omitted so a future partial index over it sees a field that is always
    // present.
    std::optional<db::TimeMs> closes_at;
    std::int64_t            version;
    std::int64_t            submission_count;
    // Zero means unlimited. An int64 because it is compared against
    // submission_count, which `$inc` keeps as one.
    std::int64_t            max_submissions;
    FormStatus              status;
    // DERIVED from the field types, never bound from a request body.
    bool                    has_pii;
    bool                    one_per_user;

    // Linear over at most 100 entries of a 4-byte comparison. A map would cost a
    // hash per lookup and an allocation per form, against a scan that fits in a
    // handful of cache lines — and this runs once per answer on the submission
    // path, which is why it is inline rather than a call.
    [[nodiscard]] const FieldSpec* find_field(const Fid& target) const noexcept {
        for (const FieldSpec& field : fields) {
            if (field.fid == target) { return &field; }
        }
        return nullptr;
    }
};

// One row of the staff listing. Deliberately NOT a FormDefinition: a list screen
// renders eight facts, and shipping the whole field array for every form would
// send a builder's worth of schema per row to draw a table.
//
// `title` is ONE locale, chosen by the PROJECTION rather than after the fact, for
// the same reason every other localised listing picks its subtree in the query.
struct FormListEntry final {
    std::string  title;
    Uuid         id;
    // Derived from the v7 id, never stored: the id already carries the
    // millisecond, and a stored copy is a second thing that can disagree with it.
    db::TimeMs   created_at;
    std::int64_t version;
    std::int64_t submission_count;
    FormStatus   status;
    // Decides whether this row's submissions link demands the unseal authority.
    // A listing that disagreed with the stored fact would offer a link that 403s.
    bool         has_pii;
    bool         one_per_user;
};

struct FormListPage final {
    std::vector<FormListEntry> entries;
    // The last id of this page. `_id` alone is the whole cursor because v7 ids
    // are unique and already ordered — there is no tie to break.
    std::optional<Uuid>        next;
};

struct SubmissionRecord final {
    std::vector<Answer>       answers;
    // The sealed envelope. Empty when the form declares no PII field; never a
    // plaintext identity, in any branch.
    std::vector<std::uint8_t> pii_envelope;
    // Computed once at SEAL time and stored beside the envelope.
    //
    // Storing it is what keeps the default read path key-free: the ordinary
    // submissions view shows a redacted identity, and decryption requires a
    // separate authority. Both can only be true if the redacted form does not
    // come from a decryption. It discloses exactly what the redacted view
    // discloses and nothing more.
    std::string               pii_redacted;
    crypto::Digest256         pii_index;
    // Every attachment this submission binds, hoisted out of `ans` to a top-level
    // array so a UNIQUE index on it can enforce "one object, one submission"
    // server-side rather than by a read-then-write.
    std::vector<Uuid>         media;
    Uuid                      id;
    Uuid                      form;
    // Attribution. Omitted for anonymous submissions, never written null.
    std::optional<Uuid>       user;
    std::array<std::uint8_t, 16> ip;
    db::TimeMs                submitted_at;
    // The definition version this was validated against, so a row stays
    // interpretable after the definition moves on.
    std::int64_t              form_version;
    bool                      has_pii;
    // Writes a SECOND copy of the user id into `uniq`, which the partial unique
    // index constrains. Two fields rather than a unique index on the attribution
    // directly, because attribution and the one-per-user rule are different
    // questions: a form that allows several submissions per person still needs to
    // know who sent each one.
    bool                      enforce_one_per_user;
};

// The retrieval cursor, on `{form, submitted_at, _id}`. Never `skip(n)`, which is
// O(n) server-side and gets slower with every page.
struct SubmissionCursor final {
    Uuid       id;
    db::TimeMs submitted_at;
};

struct SubmissionPage final {
    std::vector<SubmissionRecord>   entries;
    std::optional<SubmissionCursor> next;
};

}  // namespace anvil::forms
