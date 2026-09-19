#include "anvil/forms/export.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>

#include "anvil/core/uuid.h"
#include "anvil/http/csv_writer.h"
#include "anvil/input/fields.h"

namespace anvil::forms {
namespace {

// The fixed columns, before the form's own fields. Never translated: an export is
// read by whoever analyses it, and a header that changes with the reader's locale
// makes two exports of one form impossible to concatenate.
constexpr std::string_view kColumnSubmissionId = "submission_id";
constexpr std::string_view kColumnSubmittedAt = "submitted_at";
constexpr std::string_view kColumnUserId = "user_id";
constexpr std::string_view kColumnFormVersion = "form_version";
constexpr std::string_view kColumnIdentity = "identity";

void append_separator(std::string& out) { out.push_back(','); }

// A UTC instant as `YYYY-MM-DDTHH:MM:SSZ`, through the era arithmetic in
// anvil/input/fields.h rather than through gmtime — which is banned on a request
// path for the static buffer it returns and the timezone database it consults.
void append_instant(std::string& out, db::TimeMs at) {
    const std::int64_t ms = at.time_since_epoch().count();
    // Floor division, so an instant before the epoch does not land on the wrong
    // day. Truncation toward zero would put 1969-12-31T23:59:59Z on 1970-01-01.
    const std::int64_t seconds = ms >= 0 ? ms / 1000 : ((ms - 999) / 1000);
    const std::int64_t days = seconds >= 0 ? seconds / 86400 : ((seconds - 86399) / 86400);
    const std::int64_t rest = seconds - (days * 86400);
    const input::CalendarDate date = input::civil_from_days(days);

    std::array<char, 21> buffer{};
    const auto two = [](std::array<char, 21>& into, std::size_t at_index, std::int64_t value) {
        into[at_index] = static_cast<char>('0' + ((value / 10) % 10));
        into[at_index + 1] = static_cast<char>('0' + (value % 10));
    };
    const std::int64_t year = date.year;
    buffer[0] = static_cast<char>('0' + ((year / 1000) % 10));
    buffer[1] = static_cast<char>('0' + ((year / 100) % 10));
    buffer[2] = static_cast<char>('0' + ((year / 10) % 10));
    buffer[3] = static_cast<char>('0' + (year % 10));
    buffer[4] = '-';
    two(buffer, 5, date.month);
    buffer[7] = '-';
    two(buffer, 8, date.day);
    buffer[10] = 'T';
    two(buffer, 11, rest / 3600);
    buffer[13] = ':';
    two(buffer, 14, (rest / 60) % 60);
    buffer[16] = ':';
    two(buffer, 17, rest % 60);
    buffer[19] = 'Z';
    out.append(buffer.data(), 20);
}

// One answer as one cell. Every branch goes through append_csv_cell, which is
// what makes the formula guard structural rather than a rule each branch
// remembers.
void append_answer_cell(std::string& out, const Answer& answer) {
    switch (answer.kind) {
        case AnswerKind::Text:
        case AnswerKind::Choice:
            http::append_csv_cell(out, answer.text);
            return;
        case AnswerKind::Number: {
            std::array<char, 24> digits{};
            const auto [end, error] =
                std::to_chars(digits.data(), digits.data() + digits.size(), answer.number);
            if (error != std::errc{}) { return; }
            http::append_csv_cell(out, std::string_view{digits.data(),
                                                        static_cast<std::size_t>(end -
                                                                                 digits.data())});
            return;
        }
        case AnswerKind::Choices: {
            // Joined with `; ` into ONE cell rather than spread across columns: a
            // multi-select's column count would otherwise depend on how many
            // boxes the widest respondent ticked, and every row after that one
            // would be misaligned.
            std::string joined;
            for (const std::string& choice : answer.choices) {
                if (!joined.empty()) { joined.append("; "); }
                joined.append(choice);
            }
            http::append_csv_cell(out, joined);
            return;
        }
        case AnswerKind::Media:
            http::append_csv_cell(out, uuid::to_string(answer.media));
            return;
    }
}

}  // namespace

void append_export_header(std::string& out, const FormDefinition& form, Locale locale) {
    out.append(http::kUtf8Bom);

    http::append_csv_cell(out, kColumnSubmissionId);
    append_separator(out);
    http::append_csv_cell(out, kColumnSubmittedAt);
    append_separator(out);
    http::append_csv_cell(out, kColumnUserId);
    append_separator(out);
    http::append_csv_cell(out, kColumnFormVersion);

    for (const FieldSpec& field : form.fields) {
        append_separator(out);
        if (has_flag(field.flags, FieldTypeFlag::Pii)) {
            // One identity column, named for what it is rather than for the
            // field's label: the label is a question ("National ID number") and
            // the column is a disclosure classification.
            http::append_csv_cell(out, kColumnIdentity);
            continue;
        }
        // A staff-authored label is submitted text too — a form author is a
        // likelier source of `=cmd|...` than a form filler, because they can put
        // it in every export of that form rather than in one row of one.
        http::append_csv_cell(out, field.label[locale.index()]);
    }
    http::append_csv_row_end(out);
}

void append_export_row(std::string& out, const FormDefinition& form,
                       const SubmissionRecord& record, std::string_view identity_cell) {
    http::append_csv_cell(out, uuid::to_string(record.id));
    append_separator(out);
    std::string instant;
    append_instant(instant, record.submitted_at);
    http::append_csv_cell(out, instant);
    append_separator(out);
    http::append_csv_cell(out, record.user.has_value() ? uuid::to_string(*record.user)
                                                       : std::string_view{});
    append_separator(out);
    std::array<char, 24> version{};
    const auto [end, error] =
        std::to_chars(version.data(), version.data() + version.size(), record.form_version);
    http::append_csv_cell(out, error == std::errc{}
                                   ? std::string_view{version.data(),
                                                      static_cast<std::size_t>(end -
                                                                               version.data())}
                                   : std::string_view{});

    // Column order is the DEFINITION's field order, not the row's answer order: a
    // spreadsheet column has to mean one thing all the way down.
    for (const FieldSpec& field : form.fields) {
        append_separator(out);
        if (has_flag(field.flags, FieldTypeFlag::Pii)) {
            http::append_csv_cell(out, identity_cell);
            continue;
        }
        const Answer* answer = nullptr;
        for (const Answer& candidate : record.answers) {
            if (candidate.fid == field.fid) {
                answer = &candidate;
                break;
            }
        }
        // No answer is an EMPTY cell, which is also what an optional field sent
        // blank produces. The two are the same thing here by construction.
        if (answer == nullptr) {
            http::append_csv_cell(out, std::string_view{});
            continue;
        }
        append_answer_cell(out, *answer);
    }
    http::append_csv_row_end(out);
}

Status export_submissions(mongocxx::client& client, const FormRepository& forms,
                          const FormDefinition& form, const ExportOptions& options,
                          const ExportSink& sink, const IdentityUnsealFn& unseal) {
    std::string buffer;
    buffer.reserve(kExportChunkBytes + (kExportChunkBytes / 4));

    bool wanted_more = true;
    const auto emit = [&sink, &buffer, &wanted_more](bool force) {
        if (!force && buffer.size() < kExportChunkBytes) { return; }
        if (buffer.empty()) { return; }
        wanted_more = sink(buffer);
        buffer.clear();
    };

    append_export_header(buffer, form, options.locale);

    const Status streamed = forms.for_each_submission(
        client, form, std::clamp(options.page_rows, std::int32_t{1}, kMaxSubmissionPageRows),
        [&](const SubmissionRecord& record) {
            std::string identity;
            if (record.has_pii) {
                // The redacted form is a STORED string and needs no key. Only the
                // opted-in, separately authorised path reaches for one.
                identity = options.include_identity && unseal ? unseal(record)
                                                              : record.pii_redacted;
            }
            append_export_row(buffer, form, record, identity);
            emit(false);
            return wanted_more;
        });
    if (!streamed) { return streamed; }

    // The tail, and the header when a form has no submissions at all: an export
    // of nothing is still a file with its columns in it, which is what tells the
    // operator the form is empty rather than the export broken.
    if (wanted_more) { emit(true); }
    return ok();
}

}  // namespace anvil::forms
