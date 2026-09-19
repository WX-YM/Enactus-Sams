#include "anvil/forms/service.h"

#include <algorithm>
#include <exception>
#include <utility>

#include <mongocxx/client_session.hpp>
#include <mongocxx/exception/exception.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/core/uuid.h"
#include "anvil/db/versioned.h"
#include "anvil/i18n/utf8.h"

namespace anvil::forms {
namespace {

struct AbortTransaction final : std::exception {
    explicit AbortTransaction(Failure f) noexcept : failure{f} {}
    [[nodiscard]] const char* what() const noexcept override { return "form write aborted"; }
    Failure failure;
};

// Compile-time field names for every Failure this file produces. A name from the
// request — a client-chosen `fid`, an option value — would put client-controlled
// bytes into a response and a log line.
constexpr std::string_view kFieldsField = "fields";
constexpr std::string_view kTitleField = "title";
constexpr std::string_view kOptionsField = "options";
constexpr std::string_view kTypeField = "type";
constexpr std::string_view kRangeField = "range";
constexpr std::string_view kLabelField = "label";
constexpr std::string_view kFidField = "fid";
constexpr std::string_view kStatusField = "status";
constexpr std::string_view kClosesAtField = "closes_at";
constexpr std::string_view kFormField = "form";
constexpr std::string_view kSubmissionCountField = "submission_count";

[[nodiscard]] bool is_option_value(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxOptionValueLength) { return false; }
    for (const char c : value) {
        const bool lower = c >= 'a' && c <= 'z';
        const bool upper = c >= 'A' && c <= 'Z';
        const bool digit = c >= '0' && c <= '9';
        if (!lower && !upper && !digit && c != '_' && c != '-') { return false; }
    }
    return true;
}

// Every declared locale, present and non-empty and within the bound. A missing
// translation is a validation error, never a fallback to another locale: a
// fallback renders the wrong language to a reader who cannot tell it is wrong.
[[nodiscard]] bool is_complete_label(const LocalizedText& label) noexcept {
    for (const std::string& text : label) {
        if (text.empty()) { return false; }
        if (!i18n::within_code_point_bounds(text, 1, kMaxLabelCodePoints)) { return false; }
    }
    return true;
}

[[nodiscard]] Status validate_options(const FieldSpec& field) {
    if (!has_flag(field.flags, FieldTypeFlag::Options)) {
        // An option list on a text field is a form the author did not mean to
        // build, and storing it would make the field's rendering ambiguous.
        return field.options.empty() ? ok() : fail(ErrorCode::ValidationFailed, kOptionsField);
    }
    // Fewer than two is not a choice. A one-option select is a hidden field with
    // extra steps, and it renders as a control the user cannot use.
    if (field.options.size() < 2 || field.options.size() > kMaxFieldOptions) {
        return fail(ErrorCode::ValidationFailed, kOptionsField);
    }
    for (std::size_t i = 0; i < field.options.size(); ++i) {
        const FormOption& option = field.options[i];
        if (!is_option_value(option.value)) {
            return fail(ErrorCode::ValidationFailed, kOptionsField);
        }
        if (!is_complete_label(option.label)) {
            return fail(ErrorCode::ValidationFailed, kOptionsField);
        }
        // Duplicate values make the answer ambiguous: two options render
        // distinctly and store identically, so an export cannot say which was
        // chosen. Quadratic over at most 50 entries, which is 1 225 compares of a
        // short string — cheaper than the set a loop-free version would allocate.
        for (std::size_t j = 0; j < i; ++j) {
            if (field.options[j].value == option.value) {
                return fail(ErrorCode::ValidationFailed, kOptionsField);
            }
        }
    }
    return ok();
}

[[nodiscard]] Status validate_field(std::span<const FieldTypeSpec> types, FieldSpec& field) {
    if (field.fid.empty()) {
        // Unreachable through a binder — Fid::parse is the only constructor that
        // produces a non-empty one — and checked anyway, because this is the
        // value that becomes a BSON key.
        return fail(ErrorCode::ValidationFailed, kFidField);
    }
    // Resolving the type is part of validating it: an undeclared code and a cap
    // above the global ceiling both fail here, and on success the field carries
    // the flags and the cap every later stage reads.
    if (const Status bound =
            bind_field_type(types, field, ErrorCode::ValidationFailed, kTypeField);
        !bound) {
        return bound;
    }
    if (!is_complete_label(field.label)) {
        return fail(ErrorCode::ValidationFailed, kLabelField);
    }
    if (const Status options = validate_options(field); !options) { return options; }

    if (has_flag(field.flags, FieldTypeFlag::Ranged) && field.min_value > field.max_value) {
        return fail(ErrorCode::ValidationFailed, kRangeField);
    }
    if (has_flag(field.flags, FieldTypeFlag::MultiSelect) &&
        field.max_selections > field.options.size()) {
        return fail(ErrorCode::ValidationFailed, kRangeField);
    }
    return ok();
}

}  // namespace

// --- the rules ---------------------------------------------------------------

bool derive_has_pii(std::span<const FieldSpec> fields) noexcept {
    for (const FieldSpec& field : fields) {
        if (has_flag(field.flags, FieldTypeFlag::Pii)) { return true; }
    }
    return false;
}

Status validate_schema(std::span<const FieldTypeSpec> types, FormSchema& schema) {
    if (!is_complete_label(schema.title)) {
        return fail(ErrorCode::ValidationFailed, kTitleField);
    }
    if (schema.fields.empty() || schema.fields.size() > kMaxFormFields) {
        return fail(ErrorCode::ValidationFailed, kFieldsField);
    }
    if (schema.status > kMaxFormStatus || static_cast<std::int32_t>(schema.status) < 0) {
        return fail(ErrorCode::ValidationFailed, kStatusField);
    }
    if (schema.max_submissions < 0) {
        return fail(ErrorCode::ValidationFailed, kStatusField);
    }
    if (schema.closes_at.has_value() && schema.closes_at->time_since_epoch().count() <= 0) {
        return fail(ErrorCode::ValidationFailed, kClosesAtField);
    }

    std::size_t pii_fields = 0;
    for (std::size_t i = 0; i < schema.fields.size(); ++i) {
        if (const Status field = validate_field(types, schema.fields[i]); !field) { return field; }
        if (has_flag(schema.fields[i].flags, FieldTypeFlag::Pii)) { ++pii_fields; }
        // Unique within the form. A duplicate `fid` means the second answer
        // silently overwrites the first in the `ans` subdocument, and the export
        // shows one column where the author built two.
        for (std::size_t j = 0; j < i; ++j) {
            if (schema.fields[j].fid == schema.fields[i].fid) {
                return fail(ErrorCode::ValidationFailed, kFidField);
            }
        }
    }
    // A submission carries ONE envelope, and that envelope's AAD binds it to one
    // field id — two identity fields would need two envelopes, two blind indexes
    // and two audit rows per unseal. Refused here rather than silently written
    // single-valued.
    if (pii_fields > kMaxPiiFieldsPerForm) {
        return fail(ErrorCode::ValidationFailed, kFieldsField);
    }
    return ok();
}

Status check_edit_compatible(const FormDefinition& current, const FormSchema& next) {
    // No submissions, no constraint: the definition is still a draft in every
    // sense that matters, and an author reshaping it breaks nothing.
    if (current.submission_count == 0) { return ok(); }

    for (const FieldSpec& existing : current.fields) {
        const FieldSpec* replacement = nullptr;
        for (const FieldSpec& candidate : next.fields) {
            if (candidate.fid == existing.fid) {
                replacement = &candidate;
                break;
            }
        }
        // Removing a field orphans every answer stored under its id: the decoder
        // skips them, so the data is still on disk and invisible in every view.
        // "Duplicate this form" is the honest answer.
        if (replacement == nullptr) { return fail(ErrorCode::Conflict, kFieldsField); }
        // A type change makes the stored answers undecodable against the new
        // schema — the decoder asserts BSON type against declared type and
        // refuses to coerce.
        if (replacement->type != existing.type) { return fail(ErrorCode::Conflict, kTypeField); }
        // Making a field required retroactively invalidates every submission that
        // legitimately omitted it.
        if (existing.optional && !replacement->optional) {
            return fail(ErrorCode::Conflict, kFieldsField);
        }
        // Adding an option is fine; removing one strands the submissions that
        // chose it, whose stored value then matches nothing the form offers.
        for (const FormOption& option : existing.options) {
            bool still_offered = false;
            for (const FormOption& candidate : replacement->options) {
                if (candidate.value == option.value) {
                    still_offered = true;
                    break;
                }
            }
            if (!still_offered) { return fail(ErrorCode::Conflict, kOptionsField); }
        }
    }

    // A NEW field must be optional. A required one invalidates every existing
    // submission the moment it is added.
    for (const FieldSpec& candidate : next.fields) {
        bool existed = false;
        for (const FieldSpec& existing : current.fields) {
            if (existing.fid == candidate.fid) {
                existed = true;
                break;
            }
        }
        if (!existed && !candidate.optional) { return fail(ErrorCode::Conflict, kFieldsField); }
    }
    return ok();
}

// --- the service -------------------------------------------------------------

FormService::FormService(std::string database, std::string_view definitions,
                         std::string_view submissions, std::span<const FieldTypeSpec> types,
                         AttachmentHooks attachments)
    : database_{std::move(database)},
      types_{types},
      attachments_{std::move(attachments)},
      forms_{database_, definitions, submissions, types},
      cache_{} {}

std::size_t FormService::slot_of(const Uuid& id) noexcept {
    static_assert((kDefinitionCacheSlots & (kDefinitionCacheSlots - 1)) == 0,
                  "the slot index is a mask, not a modulo");
    // The trailing bytes of a UUIDv7 are CSPRNG output, so they distribute
    // without a hash. Direct-mapped: a colliding form evicts rather than growing
    // the table, because WHICH form is cached is decided by whoever is submitting
    // and an unbounded map is then a memory-exhaustion vector.
    return ((static_cast<std::size_t>(id[14]) << 8U) | static_cast<std::size_t>(id[15])) &
           (kDefinitionCacheSlots - 1);
}

std::shared_ptr<const FormDefinition> FormService::cached(const Uuid& id,
                                                          db::TimeMs now) const noexcept {
    const CacheSlot& slot = cache_[slot_of(id)];
    // The expiry is read BEFORE the pointer. Read the other way round, a slot
    // refreshed between the two loads would be served against the old expiry.
    const std::int64_t expires_at = slot.expires_at_ms.load(std::memory_order_acquire);
    if (expires_at <= now.time_since_epoch().count()) { return nullptr; }
    std::shared_ptr<const FormDefinition> definition =
        slot.definition.load(std::memory_order_acquire);
    // A collision lands on the same slot with a different id, which is a MISS
    // rather than the wrong form. Checking the id is what makes a direct-mapped
    // cache safe to key on attacker-chosen bytes.
    if (!definition || definition->id != id) { return nullptr; }
    return definition;
}

void FormService::invalidate(const Uuid& id) noexcept {
    CacheSlot& slot = cache_[slot_of(id)];
    slot.expires_at_ms.store(0, std::memory_order_release);
    slot.definition.store(nullptr, std::memory_order_release);
}

Result<std::shared_ptr<const FormDefinition>> FormService::definition(mongocxx::client& client,
                                                                      const Uuid& id,
                                                                      db::TimeMs now) {
    if (std::shared_ptr<const FormDefinition> hit = cached(id, now)) { return hit; }

    const Result<std::optional<FormDefinition>> loaded = forms_.find_definition(client, id);
    if (!loaded) { return loaded.error(); }
    if (!loaded.value().has_value()) { return fail(ErrorCode::NotFound, kFormField); }

    auto shared = std::make_shared<const FormDefinition>(*loaded.value());
    CacheSlot& slot = cache_[slot_of(id)];
    // The pointer first, the expiry second: a reader that sees the new expiry
    // must be able to see the pointer that goes with it.
    slot.definition.store(shared, std::memory_order_release);
    slot.expires_at_ms.store(
        now.time_since_epoch().count() +
            std::chrono::duration_cast<std::chrono::milliseconds>(kDefinitionCacheTtl).count(),
        std::memory_order_release);
    return shared;
}

Result<Uuid> FormService::create(mongocxx::client& client, const FormSchema& schema,
                                 const Uuid& creator) {
    FormSchema resolved = schema;
    if (const Status valid = validate_schema(types_, resolved); !valid) { return valid.error(); }

    FormDefinition form{};
    form.title = resolved.title;
    form.fields = std::move(resolved.fields);
    form.id = uuid::generate_v7();
    form.creator = creator;
    form.closes_at = resolved.closes_at;
    form.version = repo::kInitialVersion;
    form.submission_count = 0;
    form.max_submissions = resolved.max_submissions;
    form.status = resolved.status;
    // DERIVED, never accepted. A form that under-declares its PII would get the
    // wrong storage treatment and nothing downstream would notice.
    form.has_pii = derive_has_pii(form.fields);
    form.one_per_user = resolved.one_per_user;

    if (const Status inserted = forms_.insert_definition(client, form); !inserted) {
        return inserted.error();
    }
    return form.id;
}

Result<std::int64_t> FormService::edit(mongocxx::client& client, const Uuid& id,
                                       std::int64_t expected_version, const FormSchema& schema) {
    FormSchema resolved = schema;
    if (const Status valid = validate_schema(types_, resolved); !valid) { return valid.error(); }

    const Result<std::optional<FormDefinition>> current = forms_.find_definition(client, id);
    if (!current) { return current.error(); }
    if (!current.value().has_value()) { return fail(ErrorCode::NotFound, kFormField); }
    if (const Status compatible = check_edit_compatible(*current.value(), resolved);
        !compatible) {
        return compatible.error();
    }

    FormDefinition next = *current.value();
    next.title = resolved.title;
    next.fields = std::move(resolved.fields);
    next.closes_at = resolved.closes_at;
    next.max_submissions = resolved.max_submissions;
    next.status = resolved.status;
    next.has_pii = derive_has_pii(next.fields);
    next.one_per_user = resolved.one_per_user;

    const Result<std::int64_t> updated =
        forms_.update_definition(client, id, expected_version, next);
    // Invalidated on failure too: a VersionMismatch means somebody else's edit
    // landed, so the entry this instance holds is stale either way.
    invalidate(id);
    return updated;
}

Result<FormListPage> FormService::list(mongocxx::client& client,
                                       const std::optional<Uuid>& after, std::int32_t limit,
                                       Locale locale) const {
    return forms_.list_definitions(client, after,
                                   std::clamp(limit, kMinFormListLimit, kMaxFormListLimit),
                                   locale);
}

Result<SubmissionPage> FormService::submissions(mongocxx::client& client, const Uuid& form,
                                                const std::optional<SubmissionCursor>& after,
                                                std::int32_t limit) const {
    return forms_.page(client, form, after,
                       std::clamp(limit, kMinSubmissionPageSize, kMaxSubmissionPageSize));
}

// A submission whose transaction committed after the drop's did. It is possible
// because the two no longer serialise against each other: the counter `$inc` that
// used to force them to is now outside the submission transaction, which is what
// removed the contention every submission was paying for.
//
// The window is narrow and it is CLOSED, not merely narrow: `invalidate` runs
// first, so no reader can obtain the definition afterwards, and only the
// submissions already in flight when the drop committed can still land. Each pass
// therefore sees strictly fewer rows than the last and the loop ends.
//
// Reaping matters because an orphan is not merely untidy. The row holds
// attachment references, and a reference nothing can ever release is a file the
// sweeper will not collect.
void FormService::reap_orphaned_submissions(mongocxx::client& client, const Uuid& id,
                                            DropReport& report) {
    // Two passes settle every case the window can produce; the bound is here so
    // that a defect elsewhere costs a logged warning rather than a thread.
    constexpr int kMaxPasses = 8;

    for (int pass = 0; pass < kMaxPasses; ++pass) {
        std::int64_t reaped = 0;
        try {
            auto session = client.start_session();
            repo::in_transaction(session, [&](mongocxx::client_session* txn) {
                const Result<std::vector<Uuid>> media =
                    forms_.bound_media(client, *txn, id, kMaxDropSubmissions);
                if (!media) { throw AbortTransaction{media.error()}; }

                const Result<std::int64_t> destroyed =
                    forms_.delete_submissions(client, *txn, id);
                if (!destroyed) { throw AbortTransaction{destroyed.error()}; }

                if (attachments_.release) {
                    for (const Uuid& attachment : media.value()) {
                        const Status released = attachments_.release(client, *txn, attachment);
                        if (!released) { throw AbortTransaction{released.error()}; }
                    }
                }

                reaped = destroyed.value();
                report.submissions_destroyed += destroyed.value();
                report.attachments_released += static_cast<std::int64_t>(media.value().size());
            });
        } catch (const AbortTransaction& aborted) {
            LOG_ERROR << "form drop: reaping stragglers failed, code "
                      << static_cast<int>(aborted.failure.code);
            return;
        } catch (const mongocxx::exception& e) {
            LOG_ERROR << "form drop: reaping stragglers failed: " << e.what();
            return;
        }

        // The definition is already gone and the drop already answered, so an
        // empty pass is the normal case and costs one indexed read.
        if (reaped == 0) { return; }
        LOG_WARN << "form drop reaped " << reaped
                 << " submission(s) that committed during the drop";
    }

    LOG_ERROR << "form drop: stragglers still arriving after " << kMaxPasses
              << " passes; submissions are being accepted for a dropped form";
}

Result<DropReport> FormService::drop(mongocxx::client& client, const Uuid& id) {
    DropReport report{};
    try {
        auto session = client.start_session();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            const Result<std::int64_t> count = forms_.count_submissions(client, *txn, id);
            if (!count) { throw AbortTransaction{count.error()}; }
            if (count.value() > kMaxDropSubmissions) {
                // Refused rather than attempted. A transaction this large hits
                // the server's oplog and time budgets and fails halfway, which is
                // the same outcome with none of the explanation.
                throw AbortTransaction{fail(ErrorCode::Conflict, kSubmissionCountField)};
            }

            const Result<std::vector<Uuid>> media =
                forms_.bound_media(client, *txn, id, kMaxDropSubmissions);
            if (!media) { throw AbortTransaction{media.error()}; }

            const Result<std::int64_t> destroyed = forms_.delete_submissions(client, *txn, id);
            if (!destroyed) { throw AbortTransaction{destroyed.error()}; }

            const Result<bool> deleted = forms_.delete_definition(client, *txn, id);
            if (!deleted) { throw AbortTransaction{deleted.error()}; }
            if (!deleted.value()) { throw AbortTransaction{fail(ErrorCode::NotFound, kFormField)}; }

            // Released INSIDE the same transaction. A decrement that commits
            // while the delete aborts is a file the collector takes out from
            // under a live submission.
            if (attachments_.release) {
                for (const Uuid& attachment : media.value()) {
                    const Status released = attachments_.release(client, *txn, attachment);
                    if (!released) { throw AbortTransaction{released.error()}; }
                }
            }

            report.submissions_destroyed = destroyed.value();
            report.attachments_released = static_cast<std::int64_t>(media.value().size());
        });
    } catch (const AbortTransaction& aborted) {
        return aborted.failure;
    } catch (const mongocxx::exception& e) {
        LOG_ERROR << "form drop transaction failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable);
    }

    // BEFORE the reap, not after: this is what stops new submissions from being
    // accepted against a cached definition, so it is what makes the reap below
    // terminate rather than chase an endless supply of new rows.
    invalidate(id);
    reap_orphaned_submissions(client, id, report);
    return report;
}

}  // namespace anvil::forms
