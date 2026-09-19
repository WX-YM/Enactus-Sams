#include "anvil/forms/submission_service.h"

#include <exception>
#include <utility>

#include <mongocxx/client_session.hpp>
#include <mongocxx/exception/exception.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/core/uuid.h"
#include "anvil/forms/validators.h"

namespace anvil::forms {
namespace {

struct AbortTransaction final : std::exception {
    explicit AbortTransaction(Failure f) noexcept : failure{f} {}
    [[nodiscard]] const char* what() const noexcept override { return "submission aborted"; }
    Failure failure;
};

[[nodiscard]] bool answer_is_empty(const RawAnswer& raw) noexcept {
    if (raw.shape == input::JsonType::Array) { return raw.choices.empty(); }
    if (raw.shape == input::JsonType::String) { return raw.text.empty(); }
    return raw.shape == input::JsonType::Null;
}

}  // namespace

// --- the rules ---------------------------------------------------------------

const FieldSpec* pii_field_of(const FormDefinition& form) noexcept {
    for (const FieldSpec& field : form.fields) {
        if (has_flag(field.flags, FieldTypeFlag::Pii)) { return &field; }
    }
    return nullptr;
}

Status check_form_open(const FormDefinition& form, db::TimeMs now) {
    if (form.status != FormStatus::Active) {
        // NotFound rather than Conflict: a draft form's existence is not public
        // information, and a closed one answering differently from a missing one
        // is an enumeration oracle on an administrator's work in progress.
        return fail(ErrorCode::NotFound, kFormField);
    }
    if (form.closes_at.has_value() && *form.closes_at <= now) {
        return fail(ErrorCode::Conflict, kFormField);
    }
    // Zero means unlimited. The count is ADVISORY under concurrency — two
    // submissions may both pass it — and that is acceptable: the ceiling is a
    // business limit, not a safety property, and the alternative is serialising
    // every public submission behind one document's version.
    if (form.max_submissions > 0 && form.submission_count >= form.max_submissions) {
        return fail(ErrorCode::Conflict, kFormField);
    }
    return ok();
}

Result<ValidatedSubmission> validate_answers(std::span<const FieldTypeSpec> types,
                                             const FormDefinition& form,
                                             const SubmissionInput& input,
                                             const PiiPolicy& policy) {
    // Bounded before anything is examined: an answer count above the schema's
    // field count is either a client bug or a probe, and both are cheaper to
    // reject on a size compare.
    if (input.answers.size() > form.fields.size()) {
        return fail(ErrorCode::ValidationFailed, kAnswersField);
    }

    ValidatedSubmission validated{};
    validated.answers.reserve(input.answers.size());

    for (const RawAnswer& raw : input.answers) {
        const FieldSpec* field = form.find_field(raw.fid);
        // REJECTED, not dropped. Dropping hides a client bug and hides probing,
        // and an accepted unknown key is stored data nobody validated.
        if (field == nullptr) { return fail(ErrorCode::ValidationFailed, kUnknownField); }
        // A key sent twice would overwrite itself in `ans`. The parser rejects
        // duplicate JSON keys; this covers the case where two entries were built
        // some other way.
        for (const Answer& seen : validated.answers) {
            if (seen.fid == raw.fid) { return fail(ErrorCode::ValidationFailed, kAnswersField); }
        }
        if (!validated.identity.empty() && has_flag(field->flags, FieldTypeFlag::Pii)) {
            return fail(ErrorCode::ValidationFailed, kAnswersField);
        }

        if (answer_is_empty(raw)) {
            if (!field->optional) { return fail(ErrorCode::ValidationFailed, kMissingField); }
            // An optional field sent empty is stored as NOTHING rather than as an
            // empty string: an absent answer and a blank one must not be two
            // different things in an export.
            continue;
        }

        // The dispatch. One indexed load and one indirect call, against the
        // switch this library cannot write because the table is not its own.
        const FieldTypeSpec* spec = field_type_spec(types, field->type);
        if (spec == nullptr) {
            // The definition names a type this build does not declare. It decoded
            // only because a newer process wrote it, and refusing is the correct
            // direction to fail.
            return fail(ErrorCode::ValidationFailed, kTypeField);
        }
        // Refused by NAME rather than dereferenced. validators_are_present is the
        // check that catches this at boot; this is what makes the miss a rejected
        // answer instead of a null call, because the one thing a table-driven
        // dispatch must never do is jump through a pointer it did not verify.
        if (spec->validate == nullptr) { return fail(ErrorCode::ValidationFailed, kTypeField); }

        Answer answer{};
        answer.fid = raw.fid;
        std::string identity;
        if (const Status valid = spec->validate(*field, raw, answer, identity); !valid) {
            return valid.error();
        }

        if (has_flag(spec->flags, FieldTypeFlag::Pii)) {
            // The contract a PII validator has to meet, checked rather than
            // trusted: it fills `identity` and produces no answer at all. A
            // custom validator that got this wrong would otherwise write an
            // identity number into `ans`, which is the one thing this whole
            // subsystem exists to make impossible.
            if (identity.empty()) { return fail(ErrorCode::ValidationFailed, kIdentityField); }
            validated.identity = policy.normalise != nullptr ? policy.normalise(identity)
                                                             : identity;
            validated.identity_field = raw.fid;
            continue;
        }
        // The mirror of the above: a non-PII validator must NOT produce an
        // identity, and must produce the answer shape its flags declare. Both are
        // refused writes here rather than unreadable rows months later.
        if (!identity.empty()) { return fail(ErrorCode::ValidationFailed, kTypeField); }
        if (answer.kind != answer_kind_of(spec->flags)) {
            return fail(ErrorCode::ValidationFailed, kTypeField);
        }

        if (answer.kind == AnswerKind::Media) {
            // Hoisted to the top level as well, so the UNIQUE index on the array
            // is what enforces "one object, one submission".
            for (const Uuid& seen : validated.media) {
                if (seen == answer.media) {
                    return fail(ErrorCode::ValidationFailed, kMediaField);
                }
            }
            if (validated.media.size() >= kMaxSubmissionMedia) {
                return fail(ErrorCode::ValidationFailed, kMediaField);
            }
            validated.media.push_back(answer.media);
        }
        validated.answers.push_back(std::move(answer));
    }

    // The other direction: every required field must have arrived. Checked AFTER
    // the loop so a body that omits half the form still costs one pass.
    for (const FieldSpec& field : form.fields) {
        if (field.optional) { continue; }
        if (has_flag(field.flags, FieldTypeFlag::Pii)) {
            if (validated.identity.empty()) {
                return fail(ErrorCode::ValidationFailed, kMissingField);
            }
            continue;
        }
        bool present = false;
        for (const Answer& answer : validated.answers) {
            if (answer.fid == field.fid) {
                present = true;
                break;
            }
        }
        if (!present) { return fail(ErrorCode::ValidationFailed, kMissingField); }
    }
    return validated;
}

// --- the service -------------------------------------------------------------

SubmissionService::SubmissionService(std::string database, std::string_view definitions,
                                     std::string_view submissions,
                                     std::span<const FieldTypeSpec> types,
                                     AttachmentHooks attachments, const PiiKeys& keys,
                                     PiiPolicy policy)
    : database_{std::move(database)},
      types_{types},
      attachments_{std::move(attachments)},
      keys_{keys},
      policy_{policy},
      forms_{database_, definitions, submissions, types} {}

Result<SubmissionAccepted> SubmissionService::submit(mongocxx::client& client,
                                                     const FormDefinition& form,
                                                     const SubmissionInput& input,
                                                     db::TimeMs now) {
    if (const Status open = check_form_open(form, now); !open) { return open.error(); }

    Result<ValidatedSubmission> checked = validate_answers(types_, form, input, policy_);
    if (!checked) { return checked.error(); }
    ValidatedSubmission validated = std::move(checked).value();

    for (const Uuid& attachment : validated.media) {
        // An application with no attachment hooks has declared no attachment
        // field type, so an id reaching here references an object nothing will
        // ever release. Refused, and with the same answer every other attachment
        // failure gets: which ids exist is not something a failed request
        // confirms.
        if (!attachments_.complete()) { return fail(ErrorCode::NotFound, kMediaField); }
        const Result<bool> allowed = attachments_.may_bind(client, attachment, input.user);
        if (!allowed) { return allowed.error(); }
        if (!allowed.value()) { return fail(ErrorCode::NotFound, kMediaField); }
    }

    SubmissionRecord record{};
    record.answers = std::move(validated.answers);
    record.media = validated.media;
    record.id = uuid::generate_v7();
    record.form = form.id;
    record.ip = input.ip;
    record.submitted_at = now;
    record.form_version = form.version;
    // Attribution always; the unique-index copy only when the form asks for one
    // submission per person. The repository writes both from these two fields, so
    // there is one place that decides which index a row lands in.
    record.user = input.user;
    record.enforce_one_per_user = form.one_per_user;

    if (!validated.identity.empty()) {
        const PiiAad aad = pii_aad(form.id, validated.identity_field.view());
        const SealedIdentity sealed = seal_identity(keys_, validated.identity, aad);
        record.pii_envelope = sealed.envelope;
        record.pii_index = sealed.blind_index;
        // Computed HERE, once, from the value that is about to stop existing in
        // plaintext. Every later read of the default view is then a string read
        // rather than a decryption, which is what lets "the ordinary view is
        // redacted" and "unsealing needs a separate authority" both be true.
        record.pii_redacted = policy_.redact != nullptr ? policy_.redact(validated.identity)
                                                        : validated.identity;
        record.has_pii = true;

        // Duplicate detection WITHOUT decrypting anything. Advisory under
        // concurrency in the same way the submission ceiling is; the value is in
        // catching the ordinary repeat submission, and a unique index here would
        // make one person's identity globally unusable if a form were ever
        // restored from a backup.
        const Result<bool> seen = forms_.blind_index_seen(client, form.id, sealed.blind_index);
        if (!seen) { return seen.error(); }
        if (seen.value()) { return fail(ErrorCode::Conflict, kIdentityField); }
    }

    if (record.media.empty()) {
        // No attachment means one document and one write, which the server
        // already applies atomically — the first of the two options ENGINEERING_RULES.md §6
        // allows, and there is no second document for a transaction to bind it
        // to.
        //
        // This is the COMMON case, not an optimisation for a rare one: an
        // anonymous submitter cannot upload anything, so every submission to a
        // public form takes this path. The transaction it skips costs a session,
        // a commit round trip and a majority-commit wait, and those were the
        // whole difference between the p50 and the p99.9 under load.
        if (const Status inserted = forms_.insert_submission(client, record); !inserted) {
            return inserted.error();
        }
    } else {
        try {
            auto session = client.start_session();
            repo::in_transaction(session, [&](mongocxx::client_session* txn) {
                const Status inserted = forms_.insert_submission(client, *txn, record);
                if (!inserted) { throw AbortTransaction{inserted.error()}; }
                // Reference counts move inside the SAME transaction as the row
                // that owns them. A count that commits while the submission
                // aborts is a leak no sweep can distinguish from a live
                // reference.
                for (const Uuid& attachment : record.media) {
                    const Status attached = attachments_.bind(client, *txn, attachment);
                    if (!attached) { throw AbortTransaction{attached.error()}; }
                }
            });
        } catch (const AbortTransaction& aborted) {
            return aborted.failure;
        } catch (const mongocxx::exception& e) {
            LOG_ERROR << "submission transaction failed: " << e.what();
            return fail(ErrorCode::ServiceUnavailable);
        }
    }

    // Buffered, not written. The ceiling this feeds is already documented as
    // advisory under concurrency, so a counter that lags by a flush interval was
    // always within contract — and this was the last blocking round trip the
    // request made.
    {
        const std::lock_guard<std::mutex> held{counts_mutex_};
        if (!counts_stopped_) { ++pending_counts_[form.id]; }
    }

    return SubmissionAccepted{record.id, record.form_version};
}

void SubmissionService::stop() noexcept {
    const std::lock_guard<std::mutex> held{counts_mutex_};
    counts_stopped_ = true;
}

std::size_t SubmissionService::pending_counts() const noexcept {
    const std::lock_guard<std::mutex> held{counts_mutex_};
    return pending_counts_.size();
}

void SubmissionService::flush_counts(const CountFlushFn& flush) noexcept {
    std::map<Uuid, std::int64_t> taken;
    {
        const std::lock_guard<std::mutex> held{counts_mutex_};
        if (pending_counts_.empty()) { return; }
        taken.swap(pending_counts_);
    }

    std::vector<std::pair<Uuid, std::int64_t>> batch;
    batch.reserve(taken.size());
    for (const auto& [form, delta] : taken) { batch.emplace_back(form, delta); }

    try {
        const Result<std::int64_t> missing = flush(batch);
        if (!missing) {
            // The counts are gone with the batch. They are advisory, the
            // submissions themselves are committed, and re-queueing a failed
            // batch forever is how a buffer becomes a leak.
            LOG_ERROR << "submission counters: a batch of " << batch.size()
                      << " form(s) failed to write";
            return;
        }
        if (missing.value() > 0) {
            LOG_WARN << missing.value()
                     << " form(s) were dropped while their submissions were in flight; "
                        "the drop reaps those rows";
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "submission counters: flush threw: " << e.what();
    }
}

std::optional<std::string> SubmissionService::reveal_identity(
    const FormDefinition& form, const SubmissionRecord& record) const {
    if (!record.has_pii || record.pii_envelope.empty()) { return std::nullopt; }
    const FieldSpec* field = pii_field_of(form);
    if (field == nullptr) { return std::nullopt; }
    // The AAD is rebuilt from the form and the field, so an envelope moved to
    // another form or another field fails to open rather than decrypting into the
    // wrong record.
    return open_identity(keys_, record.pii_envelope, pii_aad(form.id, field->fid.view()));
}

}  // namespace anvil::forms
