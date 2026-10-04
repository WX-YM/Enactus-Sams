#pragma once

// The two form collections: definitions and submissions.
//
// ONE repository over TWO collections, and one database for both. Dropping a form
// deletes its definition and every one of its submissions in a single
// transaction, and stating that invariant is what the pairing buys: a deployment
// that put them in different databases would still work — a replica-set
// transaction spans databases — but there would be two places to look and nothing
// to stop them drifting apart.
//
// This layer ENCODES and DECODES. It does not know what a valid form is: the
// shape rules, the per-type constraints and the edit-compatibility rules all live
// in the service. What it does enforce is that nothing leaves here uninterpreted
// — every stored answer's BSON type is asserted against the type its field
// declares, and a `fid` is re-parsed on the way out as well as on the way in.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/repository.h"
#include "anvil/forms/definition.h"
#include "anvil/forms/field_type.h"

namespace anvil::forms {

// The field names are PUBLISHED for the same reason the user schema's are: the
// index catalogue is the application's and cannot name a column it has to guess.
// An index over a field anvil does not write is an index the planner never uses,
// and the symptom is a collection scan rather than a compile error.
namespace form_fields {

// --- form_definitions --------------------------------------------------------
inline constexpr std::string_view kId = "_id";
inline constexpr std::string_view kCreator = "creator";
inline constexpr std::string_view kTitle = "title";
inline constexpr std::string_view kFields = "fields";
inline constexpr std::string_view kFieldId = "fid";
inline constexpr std::string_view kLabel = "label";
inline constexpr std::string_view kType = "type";
inline constexpr std::string_view kOptional = "opt";
inline constexpr std::string_view kOptions = "options";
inline constexpr std::string_view kOptionValue = "value";
inline constexpr std::string_view kMaxCodePoints = "max_cp";
inline constexpr std::string_view kMinValue = "min";
inline constexpr std::string_view kMaxValue = "max";
inline constexpr std::string_view kMaxSelections = "max_sel";
inline constexpr std::string_view kHasPii = "has_pii";
inline constexpr std::string_view kStatus = "status";
inline constexpr std::string_view kMaxSubmissions = "max_submissions";
inline constexpr std::string_view kOnePerUser = "one_per_user";
inline constexpr std::string_view kClosesAt = "closes_at";
inline constexpr std::string_view kSubmissionCount = "submission_count";

// --- form_submissions --------------------------------------------------------
inline constexpr std::string_view kForm = "form";
inline constexpr std::string_view kUser = "uid";
// The SECOND copy of the user id, written only when the form asks for one
// submission per person. It is what the partial unique index constrains, and it
// is separate from `uid` because attribution and the one-per-user rule are
// different questions.
inline constexpr std::string_view kUnique = "uniq";
inline constexpr std::string_view kAnswers = "ans";
inline constexpr std::string_view kPii = "pii";
inline constexpr std::string_view kPiiRedacted = "pii_r";
inline constexpr std::string_view kPiiIndex = "pii_index";
inline constexpr std::string_view kMedia = "media";
inline constexpr std::string_view kIp = "ip";
inline constexpr std::string_view kSubmittedAt = "submitted_at";
inline constexpr std::string_view kFormVersion = "fv";

}  // namespace form_fields

// A page of the export stream. Large enough that a 100 000-row form is 200 round
// trips rather than 2 000, small enough that one page is tens of kilobytes.
inline constexpr std::int32_t kMaxSubmissionPageRows = 500;

class FormRepository final : public repo::RepositoryBase {
public:
    // `types` is a view of the application's `constexpr` table, which lives in
    // `.rodata` and outlives everything. Decoding needs it: a stored field's
    // flags and its resolved code-point cap come from the table rather than from
    // the document, so a definition naming a type this build does not declare is
    // an integrity fault rather than a field with no rules.
    FormRepository(std::string database, std::string_view definitions,
                   std::string_view submissions, std::span<const FieldTypeSpec> types) noexcept
        : RepositoryBase{std::move(database), submissions},
          definitions_{definitions},
          types_{types} {}

    [[nodiscard]] std::string_view definitions_collection() const noexcept {
        return definitions_;
    }
    [[nodiscard]] std::span<const FieldTypeSpec> field_types() const noexcept { return types_; }

    // --- definitions ---------------------------------------------------------

    [[nodiscard]] Status insert_definition(mongocxx::client& client,
                                           const FormDefinition& form) const;

    [[nodiscard]] Result<std::optional<FormDefinition>> find_definition(
        mongocxx::client& client, const Uuid& id) const;

    // The whole definition is replaced under the caller's expected version, so two
    // staff members editing one form from two tabs cannot lose a write. The rules
    // that decide WHETHER an edit is allowed live in the service; this only
    // enforces that it is not stale.
    [[nodiscard]] Result<std::int64_t> update_definition(mongocxx::client& client, const Uuid& id,
                                                         std::int64_t expected_version,
                                                         const FormDefinition& form) const;

    [[nodiscard]] Result<bool> delete_definition(mongocxx::client& client,
                                                 mongocxx::client_session& session,
                                                 const Uuid& id) const;

    // Every form, newest first, in ONE locale.
    //
    // No status filter and no creator filter, and both omissions are the point:
    // the two states a public read hides — Draft and Closed — are exactly the two
    // a staff member needs this to find, and a form belongs to the organisation
    // rather than to its author.
    //
    // Ordered and paginated by `_id` alone, which works because form ids are
    // UUIDv7 and therefore already in creation order. That is also why no
    // `created_at` field exists on this document at all.
    [[nodiscard]] Result<FormListPage> list_definitions(mongocxx::client& client,
                                                        const std::optional<Uuid>& after,
                                                        std::int32_t limit, Locale locale) const;

    // --- submissions ---------------------------------------------------------

    // Inside the caller's transaction, together with every attachment reference it
    // takes. A submission that commits while its references do not is a file the
    // sweeper deletes out from under a live row.
    [[nodiscard]] Status insert_submission(mongocxx::client& client,
                                           mongocxx::client_session& session,
                                           const SubmissionRecord& record) const;

    // The same insert with NO transaction, for a submission that references
    // nothing. It is then a single-document write, which the server already
    // applies atomically — the condition CLAUDE.md §6 names first, before the
    // transaction alternative.
    //
    // Worth its own overload because a transaction is not free: a session, a
    // commit round trip and a majority-commit wait, all to make one insert atomic
    // that already was. Most submissions carry no attachment at all — an anonymous
    // submitter cannot upload one — so this is the common path.
    [[nodiscard]] Status insert_submission(mongocxx::client& client,
                                           const SubmissionRecord& record) const;

    // One `$inc` per form, all in ONE unordered bulk write.
    //
    // This is what keeps the counter off the request path entirely. N submissions
    // to one form collapse into a single `$inc` of N, so a burst that would have
    // been N round trips and N durable writes becomes one — and the request no
    // longer waits for any of it.
    //
    // Returns the number of forms whose definition no longer exists, which is the
    // dropped-mid-submission case the caller reports.
    [[nodiscard]] Result<std::int64_t> add_submission_counts(
        mongocxx::client& client, std::span<const std::pair<Uuid, std::int64_t>> counts) const;

    [[nodiscard]] Result<SubmissionPage> page(mongocxx::client& client, const Uuid& form,
                                              const std::optional<SubmissionCursor>& after,
                                              std::int32_t limit) const;

    // One page decoded against a definition the caller already holds. The export
    // reads the definition once and pages hundreds of times; making this the
    // primitive keeps a query per 500 rows off the one path where round trips are
    // the whole cost.
    [[nodiscard]] Result<SubmissionPage> page_of(mongocxx::client& client,
                                                 const FormDefinition& definition,
                                                 const std::optional<SubmissionCursor>& after,
                                                 std::int32_t limit) const;

    // Whether this identity has already been submitted to this form, answered
    // WITHOUT decrypting anything. The blind index is what makes that possible.
    [[nodiscard]] Result<bool> blind_index_seen(mongocxx::client& client, const Uuid& form,
                                                const crypto::Digest256& index) const;

    [[nodiscard]] Result<std::int64_t> count_submissions(mongocxx::client& client,
                                                         mongocxx::client_session& session,
                                                         const Uuid& form) const;

    // Every attachment bound by this form's submissions, so a drop can release the
    // references it holds inside the same transaction that destroys them. Bounded
    // by `limit`; the service refuses to drop a form whose submissions exceed it
    // rather than issuing an unbounded read inside a transaction.
    [[nodiscard]] Result<std::vector<Uuid>> bound_media(mongocxx::client& client,
                                                        mongocxx::client_session& session,
                                                        const Uuid& form,
                                                        std::int64_t limit) const;

    // `delete_many`, never a collection drop.
    [[nodiscard]] Result<std::int64_t> delete_submissions(mongocxx::client& client,
                                                          mongocxx::client_session& session,
                                                          const Uuid& form) const;

    // Streams every submission of one form to `sink` in cursor order, holding ONE
    // page at a time. The export path is the reason this exists: a form with
    // 100 000 submissions must never materialise as one vector, let alone as one
    // string.
    //
    // `sink` returns false to stop early. Exceptions from it propagate.
    [[nodiscard]] Status for_each_submission(
        mongocxx::client& client, const FormDefinition& definition, std::int32_t page_size,
        const std::function<bool(const SubmissionRecord&)>& sink) const;

private:
    [[nodiscard]] mongocxx::collection bind_definitions(mongocxx::client& client) const {
        return client[std::string{database()}][std::string{definitions_}];
    }

    // Declaration order is initialisation order; both outlive every instance.
    std::string_view                definitions_;
    std::span<const FieldTypeSpec>  types_;
};

}  // namespace anvil::forms
