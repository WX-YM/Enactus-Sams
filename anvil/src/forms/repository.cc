// versioned-write-exempt: `submission_count` is a monotonic counter and the ONLY
// unversioned write on this class. It is a bare `$inc`, which the server applies
// atomically — there is no read, so there is no read-modify-write and nothing to
// lose. Putting it behind update_versioned would be strictly worse: two people
// submitting the same public form at the same moment would collide on the
// definition's version and one would be told VERSION_MISMATCH, which is not a
// thing that has happened to them. Every OTHER write here — the definition edit —
// goes through anvil/db/versioned.h.

#include "anvil/forms/repository.h"

#include <chrono>
#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/model/update_one.hpp>
#include <mongocxx/options/bulk_write.hpp>
#include <mongocxx/options/count.hpp>
#include <mongocxx/options/find.hpp>

#include "anvil/core/uuid.h"
#include "anvil/db/versioned.h"

namespace anvil::forms {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace f = form_fields;

constexpr std::int64_t kCountLimit = 1;

[[nodiscard]] bsoncxx::types::b_string text_of(std::string_view value) noexcept {
    return bsoncxx::types::b_string{codec::key_of(value)};
}

// A stored localised value is owned strings; the codec takes views. One
// conversion here rather than at every append site.
[[nodiscard]] LocalizedView view_of(const LocalizedText& text) noexcept {
    LocalizedView out{};
    for (std::size_t i = 0; i < kLocaleCount; ++i) { out.values[i] = text[i]; }
    return out;
}

void assign_localized(LocalizedText& out, LocalizedView text) {
    for (std::size_t i = 0; i < kLocaleCount; ++i) { out[i].assign(text.values[i]); }
}

// --- definition encoding ----------------------------------------------------

[[nodiscard]] Status append_field(sub_array& array, const FieldSpec& field) {
    Status label = ok();
    array.append([&field, &label](sub_document doc) {
        doc.append(kvp(codec::key_of(f::kFieldId), text_of(field.fid.view())));
        label = codec::append_localized(doc, f::kLabel, view_of(field.label));
        // int32, never the display name: a type string repeated across every
        // field of every form is wasted bytes on disk and a string compare per
        // field at validation time, where an int is a direct index.
        doc.append(kvp(codec::key_of(f::kType), bsoncxx::types::b_int32{field.type}));
        doc.append(kvp(codec::key_of(f::kOptional), bsoncxx::types::b_bool{field.optional}));
        if (has_flag(field.flags, FieldTypeFlag::Options)) {
            doc.append(kvp(codec::key_of(f::kOptions), [&field, &label](sub_array options) {
                for (const FormOption& option : field.options) {
                    options.append([&option, &label](sub_document entry) {
                        entry.append(kvp(codec::key_of(f::kOptionValue), text_of(option.value)));
                        const Status one =
                            codec::append_localized(entry, f::kLabel, view_of(option.label));
                        if (!one) { label = one; }
                    });
                }
            }));
        }
        if (has_flag(field.flags, FieldTypeFlag::Ranged)) {
            doc.append(kvp(codec::key_of(f::kMinValue),
                           bsoncxx::types::b_int64{field.min_value}));
            doc.append(kvp(codec::key_of(f::kMaxValue),
                           bsoncxx::types::b_int64{field.max_value}));
        }
        if (has_flag(field.flags, FieldTypeFlag::CodePointCapped)) {
            doc.append(kvp(codec::key_of(f::kMaxCodePoints),
                           bsoncxx::types::b_int32{
                               static_cast<std::int32_t>(field.max_code_points)}));
        }
        if (has_flag(field.flags, FieldTypeFlag::MultiSelect)) {
            doc.append(kvp(codec::key_of(f::kMaxSelections),
                           bsoncxx::types::b_int32{
                               static_cast<std::int32_t>(field.max_selections)}));
        }
    });
    return label;
}

// The `$set` body of a definition: everything except `_id`, the version and the
// submission counter. Those three are written by whoever creates the document and
// are never part of an edit.
[[nodiscard]] Result<bsoncxx::document::value> definition_body(const FormDefinition& form) {
    bsoncxx::builder::basic::document doc;
    codec::append_uuid(doc, f::kCreator, form.creator);
    if (const Status title = codec::append_localized(doc, f::kTitle, view_of(form.title));
        !title) {
        return title.error();
    }
    Status fields = ok();
    doc.append(kvp(codec::key_of(f::kFields), [&form, &fields](sub_array array) {
        for (const FieldSpec& field : form.fields) {
            const Status one = append_field(array, field);
            if (!one) { fields = one; }
        }
    }));
    if (!fields) { return fields.error(); }
    // DERIVED at the service and written here. There is no path that takes this
    // from a request body.
    doc.append(kvp(codec::key_of(f::kHasPii), bsoncxx::types::b_bool{form.has_pii}));
    codec::append_enum(doc, f::kStatus, form.status);
    codec::append_int64(doc, f::kMaxSubmissions, form.max_submissions);
    doc.append(kvp(codec::key_of(f::kOnePerUser), bsoncxx::types::b_bool{form.one_per_user}));
    codec::append_optional_time(doc, f::kClosesAt, form.closes_at);
    return doc.extract();
}

// --- definition decoding ----------------------------------------------------

[[nodiscard]] Result<FormOption> decode_option(const bsoncxx::document::view& doc) {
    const Result<std::string_view> value = codec::read_text(doc, f::kOptionValue);
    if (!value) { return value.error(); }
    const Result<LocalizedView> label = codec::read_localized(doc, f::kLabel);
    if (!label) { return label.error(); }

    FormOption option{};
    option.value.assign(value.value());
    assign_localized(option.label, label.value());
    return option;
}

[[nodiscard]] Result<FieldSpec> decode_field(const bsoncxx::document::view& doc,
                                             std::span<const FieldTypeSpec> types) {
    const Result<std::string_view> raw_fid = codec::read_text(doc, f::kFieldId);
    if (!raw_fid) { return raw_fid.error(); }
    // Re-parsed on the way OUT as well. A stored id that no longer matches the
    // grammar is corruption or an older writer, and it must not become a BSON key
    // again just because it is already on disk.
    const std::optional<Fid> fid = Fid::parse(raw_fid.value());
    if (!fid.has_value()) { return fail(ErrorCode::Internal, f::kFieldId); }

    const Result<LocalizedView> label = codec::read_localized(doc, f::kLabel);
    if (!label) { return label.error(); }
    const Result<std::int32_t> type = codec::read_int32(doc, f::kType);
    if (!type) { return type.error(); }
    const Result<bool> optional_field = codec::read_bool(doc, f::kOptional);
    if (!optional_field) { return optional_field.error(); }

    FieldSpec field{};
    assign_localized(field.label, label.value());
    field.fid = *fid;
    field.type = type.value();
    field.optional = optional_field.value();

    // The cap is read BEFORE the type is bound, because binding is what resolves
    // an absent cap to the type's default — and a stored value above the global
    // ceiling has to fail there rather than be quietly replaced.
    if (doc[codec::key_of(f::kMaxCodePoints)]) {
        const Result<std::int32_t> max_cp = codec::read_int32(doc, f::kMaxCodePoints);
        if (!max_cp) { return max_cp.error(); }
        if (max_cp.value() < 0) { return fail(ErrorCode::Internal, f::kMaxCodePoints); }
        field.max_code_points = static_cast<std::uint32_t>(max_cp.value());
    }
    // A type this build does not declare is an integrity fault, not a field with
    // no rules. It is also a real state during a rolling deploy, which is why a
    // retired code must never be reused.
    if (const Status bound = bind_field_type(types, field, ErrorCode::Internal, f::kType);
        !bound) {
        return bound.error();
    }

    if (has_flag(field.flags, FieldTypeFlag::Options)) {
        const bsoncxx::document::element options = doc[codec::key_of(f::kOptions)];
        if (!options || options.type() != bsoncxx::type::k_array) {
            return fail(ErrorCode::Internal, f::kOptions);
        }
        for (const bsoncxx::array::element& entry : options.get_array().value) {
            if (field.options.size() >= kMaxFieldOptions) {
                return fail(ErrorCode::Internal, f::kOptions);
            }
            if (entry.type() != bsoncxx::type::k_document) {
                return fail(ErrorCode::Internal, f::kOptions);
            }
            Result<FormOption> option = decode_option(entry.get_document().value);
            if (!option) { return option.error(); }
            field.options.push_back(std::move(option).value());
        }
    }
    if (has_flag(field.flags, FieldTypeFlag::Ranged)) {
        const Result<std::int64_t> min_value = codec::read_int64(doc, f::kMinValue);
        if (!min_value) { return min_value.error(); }
        const Result<std::int64_t> max_value = codec::read_int64(doc, f::kMaxValue);
        if (!max_value) { return max_value.error(); }
        field.min_value = min_value.value();
        field.max_value = max_value.value();
    }
    if (has_flag(field.flags, FieldTypeFlag::MultiSelect) &&
        doc[codec::key_of(f::kMaxSelections)]) {
        const Result<std::int32_t> max_sel = codec::read_int32(doc, f::kMaxSelections);
        if (!max_sel) { return max_sel.error(); }
        if (max_sel.value() < 0) { return fail(ErrorCode::Internal, f::kMaxSelections); }
        field.max_selections = static_cast<std::uint32_t>(max_sel.value());
    }
    return field;
}

[[nodiscard]] Result<FormDefinition> decode_definition(const bsoncxx::document::view& doc,
                                                       std::span<const FieldTypeSpec> types) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<Uuid> creator = codec::read_uuid(doc, f::kCreator);
    if (!creator) { return creator.error(); }
    const Result<LocalizedView> title = codec::read_localized(doc, f::kTitle);
    if (!title) { return title.error(); }
    const Result<FormStatus> status = codec::read_enum(doc, f::kStatus, kMaxFormStatus);
    if (!status) { return status.error(); }
    const Result<bool> has_pii = codec::read_bool(doc, f::kHasPii);
    if (!has_pii) { return has_pii.error(); }
    const Result<bool> one_per_user = codec::read_bool(doc, f::kOnePerUser);
    if (!one_per_user) { return one_per_user.error(); }
    const Result<std::int64_t> max_submissions = codec::read_int64(doc, f::kMaxSubmissions);
    if (!max_submissions) { return max_submissions.error(); }
    const Result<std::optional<db::TimeMs>> closes_at =
        codec::read_optional_time(doc, f::kClosesAt);
    if (!closes_at) { return closes_at.error(); }
    const Result<std::int64_t> version = repo::document_version(doc);
    if (!version) { return version.error(); }
    // Absent rather than defaulted to zero: a definition written without a
    // counter is a document this system did not write.
    const Result<std::int64_t> count = codec::read_int64(doc, f::kSubmissionCount);
    if (!count) { return count.error(); }

    FormDefinition form{};
    assign_localized(form.title, title.value());
    form.id = id.value();
    form.creator = creator.value();
    form.closes_at = closes_at.value();
    form.version = version.value();
    form.submission_count = count.value();
    form.max_submissions = max_submissions.value();
    form.status = status.value();
    form.has_pii = has_pii.value();
    form.one_per_user = one_per_user.value();

    const bsoncxx::document::element fields = doc[codec::key_of(f::kFields)];
    if (!fields || fields.type() != bsoncxx::type::k_array) {
        return fail(ErrorCode::Internal, f::kFields);
    }
    for (const bsoncxx::array::element& entry : fields.get_array().value) {
        if (form.fields.size() >= kMaxFormFields) { return fail(ErrorCode::Internal, f::kFields); }
        if (entry.type() != bsoncxx::type::k_document) {
            return fail(ErrorCode::Internal, f::kFields);
        }
        Result<FieldSpec> field = decode_field(entry.get_document().value, types);
        if (!field) { return field.error(); }
        form.fields.push_back(std::move(field).value());
    }
    return form;
}

// --- submission encoding ----------------------------------------------------

void append_answer(sub_document& doc, const Answer& answer) {
    switch (answer.kind) {
        case AnswerKind::Text:
        case AnswerKind::Choice:
            doc.append(kvp(codec::key_of(answer.fid.view()), text_of(answer.text)));
            return;
        case AnswerKind::Number:
            // b_int64, never b_double. The stored type is the guarantee.
            doc.append(kvp(codec::key_of(answer.fid.view()),
                           bsoncxx::types::b_int64{answer.number}));
            return;
        case AnswerKind::Choices:
            doc.append(kvp(codec::key_of(answer.fid.view()), [&answer](sub_array array) {
                for (const std::string& choice : answer.choices) { array.append(text_of(choice)); }
            }));
            return;
        case AnswerKind::Media:
            doc.append(kvp(codec::key_of(answer.fid.view()), codec::uuid_bin(answer.media)));
            return;
    }
}

[[nodiscard]] bsoncxx::document::value submission_document(const SubmissionRecord& record) {
    bsoncxx::builder::basic::document doc;
    codec::append_uuid(doc, f::kId, record.id);
    codec::append_uuid(doc, f::kForm, record.form);
    // OMITTED when anonymous, never null. `uniq` carries the second copy the
    // partial unique index constrains and is written ONLY when the form asks for
    // one submission per person — a null there would make every anonymous row
    // collide with every other one.
    if (record.user.has_value()) {
        codec::append_uuid(doc, f::kUser, *record.user);
        if (record.enforce_one_per_user) { codec::append_uuid(doc, f::kUnique, *record.user); }
    }
    doc.append(kvp(codec::key_of(f::kAnswers), [&record](sub_document answers) {
        for (const Answer& answer : record.answers) { append_answer(answers, answer); }
    }));
    if (record.has_pii) {
        doc.append(kvp(codec::key_of(f::kPii),
                       bsoncxx::types::b_binary{
                           bsoncxx::binary_sub_type::k_binary,
                           static_cast<std::uint32_t>(record.pii_envelope.size()),
                           record.pii_envelope.data()}));
        doc.append(kvp(codec::key_of(f::kPiiRedacted), text_of(record.pii_redacted)));
        codec::append_digest(doc, f::kPiiIndex, record.pii_index);
    }
    // Omitted when empty for the same reason `uid` is: the unique index on this
    // array is partial on `$exists`, so an empty array would put every
    // attachment-free submission into it.
    if (!record.media.empty()) {
        doc.append(kvp(codec::key_of(f::kMedia), [&record](sub_array array) {
            for (const Uuid& id : record.media) { array.append(codec::uuid_bin(id)); }
        }));
    }
    doc.append(kvp(codec::key_of(f::kIp), codec::bytes_bin(record.ip)));
    codec::append_time(doc, f::kSubmittedAt, record.submitted_at);
    codec::append_int64(doc, f::kFormVersion, record.form_version);
    return doc.extract();
}

// --- submission decoding ----------------------------------------------------

[[nodiscard]] Result<Uuid> uuid_of(const bsoncxx::types::b_binary& raw) {
    if (raw.sub_type != bsoncxx::binary_sub_type::k_uuid || raw.size != 16) {
        return fail(ErrorCode::Internal, f::kMedia);
    }
    Uuid id{};
    for (std::size_t i = 0; i < id.size(); ++i) { id[i] = raw.bytes[i]; }
    return id;
}

[[nodiscard]] Result<Answer> decode_answer(const Fid& fid, const FieldSpec& field,
                                           const bsoncxx::document::element& element) {
    // A PII field with a value in `ans` means something wrote one there, which is
    // exactly what the storage rule forbids. Reported as an integrity fault rather
    // than decoded — the point of the rule is that no read path can produce one.
    if (has_flag(field.flags, FieldTypeFlag::Pii)) {
        return fail(ErrorCode::Internal, f::kAnswers);
    }

    // The expected shape comes from answer_kind_of, which is also what the
    // submission service held the validator to. One derivation, so the writer and
    // the reader cannot drift apart.
    Answer answer{};
    answer.fid = fid;
    answer.kind = answer_kind_of(field.flags);
    switch (answer.kind) {
        case AnswerKind::Media: {
            if (element.type() != bsoncxx::type::k_binary) {
                return fail(ErrorCode::Internal, f::kAnswers);
            }
            const Result<Uuid> media = uuid_of(element.get_binary());
            if (!media) { return media.error(); }
            answer.media = media.value();
            return answer;
        }
        case AnswerKind::Choices: {
            if (element.type() != bsoncxx::type::k_array) {
                return fail(ErrorCode::Internal, f::kAnswers);
            }
            for (const bsoncxx::array::element& choice : element.get_array().value) {
                if (choice.type() != bsoncxx::type::k_string) {
                    return fail(ErrorCode::Internal, f::kAnswers);
                }
                if (answer.choices.size() >= kMaxFieldOptions) {
                    return fail(ErrorCode::Internal, f::kAnswers);
                }
                const bsoncxx::stdx::string_view text = choice.get_string().value;
                answer.choices.emplace_back(text.data(), text.size());
            }
            return answer;
        }
        case AnswerKind::Number: {
            // int64, never double. The stored type is the guarantee.
            if (element.type() != bsoncxx::type::k_int64) {
                return fail(ErrorCode::Internal, f::kAnswers);
            }
            answer.number = element.get_int64().value;
            return answer;
        }
        case AnswerKind::Choice:
        case AnswerKind::Text: {
            if (element.type() != bsoncxx::type::k_string) {
                return fail(ErrorCode::Internal, f::kAnswers);
            }
            answer.text.assign(element.get_string().value.data(),
                               element.get_string().value.size());
            return answer;
        }
    }
    return fail(ErrorCode::Internal, f::kAnswers);
}

[[nodiscard]] Result<SubmissionRecord> decode_submission(const bsoncxx::document::view& doc,
                                                         const FormDefinition& form) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<Uuid> form_id = codec::read_uuid(doc, f::kForm);
    if (!form_id) { return form_id.error(); }
    const Result<db::TimeMs> submitted_at = codec::read_time(doc, f::kSubmittedAt);
    if (!submitted_at) { return submitted_at.error(); }
    const Result<std::int64_t> form_version = codec::read_int64(doc, f::kFormVersion);
    if (!form_version) { return form_version.error(); }

    SubmissionRecord record{};
    record.id = id.value();
    record.form = form_id.value();
    record.submitted_at = submitted_at.value();
    record.form_version = form_version.value();

    // Omitted for anonymous rows, so absence is the normal case rather than an
    // error.
    if (doc[codec::key_of(f::kUser)]) {
        const Result<Uuid> user = codec::read_uuid(doc, f::kUser);
        if (!user) { return user.error(); }
        record.user = user.value();
    }
    if (doc[codec::key_of(f::kIp)]) {
        const Status ip = codec::read_bytes(doc, f::kIp, record.ip);
        if (!ip) { return ip.error(); }
    }

    const bsoncxx::document::element answers = doc[codec::key_of(f::kAnswers)];
    if (!answers || answers.type() != bsoncxx::type::k_document) {
        return fail(ErrorCode::Internal, f::kAnswers);
    }
    for (const bsoncxx::document::element& entry : answers.get_document().value) {
        const std::optional<Fid> fid =
            Fid::parse(std::string_view{entry.key().data(), entry.key().size()});
        if (!fid.has_value()) { return fail(ErrorCode::Internal, f::kAnswers); }
        const FieldSpec* spec = form.find_field(*fid);
        // A stored answer whose field the definition no longer names is SKIPPED
        // rather than reported: an edit may have removed an optional field, and
        // the row has to stay readable. The submission's own `fv` records which
        // definition version it was validated against.
        if (spec == nullptr) { continue; }
        Result<Answer> answer = decode_answer(*fid, *spec, entry);
        if (!answer) { return answer.error(); }
        record.answers.push_back(std::move(answer).value());
    }

    if (const bsoncxx::document::element envelope = doc[codec::key_of(f::kPii)]; envelope) {
        if (envelope.type() == bsoncxx::type::k_binary) {
            const bsoncxx::types::b_binary raw = envelope.get_binary();
            record.pii_envelope.assign(raw.bytes, raw.bytes + raw.size);
            record.has_pii = true;
        } else if (envelope.type() != bsoncxx::type::k_null) {
            return fail(ErrorCode::Internal, f::kPii);
        }
    }
    if (record.has_pii) {
        const Result<crypto::Digest256> index = codec::read_digest(doc, f::kPiiIndex);
        if (!index) { return index.error(); }
        record.pii_index = index.value();
        const Result<std::string_view> redacted = codec::read_text(doc, f::kPiiRedacted);
        if (!redacted) { return redacted.error(); }
        record.pii_redacted.assign(redacted.value());
    }

    if (const bsoncxx::document::element media = doc[codec::key_of(f::kMedia)]; media) {
        if (media.type() != bsoncxx::type::k_array) { return fail(ErrorCode::Internal, f::kMedia); }
        for (const bsoncxx::array::element& entry : media.get_array().value) {
            if (record.media.size() >= kMaxSubmissionMedia) {
                return fail(ErrorCode::Internal, f::kMedia);
            }
            if (entry.type() != bsoncxx::type::k_binary) {
                return fail(ErrorCode::Internal, f::kMedia);
            }
            const Result<Uuid> bound = uuid_of(entry.get_binary());
            if (!bound) { return bound.error(); }
            record.media.push_back(bound.value());
        }
    }
    return record;
}

// The ONE place the retrieval filter is written, so no caller can forget the
// discriminator that makes a partitioned collection safe.
void append_form_filter(bsoncxx::builder::basic::document& filter, const Uuid& form) {
    codec::append_uuid(filter, f::kForm, form);
}

void append_cursor(bsoncxx::builder::basic::document& filter, const SubmissionCursor& cursor) {
    filter.append(kvp("$or", [cursor](sub_array branches) {
        branches.append([cursor](sub_document sub) {
            sub.append(kvp(codec::key_of(f::kSubmittedAt), [cursor](sub_document range) {
                range.append(kvp("$lt", codec::time_date(cursor.submitted_at)));
            }));
        });
        branches.append([cursor](sub_document sub) {
            sub.append(kvp(codec::key_of(f::kSubmittedAt), codec::time_date(cursor.submitted_at)));
            sub.append(kvp(codec::key_of(f::kId), [cursor](sub_document range) {
                range.append(kvp("$lt", codec::uuid_bin(cursor.id)));
            }));
        });
    }));
}

[[nodiscard]] mongocxx::options::find page_options(std::int32_t limit) {
    mongocxx::options::find options{};
    options.sort(make_document(kvp(codec::key_of(f::kSubmittedAt), bsoncxx::types::b_int32{-1}),
                               kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{-1})));
    options.limit(limit);
    return options;
}

}  // namespace

// --- definitions ------------------------------------------------------------

Status FormRepository::insert_definition(mongocxx::client& client,
                                         const FormDefinition& form) const {
    Result<bsoncxx::document::value> body = definition_body(form);
    if (!body) { return body.error(); }
    return repo::guarded([&]() -> Status {
        mongocxx::collection collection = bind_definitions(client);

        bsoncxx::builder::basic::document doc;
        codec::append_uuid(doc, f::kId, form.id);
        doc.append(bsoncxx::builder::concatenate(body.value().view()));
        codec::append_int64(doc, f::kSubmissionCount, 0);
        repo::append_initial_version(doc);

        collection.insert_one(doc.view());
        return ok();
    });
}

Result<std::optional<FormDefinition>> FormRepository::find_definition(mongocxx::client& client,
                                                                      const Uuid& id) const {
    return repo::guarded([&]() -> Result<std::optional<FormDefinition>> {
        mongocxx::collection collection = bind_definitions(client);

        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, id);

        const auto found = collection.find_one(filter.view());
        if (!found) { return std::optional<FormDefinition>{}; }
        Result<FormDefinition> decoded = decode_definition(found->view(), types_);
        if (!decoded) { return decoded.error(); }
        return std::optional<FormDefinition>{std::move(decoded).value()};
    });
}

Result<std::int64_t> FormRepository::update_definition(mongocxx::client& client, const Uuid& id,
                                                       std::int64_t expected_version,
                                                       const FormDefinition& form) const {
    Result<bsoncxx::document::value> body = definition_body(form);
    if (!body) { return body.error(); }
    mongocxx::collection collection = bind_definitions(client);
    // `submission_count` is deliberately NOT in the `$set`: an edit that rewrote
    // it would race every in-flight submission's `$inc` and silently lose rows
    // from the count that bounds the form.
    const bsoncxx::document::value identity =
        make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(id)));
    return repo::update_versioned(collection, identity.view(), expected_version,
                                  body.value().view());
}

Result<bool> FormRepository::delete_definition(mongocxx::client& client,
                                               mongocxx::client_session& session,
                                               const Uuid& id) const {
    return repo::guarded_in_transaction([&]() -> Result<bool> {
        mongocxx::collection collection = bind_definitions(client);

        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, id);

        const auto result = collection.delete_one(session, filter.view());
        if (!result) { return fail(ErrorCode::Internal); }
        return result->deleted_count() == 1;
    });
}

Result<FormListPage> FormRepository::list_definitions(mongocxx::client& client,
                                                      const std::optional<Uuid>& after,
                                                      std::int32_t limit, Locale locale) const {
    return repo::guarded([&]() -> Result<FormListPage> {
        mongocxx::collection collection = bind_definitions(client);

        const std::string title_path = std::string{f::kTitle} + "." + std::string{locale.tag()};

        // Descending `_id`, which is descending creation time because the ids are
        // v7. The cursor is one `$lt` on the same key — no compound clause,
        // because there is no tie to break.
        bsoncxx::builder::basic::document filter;
        if (after.has_value()) {
            filter.append(kvp(codec::key_of(f::kId), [&after](sub_document sub) {
                sub.append(kvp("$lt", codec::uuid_bin(*after)));
            }));
        }

        mongocxx::options::find options{};
        options.projection(make_document(
            kvp(codec::key_of(title_path), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kStatus), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kHasPii), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kOnePerUser), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(f::kSubmissionCount), bsoncxx::types::b_int32{1}),
            kvp(codec::key_of(repo::kVersionField), bsoncxx::types::b_int32{1})));
        options.sort(make_document(kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{-1})));
        // One more than asked for, so "is there another page" is answered by the
        // same query rather than by a count.
        options.limit(limit + 1);

        FormListPage page{};
        page.entries.reserve(static_cast<std::size_t>(limit));
        for (const bsoncxx::document::view& doc : collection.find(filter.view(), options)) {
            if (page.entries.size() == static_cast<std::size_t>(limit)) {
                page.next = page.entries.back().id;
                break;
            }
            const Result<Uuid> id = codec::read_uuid(doc, f::kId);
            if (!id) { return id.error(); }
            // The projection removed every other locale, so read_localized —
            // which requires them all — is the wrong reader here.
            const bsoncxx::document::element title = doc[codec::key_of(f::kTitle)];
            if (!title || title.type() != bsoncxx::type::k_document) {
                return fail(ErrorCode::Internal, f::kTitle);
            }
            const Result<std::string_view> text =
                codec::read_text(title.get_document().value, locale.tag());
            if (!text) { return fail(ErrorCode::Internal, f::kTitle); }
            const Result<FormStatus> status = codec::read_enum(doc, f::kStatus, kMaxFormStatus);
            if (!status) { return status.error(); }
            const Result<std::int64_t> version = codec::read_int64(doc, repo::kVersionField);
            if (!version) { return version.error(); }
            const Result<std::int64_t> submissions = codec::read_int64(doc, f::kSubmissionCount);
            if (!submissions) { return submissions.error(); }
            const Result<bool> has_pii = codec::read_bool(doc, f::kHasPii);
            if (!has_pii) { return has_pii.error(); }
            const Result<bool> one_per_user = codec::read_bool(doc, f::kOnePerUser);
            if (!one_per_user) { return one_per_user.error(); }

            FormListEntry entry{};
            entry.title.assign(text.value());
            entry.id = id.value();
            entry.created_at =
                db::TimeMs{std::chrono::milliseconds{uuid::v7_timestamp_ms(id.value())}};
            entry.version = version.value();
            entry.submission_count = submissions.value();
            entry.status = status.value();
            entry.has_pii = has_pii.value();
            entry.one_per_user = one_per_user.value();
            page.entries.push_back(std::move(entry));
        }
        return page;
    });
}

// --- submissions ------------------------------------------------------------

Status FormRepository::insert_submission(mongocxx::client& client,
                                         mongocxx::client_session& session,
                                         const SubmissionRecord& record) const {
    return repo::guarded_in_transaction([&]() -> Status {
        bind(client).insert_one(session, submission_document(record).view());
        return ok();
    });
}

Status FormRepository::insert_submission(mongocxx::client& client,
                                         const SubmissionRecord& record) const {
    return repo::guarded([&]() -> Status {
        bind(client).insert_one(submission_document(record).view());
        return ok();
    });
}

Result<std::int64_t> FormRepository::add_submission_counts(
    mongocxx::client& client, std::span<const std::pair<Uuid, std::int64_t>> counts) const {
    if (counts.empty()) { return std::int64_t{0}; }
    return repo::guarded([&]() -> Result<std::int64_t> {
        mongocxx::collection collection = bind_definitions(client);

        // Unordered: one dropped form must not stop the counters behind it.
        mongocxx::options::bulk_write options{};
        options.ordered(false);
        auto bulk = collection.create_bulk_write(options);

        for (const auto& [form, delta] : counts) {
            bulk.append(mongocxx::model::update_one{
                make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(form))).view(),
                make_document(kvp("$inc", [delta](sub_document sub) {
                    sub.append(kvp(codec::key_of(f::kSubmissionCount),
                                   bsoncxx::types::b_int64{delta}));
                })).view()});
        }

        const auto result = bulk.execute();
        if (!result) { return fail(ErrorCode::ServiceUnavailable, f::kForm); }
        // Every form that matched nothing was dropped while its submissions were
        // in flight. Their rows are already committed; the drop reaps them.
        return static_cast<std::int64_t>(counts.size()) -
               static_cast<std::int64_t>(result->matched_count());
    });
}

Result<SubmissionPage> FormRepository::page_of(mongocxx::client& client,
                                               const FormDefinition& definition,
                                               const std::optional<SubmissionCursor>& after,
                                               std::int32_t limit) const {
    return repo::guarded([&]() -> Result<SubmissionPage> {
        mongocxx::collection collection = bind(client);

        bsoncxx::builder::basic::document filter;
        append_form_filter(filter, definition.id);
        if (after.has_value()) { append_cursor(filter, *after); }

        SubmissionPage result{};
        result.entries.reserve(static_cast<std::size_t>(limit));
        for (const bsoncxx::document::view& doc :
             collection.find(filter.view(), page_options(limit + 1))) {
            if (result.entries.size() == static_cast<std::size_t>(limit)) {
                result.next = SubmissionCursor{result.entries.back().id,
                                               result.entries.back().submitted_at};
                break;
            }
            Result<SubmissionRecord> decoded = decode_submission(doc, definition);
            if (!decoded) { return decoded.error(); }
            result.entries.push_back(std::move(decoded).value());
        }
        return result;
    });
}

Result<SubmissionPage> FormRepository::page(mongocxx::client& client, const Uuid& form,
                                            const std::optional<SubmissionCursor>& after,
                                            std::int32_t limit) const {
    // The definition is needed to decode: an answer's BSON type is asserted
    // against its declared field type rather than sniffed.
    const Result<std::optional<FormDefinition>> definition = find_definition(client, form);
    if (!definition) { return definition.error(); }
    if (!definition.value().has_value()) { return fail(ErrorCode::NotFound, f::kForm); }
    return page_of(client, *definition.value(), after, limit);
}

Result<bool> FormRepository::blind_index_seen(mongocxx::client& client, const Uuid& form,
                                              const crypto::Digest256& index) const {
    return repo::guarded([&]() -> Result<bool> {
        mongocxx::collection collection = bind(client);

        bsoncxx::builder::basic::document filter;
        append_form_filter(filter, form);
        // The form is part of the FILTER, not a check on the result: one person
        // may legitimately submit their identity to two different forms, and
        // answering otherwise would leak across forms.
        codec::append_digest(filter, f::kPiiIndex, index);

        mongocxx::options::count options{};
        options.limit(kCountLimit);
        return collection.count_documents(filter.view(), options) > 0;
    });
}

Result<std::int64_t> FormRepository::count_submissions(mongocxx::client& client,
                                                       mongocxx::client_session& session,
                                                       const Uuid& form) const {
    return repo::guarded_in_transaction([&]() -> Result<std::int64_t> {
        mongocxx::collection collection = bind(client);

        bsoncxx::builder::basic::document filter;
        append_form_filter(filter, form);
        return collection.count_documents(session, filter.view());
    });
}

Result<std::vector<Uuid>> FormRepository::bound_media(mongocxx::client& client,
                                                      mongocxx::client_session& session,
                                                      const Uuid& form,
                                                      std::int64_t limit) const {
    return repo::guarded_in_transaction([&]() -> Result<std::vector<Uuid>> {
        mongocxx::collection collection = bind(client);

        bsoncxx::builder::basic::document filter;
        append_form_filter(filter, form);

        mongocxx::options::find options{};
        // Only the attachment array comes back. Pulling whole submissions —
        // answers, envelopes and all — to read one array would cost network, BSON
        // decode CPU and heap for data nothing here reads (ENGINEERING_RULES.md §7).
        options.projection(make_document(kvp(codec::key_of(f::kMedia),
                                             bsoncxx::types::b_int32{1})));
        options.sort(make_document(kvp(codec::key_of(f::kSubmittedAt),
                                       bsoncxx::types::b_int32{-1}),
                                   kvp(codec::key_of(f::kId), bsoncxx::types::b_int32{-1})));
        options.limit(limit);

        std::vector<Uuid> media;
        for (const bsoncxx::document::view& doc :
             collection.find(session, filter.view(), options)) {
            const bsoncxx::document::element field = doc[codec::key_of(f::kMedia)];
            if (!field) { continue; }
            if (field.type() != bsoncxx::type::k_array) {
                return fail(ErrorCode::Internal, f::kMedia);
            }
            for (const bsoncxx::array::element& entry : field.get_array().value) {
                if (entry.type() != bsoncxx::type::k_binary) {
                    return fail(ErrorCode::Internal, f::kMedia);
                }
                const Result<Uuid> bound = uuid_of(entry.get_binary());
                if (!bound) { return bound.error(); }
                media.push_back(bound.value());
            }
        }
        return media;
    });
}

Result<std::int64_t> FormRepository::delete_submissions(mongocxx::client& client,
                                                        mongocxx::client_session& session,
                                                        const Uuid& form) const {
    return repo::guarded_in_transaction([&]() -> Result<std::int64_t> {
        mongocxx::collection collection = bind(client);

        bsoncxx::builder::basic::document filter;
        append_form_filter(filter, form);

        // delete_many, never collection.drop(). A drop is DDL: it takes locks, it
        // is not transactional with the definition delete, and it only exists as
        // an option in a design where each form owns a collection.
        const auto result = collection.delete_many(session, filter.view());
        if (!result) { return fail(ErrorCode::Internal); }
        return result->deleted_count();
    });
}

Status FormRepository::for_each_submission(
    mongocxx::client& client, const FormDefinition& definition, std::int32_t page_size,
    const std::function<bool(const SubmissionRecord&)>& sink) const {
    const std::int32_t bounded =
        page_size < 1 ? 1 : (page_size > kMaxSubmissionPageRows ? kMaxSubmissionPageRows
                                                                : page_size);
    std::optional<SubmissionCursor> cursor;
    while (true) {
        Result<SubmissionPage> current = page_of(client, definition, cursor, bounded);
        if (!current) { return current.error(); }
        // Bounded memory by construction: one page is resident, the previous is
        // released before the next is read. A form with 100 000 submissions never
        // materialises as one vector.
        for (const SubmissionRecord& record : current.value().entries) {
            if (!sink(record)) { return ok(); }
        }
        if (!current.value().next.has_value()) { return ok(); }
        cursor = current.value().next;
    }
}

}  // namespace anvil::forms
