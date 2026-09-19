#pragma once

// Validate and submit: the most exposed write endpoint a system built on this
// library has.
//
// A public form is unauthenticated by design, so every check here runs against
// input from nobody in particular. The order below is the order of the steps, and
// each is there because skipping it is a specific, named failure:
//
//   1. STATUS, CLOSING TIME AND THE CEILING. A closed form that still accepts
//      writes is a form nobody is watching.
//   2. ANSWER KEYS IN BOTH DIRECTIONS. Every key in `ans` must name a field, and
//      every required field must have a key. An unknown key is REJECTED, never
//      dropped: dropping hides both client bugs and probing, and an accepted
//      unknown key is stored data nobody validated.
//   3. PER-TYPE VALIDATION through the application's table, dispatched by a
//      function pointer rather than by a switch this library cannot write.
//   4. OPTION VALUES CHECKED AGAINST THE SERVER'S LIST. The list rendered in the
//      browser is not a constraint.
//   5. ATTACHMENTS resolved through the application's hooks: they must exist, be
//      in the right namespace, belong to this submitter, and not already be
//      bound. Without the ownership check a submitter references somebody else's
//      private object and a staff review screen renders it — an IDOR with a
//      confidentiality impact.
//   6. PII SPLIT OUT of `ans` entirely and sealed.
//   7. INSERT — and, only when there are attachments, a transaction.
//
// --- what is enforced by an INDEX rather than by this code ------------------
//
// Two rules are races if they are checked here and then acted on:
//
//   one submission per user    `{form, uniq}` unique, partial on $exists
//   one submission per object  `{media}` unique, multikey, partial on $exists
//
// Both surface as a duplicate-key error, which repo::translate turns into
// Conflict. A count-then-insert loses to a double-click every time.
//
// --- the counter is not on this path ----------------------------------------
//
// Every submission to a form would increment the SAME definition document. Inside
// a transaction that is a write conflict that rolls back the insert and the
// attachment references with it; even outside one it is a second blocking round
// trip and a second durable write the client waits for. Buffered, N submissions
// to one form collapse into a single `$inc` of N.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/forms/attachment.h"
#include "anvil/forms/definition.h"
#include "anvil/forms/field_type.h"
#include "anvil/forms/pii.h"
#include "anvil/forms/repository.h"

namespace anvil::forms {

struct SubmissionInput final {
    std::vector<RawAnswer>       answers;
    std::array<std::uint8_t, 16> ip;
    // Absent for a public, unauthenticated submission.
    std::optional<Uuid>          user;
};

// What the caller needs back. The submission id is returned so a client can
// reference its own row; nothing about other rows is disclosed.
struct SubmissionAccepted final {
    Uuid         id;
    std::int64_t form_version;
};

// The split: every non-PII answer becomes an Answer, and the single PII value —
// if the form declares one — is returned SEPARATELY and never as an answer.
struct ValidatedSubmission final {
    std::vector<Answer> answers;
    std::vector<Uuid>   media;
    // The normalised identity, extracted and not stored in `answers`. Empty when
    // the form declares no PII field.
    std::string         identity;
    Fid                 identity_field;
};

// How long a counter increment may sit in memory. A submission's row is durable
// the moment it commits; only the aggregate count waits, and it is advisory.
inline constexpr std::chrono::milliseconds kCountFlushInterval{1000};

// How a buffered count reaches the database. The service cannot acquire a client
// itself — which pool a flush runs on is the application's decision, and a
// library that reached for a process-wide singleton would make that decision for
// it. Returning a failure is enough: the counts are advisory and the submissions
// they describe are already committed.
using CountFlushFn = std::function<Result<std::int64_t>(
    std::span<const std::pair<Uuid, std::int64_t>> counts)>;

class SubmissionService final {
public:
    // `keys` is BORROWED from the process-wide PiiKeys, owned by whatever wired
    // the application together and outliving every service: copying them into
    // each service would mean N copies of key material to zero rather than one.
    //
    // `policy` is the normalisation and redaction pair. Changing the normaliser
    // after the first row is written changes what "duplicate" means for every row
    // before it, so it belongs next to the keys rather than at a call site.
    SubmissionService(std::string database, std::string_view definitions,
                      std::string_view submissions, std::span<const FieldTypeSpec> types,
                      AttachmentHooks attachments, const PiiKeys& keys,
                      PiiPolicy policy = kDefaultPiiPolicy);

    // BLOCKING: db_pool only. The definition is passed in rather than loaded here,
    // so the caller decides between a cache hit and a read and this function has
    // exactly one kind of database interaction.
    [[nodiscard]] Result<SubmissionAccepted> submit(mongocxx::client& client,
                                                    const FormDefinition& form,
                                                    const SubmissionInput& input,
                                                    db::TimeMs now);

    // Decrypts one submission's identity. Separate from the read path on purpose:
    // reading submissions and reading identity numbers are different authorities,
    // and every call is audited by the caller.
    //
    // nullopt when the envelope does not open — tampered, truncated, wrong key, or
    // moved between forms. The caller cannot distinguish these.
    [[nodiscard]] std::optional<std::string> reveal_identity(
        const FormDefinition& form, const SubmissionRecord& record) const;

    // --- the buffered counter -----------------------------------------------

    // Applies the buffered increments through `flush` and clears them. Call it on
    // a timer the application owns — a library does not install itself on
    // somebody's event loop — and once more during shutdown, on the calling
    // thread, BEFORE the pools drain: a flush posted to a pool that is shutting
    // down may never run, and these counts exist nowhere else.
    void flush_counts(const CountFlushFn& flush) noexcept;

    // Stops accepting new counts. Idempotent; flush once more afterwards.
    void stop() noexcept;

    // Forms with an increment not yet written. Zero in a settled system; exposed
    // so a test can observe the flush rather than sleep through it.
    [[nodiscard]] std::size_t pending_counts() const noexcept;

    [[nodiscard]] const FormRepository& repository() const noexcept { return forms_; }

    SubmissionService(const SubmissionService&) = delete;
    SubmissionService& operator=(const SubmissionService&) = delete;

private:
    // Declaration order is construction order.
    const std::string              database_;
    std::span<const FieldTypeSpec> types_;
    const AttachmentHooks          attachments_;
    const PiiKeys&                 keys_;
    const PiiPolicy                policy_;
    FormRepository                 forms_;
    mutable std::mutex             counts_mutex_;
    // form id -> submissions counted since the last flush. A map rather than a
    // queue because the whole point is that N submissions to one form collapse
    // into ONE `$inc` of N: the write the request path used to make per
    // submission is the write this removes.
    std::map<Uuid, std::int64_t>   pending_counts_;
    bool                           counts_stopped_ = false;
};

// --- the rules, exposed because they are the interesting part ---------------

// Both directions of the key check, the table dispatch, and the PII split.
//
// `Failure::field` is the compile-time name of the RULE that was broken, never
// the offending `fid` and never the submitted value.
[[nodiscard]] Result<ValidatedSubmission> validate_answers(
    std::span<const FieldTypeSpec> types, const FormDefinition& form,
    const SubmissionInput& input, const PiiPolicy& policy = kDefaultPiiPolicy);

// Whether the form is accepting writes at `now`. Separate because it is the half
// a caller may want to answer before doing any work at all.
[[nodiscard]] Status check_form_open(const FormDefinition& form, db::TimeMs now);

// The single PII field a form may declare, or nullptr. More than one is refused
// at creation: one envelope per submission keeps the AAD binding — form id plus
// field id — unambiguous and the unseal path single-valued.
[[nodiscard]] const FieldSpec* pii_field_of(const FormDefinition& form) noexcept;

}  // namespace anvil::forms
