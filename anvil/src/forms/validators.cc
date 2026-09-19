#include "anvil/forms/validators.h"

#include <string>

#include "anvil/core/types.h"
#include "anvil/i18n/bidi.h"
#include "anvil/i18n/digits.h"
#include "anvil/input/fields.h"

namespace anvil::forms {
namespace {

// Every validator starts here. The declared shape is compared against the shape
// the client actually sent, and nothing is coerced: `{"f1": {"$gt": ""}}` fails
// as "not a string" before any value is looked at.
[[nodiscard]] Status require_shape(const RawAnswer& raw, input::JsonType shape) noexcept {
    return raw.shape == shape ? ok() : fail(ErrorCode::ValidationFailed, kTypeField);
}

}  // namespace

bool option_offered(const FieldSpec& field, std::string_view value) noexcept {
    for (const FormOption& option : field.options) {
        if (option.value == value) { return true; }
    }
    return false;
}

namespace validators {

Status text(const FieldSpec& field, const RawAnswer& raw, Answer& out, std::string& identity) {
    (void)identity;
    if (const Status shape = require_shape(raw, input::JsonType::String); !shape) { return shape; }

    const input::TextRules rules{1, field.max_code_points, i18n::TextClass::Prose,
                                 has_flag(field.flags, FieldTypeFlag::MultiLine)};
    if (!input::is_ok(input::check_text(raw.text, rules))) {
        return fail(ErrorCode::ValidationFailed, kRangeField);
    }
    out.kind = AnswerKind::Text;
    out.text = raw.text;
    return ok();
}

Status number(const FieldSpec& field, const RawAnswer& raw, Answer& out, std::string& identity) {
    (void)identity;
    if (const Status shape = require_shape(raw, input::JsonType::Number); !shape) { return shape; }
    if (raw.number < field.min_value || raw.number > field.max_value) {
        return fail(ErrorCode::ValidationFailed, kRangeField);
    }
    out.kind = AnswerKind::Number;
    out.number = raw.number;
    return ok();
}

Status email(const FieldSpec& field, const RawAnswer& raw, Answer& out, std::string& identity) {
    (void)field;
    (void)identity;
    if (const Status shape = require_shape(raw, input::JsonType::String); !shape) { return shape; }
    if (!input::is_ok(input::check_email(raw.text))) {
        return fail(ErrorCode::ValidationFailed, kRangeField);
    }
    out.kind = AnswerKind::Text;
    out.text = raw.text;
    return ok();
}

Status date(const FieldSpec& field, const RawAnswer& raw, Answer& out, std::string& identity) {
    (void)field;
    (void)identity;
    if (const Status shape = require_shape(raw, input::JsonType::String); !shape) { return shape; }

    // Folded first: an Egyptian user typing ٢٠٢٦-٠٢-٠٣ on an Arabic keyboard has
    // entered a date, and rejecting it is a defect wearing the clothes of a
    // control. The allocation is skipped in the all-ASCII case, which is most.
    std::string folded = i18n::has_non_ascii_digits(raw.text) ? i18n::fold_digits(raw.text)
                                                              : raw.text;
    input::CalendarDate parsed{};
    if (!input::is_ok(input::parse_date(folded, parsed))) {
        return fail(ErrorCode::ValidationFailed, kRangeField);
    }
    out.kind = AnswerKind::Text;
    out.text = std::move(folded);
    return ok();
}

Status select_single(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                     std::string& identity) {
    (void)identity;
    if (const Status shape = require_shape(raw, input::JsonType::String); !shape) { return shape; }
    if (!option_offered(field, raw.text)) {
        return fail(ErrorCode::ValidationFailed, kOptionField);
    }
    out.kind = AnswerKind::Choice;
    out.text = raw.text;
    return ok();
}

Status checkbox_multi(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                      std::string& identity) {
    (void)identity;
    if (const Status shape = require_shape(raw, input::JsonType::Array); !shape) { return shape; }

    // Zero means "as many as there are options", which is the only sensible
    // reading of a ceiling the author did not set.
    const std::size_t ceiling =
        field.max_selections == 0 ? field.options.size() : field.max_selections;
    if (raw.choices.size() > ceiling) {
        return fail(ErrorCode::ValidationFailed, kRangeField);
    }
    for (std::size_t i = 0; i < raw.choices.size(); ++i) {
        if (!option_offered(field, raw.choices[i])) {
            return fail(ErrorCode::ValidationFailed, kOptionField);
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (raw.choices[j] == raw.choices[i]) {
                return fail(ErrorCode::ValidationFailed, kOptionField);
            }
        }
    }
    out.kind = AnswerKind::Choices;
    out.choices = raw.choices;
    return ok();
}

Status attachment_uuid(const FieldSpec& field, const RawAnswer& raw, Answer& out,
                       std::string& identity) {
    (void)field;
    (void)identity;
    if (const Status shape = require_shape(raw, input::JsonType::String); !shape) { return shape; }

    Uuid media{};
    if (!input::is_ok(input::parse_uuid(raw.text, media))) {
        return fail(ErrorCode::ValidationFailed, kMediaField);
    }
    out.kind = AnswerKind::Media;
    out.media = media;
    return ok();
}

}  // namespace validators
}  // namespace anvil::forms
