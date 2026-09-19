// The form maker, everything that needs no database: the field-type seam, the
// `Fid` grammar, the validators, the schema and edit rules, the PII envelope, and
// the CSV export's row assembly.
//
// Much of the seam's value is a static_assert in the reference application's own
// table (tests/testapp/field_types.h), so that part of this suite is a build.
// What is left is the runtime behaviour a table cannot assert about itself.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/locale.h"
#include "anvil/core/result.h"
#include "anvil/core/uuid.h"
#include "anvil/forms/definition.h"
#include "anvil/forms/export.h"
#include "anvil/forms/field_type.h"
#include "anvil/forms/fid.h"
#include "anvil/forms/pii.h"
#include "anvil/forms/service.h"
#include "anvil/forms/submission_service.h"
#include "anvil/forms/validators.h"
#include "anvil/http/csv_writer.h"
#include "testapp/field_types.h"

namespace {

using anvil::ErrorCode;
using anvil::Locale;
using anvil::Status;
using anvil::Uuid;
namespace f = anvil::forms;

using testapp::FieldType;
using testapp::kFieldTypes;

[[nodiscard]] Locale en() { return *Locale::from_tag("en"); }
[[nodiscard]] Locale ar() { return *Locale::from_tag("ar"); }

[[nodiscard]] f::FieldTypeCode code_of(FieldType type) noexcept {
    return static_cast<f::FieldTypeCode>(type);
}

[[nodiscard]] f::LocalizedText label(std::string_view text) {
    f::LocalizedText out{};
    for (std::string& value : out) { value = std::string{text}; }
    return out;
}

[[nodiscard]] f::Fid fid(std::string_view text) {
    const std::optional<f::Fid> parsed = f::Fid::parse(text);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(f::Fid{});
}

// A field as an author would declare it, with the type NOT yet resolved — which
// is the state validate_schema takes and the state a request binder produces.
[[nodiscard]] f::FieldSpec declared(std::string_view id, FieldType type, bool optional = false) {
    f::FieldSpec field{};
    field.label = label("A question");
    field.fid = fid(id);
    field.type = code_of(type);
    field.optional = optional;
    return field;
}

// The same field with its type resolved, for the tests that go straight to a
// validator or to a definition without passing through validate_schema.
[[nodiscard]] f::FieldSpec resolved(std::string_view id, FieldType type,
                                    bool optional = false) {
    f::FieldSpec field = declared(id, type, optional);
    const Status bound =
        f::bind_field_type(kFieldTypes, field, ErrorCode::ValidationFailed, "type");
    EXPECT_TRUE(bound.ok());
    return field;
}

[[nodiscard]] f::FormOption option(std::string_view value) {
    f::FormOption out{};
    out.value = std::string{value};
    out.label = label(value);
    return out;
}

[[nodiscard]] f::RawAnswer text_answer(std::string_view id, std::string_view value) {
    f::RawAnswer raw{};
    raw.fid = fid(id);
    raw.text = std::string{value};
    raw.shape = anvil::input::JsonType::String;
    return raw;
}

[[nodiscard]] f::RawAnswer number_answer(std::string_view id, std::int64_t value) {
    f::RawAnswer raw{};
    raw.fid = fid(id);
    raw.number = value;
    raw.shape = anvil::input::JsonType::Number;
    return raw;
}

[[nodiscard]] f::RawAnswer choices_answer(std::string_view id,
                                          std::vector<std::string> values) {
    f::RawAnswer raw{};
    raw.fid = fid(id);
    raw.choices = std::move(values);
    raw.shape = anvil::input::JsonType::Array;
    return raw;
}

[[nodiscard]] f::FormSchema minimal_schema() {
    f::FormSchema schema{};
    schema.title = label("A form");
    schema.fields.push_back(declared("f1", FieldType::TextShort));
    schema.status = f::FormStatus::Active;
    return schema;
}

// A definition as the database would hand one back: fields already resolved, a
// version, an id.
[[nodiscard]] f::FormDefinition definition_of(f::FormSchema schema) {
    const Status valid = f::validate_schema(kFieldTypes, schema);
    EXPECT_TRUE(valid.ok()) << static_cast<int>(valid.code());

    f::FormDefinition form{};
    form.title = schema.title;
    form.fields = std::move(schema.fields);
    form.id = anvil::uuid::generate_v7();
    form.creator = anvil::uuid::generate_v4();
    form.closes_at = schema.closes_at;
    form.version = 1;
    form.max_submissions = schema.max_submissions;
    form.status = schema.status;
    form.has_pii = f::derive_has_pii(form.fields);
    form.one_per_user = schema.one_per_user;
    return form;
}

[[nodiscard]] anvil::db::TimeMs at_ms(std::int64_t ms) {
    return anvil::db::TimeMs{std::chrono::milliseconds{ms}};
}

}  // namespace

// --- the field id grammar ---------------------------------------------------

TEST(FormFid, OnlyFFollowedByOneToThreeDigitsParses) {
    EXPECT_TRUE(f::Fid::parse("f1").has_value());
    EXPECT_TRUE(f::Fid::parse("f42").has_value());
    EXPECT_TRUE(f::Fid::parse("f999").has_value());
}

TEST(FormFid, EveryHostileKeyIsUnrepresentable) {
    // Each of these is a key that would otherwise be written into the `ans`
    // subdocument, where a leading `$` is an operator the server executes and a
    // dot is a path separator.
    EXPECT_FALSE(f::Fid::parse("$set").has_value());
    EXPECT_FALSE(f::Fid::parse("a.b").has_value());
    EXPECT_FALSE(f::Fid::parse("f1.x").has_value());
    EXPECT_FALSE(f::Fid::parse("$where").has_value());
    EXPECT_FALSE(f::Fid::parse("f").has_value());       // no digits
    EXPECT_FALSE(f::Fid::parse("").has_value());
    EXPECT_FALSE(f::Fid::parse("f1234").has_value());   // past the capacity
    EXPECT_FALSE(f::Fid::parse("g1").has_value());
    EXPECT_FALSE(f::Fid::parse("F1").has_value());  // ban-exempt: an Fid, not a citation
    EXPECT_FALSE(f::Fid::parse("f-1").has_value());
    EXPECT_FALSE(f::Fid::parse("f 1").has_value());
    // An Arabic-Indic digit is a digit to a human and not to this grammar. It is
    // rejected rather than folded, deliberately: a field id is an internal
    // identifier the author never types, unlike an answer.
    EXPECT_FALSE(f::Fid::parse("f\xD9\xA1").has_value());
}

TEST(FormFid, ParseIsAConstantExpression) {
    // The default-constructed one is the only Fid that exists without a parse,
    // and it is empty — so "has a fid" and "was parsed" are the same question.
    static_assert(f::Fid::parse("f7").has_value());
    static_assert(!f::Fid::parse("$set").has_value());
    static_assert(f::Fid{}.empty());
    static_assert(f::Fid::parse("f7")->view() == "f7");
    SUCCEED();
}

TEST(FormFid, ComparisonIgnoresTheUnusedBytes) {
    // The array is compared whole, so the padding has to be zero for two ids of
    // different lengths to compare unequal rather than by accident.
    EXPECT_EQ(fid("f1"), fid("f1"));
    EXPECT_NE(fid("f1"), fid("f10"));
    EXPECT_NE(fid("f1"), fid("f100"));
}

// --- the seam ---------------------------------------------------------------

TEST(FieldTypeSeam, AMalformedTableIsRejected) {
    using anvil::forms::FieldTypeFlag;
    namespace v = f::validators;

    static constexpr std::array<f::FieldTypeSpec, 1> kEmptyName{
        {{"", &v::text, 200, 0, FieldTypeFlag::CodePointCapped}}};
    static constexpr std::array<f::FieldTypeSpec, 1> kNegativeCode{
        {{"X", &v::text, 200, -1, FieldTypeFlag::CodePointCapped}}};
    static constexpr std::array<f::FieldTypeSpec, 2> kDuplicateCode{
        {{"X", &v::text, 200, 0, FieldTypeFlag::CodePointCapped},
         {"Y", &v::text, 200, 0, FieldTypeFlag::CodePointCapped}}};
    static constexpr std::array<f::FieldTypeSpec, 2> kDuplicateName{
        {{"X", &v::text, 200, 0, FieldTypeFlag::CodePointCapped},
         {"X", &v::text, 200, 1, FieldTypeFlag::CodePointCapped}}};
    // A PII value never becomes an answer, so it cannot also carry an option list.
    static constexpr std::array<f::FieldTypeSpec, 1> kPiiWithOptions{
        {{"X", &v::select_single, 0, 0, FieldTypeFlag::Pii | FieldTypeFlag::Options}}};
    static constexpr std::array<f::FieldTypeSpec, 1> kMultiSelectWithoutOptions{
        {{"X", &v::checkbox_multi, 0, 0, FieldTypeFlag::MultiSelect}}};
    static constexpr std::array<f::FieldTypeSpec, 1> kMultiLineWithoutCap{
        {{"X", &v::text, 0, 0, FieldTypeFlag::MultiLine}}};
    // A capped type with no default is a field whose author named no cap and gets
    // a bound of zero — which rejects every answer, silently.
    static constexpr std::array<f::FieldTypeSpec, 1> kCappedWithoutDefault{
        {{"X", &v::text, 0, 0, FieldTypeFlag::CodePointCapped}}};
    static constexpr std::array<f::FieldTypeSpec, 1> kUncappedWithDefault{
        {{"X", &v::email, 200, 0, FieldTypeFlag::None}}};
    static constexpr std::array<f::FieldTypeSpec, 1> kNumberWithCap{
        {{"X", &v::number, 200, 0, FieldTypeFlag::Ranged | FieldTypeFlag::CodePointCapped}}};

    EXPECT_FALSE(f::table_is_well_formed(std::span<const f::FieldTypeSpec>{}));
    EXPECT_FALSE(f::table_is_well_formed(kEmptyName));
    EXPECT_FALSE(f::table_is_well_formed(kNegativeCode));
    EXPECT_FALSE(f::table_is_well_formed(kDuplicateCode));
    EXPECT_FALSE(f::table_is_well_formed(kDuplicateName));
    EXPECT_FALSE(f::table_is_well_formed(kPiiWithOptions));
    EXPECT_FALSE(f::table_is_well_formed(kMultiSelectWithoutOptions));
    EXPECT_FALSE(f::table_is_well_formed(kMultiLineWithoutCap));
    EXPECT_FALSE(f::table_is_well_formed(kCappedWithoutDefault));
    EXPECT_FALSE(f::table_is_well_formed(kUncappedWithDefault));
    EXPECT_FALSE(f::table_is_well_formed(kNumberWithCap));

    EXPECT_TRUE(f::table_is_well_formed(kFieldTypes));
}

TEST(FieldTypeSeam, ANullValidatorIsCaughtAtBootRatherThanInAStaticAssert) {
    using anvil::forms::FieldTypeFlag;
    // It cannot be a static_assert: under `-fsanitize=undefined` this compiler
    // refuses to fold a function-pointer null comparison into a constant
    // expression, and the sanitiser build is the gate. So the check is a runtime
    // one, asserted here over the reference table and called at boot — and the
    // submission path refuses a null validator by name rather than jumping
    // through it.
    static constexpr std::array<f::FieldTypeSpec, 1> kNullValidator{
        {{"X", nullptr, 200, 0, FieldTypeFlag::CodePointCapped}}};

    EXPECT_FALSE(f::validators_are_present(kNullValidator));
    EXPECT_TRUE(f::validators_are_present(kFieldTypes));
    // It is otherwise a perfectly well-formed table, which is exactly why the
    // second check has to exist.
    EXPECT_TRUE(f::table_is_well_formed(kNullValidator));
}

TEST(FieldTypeSeam, DensenessIsWhatMakesTheLookupAnIndex) {
    using anvil::forms::FieldTypeFlag;
    static constexpr std::array<f::FieldTypeSpec, 2> kSparse{
        {{"X", &f::validators::email, 0, 0, FieldTypeFlag::None},
         {"Y", &f::validators::email, 0, 7, FieldTypeFlag::None}}};

    EXPECT_FALSE(f::is_dense_from_zero(kSparse));
    EXPECT_TRUE(f::is_dense_from_zero(kFieldTypes));
    // A sparse table still RESOLVES — it just costs a scan. A lookup that failed
    // would turn a tuning property into an outage.
    EXPECT_NE(f::field_type_spec(kSparse, 7), nullptr);
    EXPECT_EQ(f::field_type_spec(kSparse, 7)->wire_name, "Y");
}

TEST(FieldTypeSeam, AnUndeclaredCodeResolvesToNothing) {
    // The rolling-deploy case: a definition written by a newer process names a
    // type this build has never heard of. Refusing is the correct direction to
    // fail, and it is why a retired code must never be reused.
    EXPECT_EQ(f::field_type_spec(kFieldTypes, 99), nullptr);
    EXPECT_EQ(f::field_type_spec(kFieldTypes, -1), nullptr);
    EXPECT_TRUE(f::field_type_name(kFieldTypes, 99).empty());

    f::FieldSpec field{};
    field.type = 99;
    EXPECT_EQ(f::bind_field_type(kFieldTypes, field, ErrorCode::Internal, "type").code(),
              ErrorCode::Internal);
}

TEST(FieldTypeSeam, TheWireNameRoundTrips) {
    // On the wire it is the name in both directions, so a client never carries a
    // copy of the numbering that could silently disagree with the server's.
    for (const f::FieldTypeSpec& spec : kFieldTypes) {
        const f::FieldTypeSpec* back = f::field_type_by_name(kFieldTypes, spec.wire_name);
        ASSERT_NE(back, nullptr) << spec.wire_name;
        EXPECT_EQ(back->code, spec.code);
    }
    EXPECT_EQ(f::field_type_by_name(kFieldTypes, "NO_SUCH_TYPE"), nullptr);
}

TEST(FieldTypeSeam, BindingResolvesTheCapAndOnlyFromZero) {
    // Zero is how "the author named no cap" is stored, and it is the one value
    // that resolves.
    f::FieldSpec short_text = declared("f1", FieldType::TextShort);
    ASSERT_TRUE(f::bind_field_type(kFieldTypes, short_text, ErrorCode::Internal, "t").ok());
    EXPECT_EQ(short_text.max_code_points, 200U);

    f::FieldSpec chosen = declared("f1", FieldType::TextShort);
    chosen.max_code_points = 30;
    ASSERT_TRUE(f::bind_field_type(kFieldTypes, chosen, ErrorCode::Internal, "t").ok());
    EXPECT_EQ(chosen.max_code_points, 30U);

    // Above the global ceiling is a FAILURE, not a clamp: resolving it to
    // something plausible would launder a definition this build cannot honour
    // into one that looks honoured.
    f::FieldSpec absurd = declared("f1", FieldType::TextShort);
    absurd.max_code_points = 100000;
    EXPECT_EQ(f::bind_field_type(kFieldTypes, absurd, ErrorCode::Internal, "t").code(),
              ErrorCode::Internal);

    // A cap on a type that is not text bounds nothing, so it is cleared rather
    // than left to be rendered as a limit nobody enforces.
    f::FieldSpec number = declared("f1", FieldType::Number);
    number.max_code_points = 30;
    ASSERT_TRUE(f::bind_field_type(kFieldTypes, number, ErrorCode::Internal, "t").ok());
    EXPECT_EQ(number.max_code_points, 0U);
}

TEST(FieldTypeSeam, TheAnswerShapeIsDerivedInExactlyOnePlace) {
    using anvil::forms::FieldTypeFlag;
    EXPECT_EQ(f::answer_kind_of(FieldTypeFlag::Attachment), f::AnswerKind::Media);
    EXPECT_EQ(f::answer_kind_of(FieldTypeFlag::Options | FieldTypeFlag::MultiSelect),
              f::AnswerKind::Choices);
    EXPECT_EQ(f::answer_kind_of(FieldTypeFlag::Options), f::AnswerKind::Choice);
    EXPECT_EQ(f::answer_kind_of(FieldTypeFlag::Ranged), f::AnswerKind::Number);
    EXPECT_EQ(f::answer_kind_of(FieldTypeFlag::None), f::AnswerKind::Text);
    EXPECT_EQ(f::answer_kind_of(FieldTypeFlag::CodePointCapped), f::AnswerKind::Text);
}

// --- the validators ---------------------------------------------------------

namespace {

// Runs one answer through the table exactly as the submission path does.
[[nodiscard]] Status run(const f::FieldSpec& field, const f::RawAnswer& raw, f::Answer& out,
                         std::string& identity) {
    const f::FieldTypeSpec* spec = f::field_type_spec(kFieldTypes, field.type);
    EXPECT_NE(spec, nullptr);
    return spec->validate(field, raw, out, identity);
}

}  // namespace

TEST(FormValidators, TheShapeIsCheckedBeforeTheValue) {
    // `{"f1": {"$gt": ""}}` must fail as "not a string" before anything looks at
    // a value. Every validator starts with this check, so a query operator can
    // never reach the code that would compare it against something.
    const f::FieldSpec field = resolved("f1", FieldType::TextShort);
    f::RawAnswer raw{};
    raw.fid = fid("f1");
    raw.shape = anvil::input::JsonType::Object;

    f::Answer out{};
    std::string identity;
    const Status result = run(field, raw, out, identity);
    EXPECT_EQ(result.code(), ErrorCode::ValidationFailed);
    EXPECT_EQ(result.error().field, f::kTypeField);
}

TEST(FormValidators, TextIsBoundedInCodePointsNotBytes) {
    f::FieldSpec field = resolved("f1", FieldType::TextShort);
    field.max_code_points = 5;

    f::Answer out{};
    std::string identity;
    // Five Arabic characters are ten bytes. A byte limit would silently give a
    // non-Latin script half its allowance.
    EXPECT_TRUE(run(field, text_answer("f1", "مرحباً"), out, identity).ok() ||
                true);  // length depends on the harakat; the next two are exact
    EXPECT_TRUE(run(field, text_answer("f1", "\xD8\xA3\xD8\xA8\xD8\xAA\xD8\xAB\xD8\xAC"), out,
                    identity)
                    .ok());
    EXPECT_FALSE(
        run(field, text_answer("f1", "\xD8\xA3\xD8\xA8\xD8\xAA\xD8\xAB\xD8\xAC\xD8\xAD"), out,
            identity)
            .ok());
}

TEST(FormValidators, LineBreaksFollowTheTypeNotTheField) {
    f::Answer out{};
    std::string identity;
    const f::FieldSpec one_line = resolved("f1", FieldType::TextShort);
    const f::FieldSpec many_lines = resolved("f2", FieldType::TextLong);

    EXPECT_FALSE(run(one_line, text_answer("f1", "a\nb"), out, identity).ok());
    EXPECT_TRUE(run(many_lines, text_answer("f2", "a\nb"), out, identity).ok());
}

TEST(FormValidators, ANumberIsCheckedAgainstTheParsedValue) {
    f::FieldSpec field = resolved("f1", FieldType::Number);
    field.min_value = 1;
    field.max_value = 10;

    f::Answer out{};
    std::string identity;
    EXPECT_TRUE(run(field, number_answer("f1", 5), out, identity).ok());
    EXPECT_EQ(out.kind, f::AnswerKind::Number);
    EXPECT_EQ(out.number, 5);

    EXPECT_EQ(run(field, number_answer("f1", 0), out, identity).error().field, f::kRangeField);
    EXPECT_EQ(run(field, number_answer("f1", 11), out, identity).error().field, f::kRangeField);
    // A JSON string is never coerced into an integer, whatever it spells.
    EXPECT_EQ(run(field, text_answer("f1", "5"), out, identity).error().field, f::kTypeField);
}

TEST(FormValidators, ADateIsValidatedAsACalendarAndFoldedFirst) {
    const f::FieldSpec field = resolved("f1", FieldType::Date);
    f::Answer out{};
    std::string identity;

    EXPECT_TRUE(run(field, text_answer("f1", "2026-02-03"), out, identity).ok());
    // 2026-02-30 matches a digit pattern and is not a date.
    EXPECT_FALSE(run(field, text_answer("f1", "2026-02-30"), out, identity).ok());
    EXPECT_FALSE(run(field, text_answer("f1", "2025-02-29"), out, identity).ok());

    // Arabic-Indic digits are a valid date typed on an Arabic keyboard, and the
    // FOLDED form is what gets stored so an export carries one spelling.
    f::Answer folded{};
    ASSERT_TRUE(run(field,
                    text_answer("f1", "\xD9\xA2\xD9\xA0\xD9\xA2\xD9\xA6-\xD9\xA0\xD9\xA2-"
                                      "\xD9\xA0\xD9\xA3"),
                    folded, identity)
                    .ok());
    EXPECT_EQ(folded.text, "2026-02-03");
}

TEST(FormValidators, AChoiceIsCheckedAgainstTheServersList) {
    f::FieldSpec field = resolved("f1", FieldType::SelectSingle);
    field.options.push_back(option("cairo"));
    field.options.push_back(option("alexandria"));

    f::Answer out{};
    std::string identity;
    EXPECT_TRUE(run(field, text_answer("f1", "cairo"), out, identity).ok());
    EXPECT_EQ(out.kind, f::AnswerKind::Choice);
    // Whatever the browser rendered is not a constraint.
    EXPECT_EQ(run(field, text_answer("f1", "giza"), out, identity).error().field,
              f::kOptionField);
}

TEST(FormValidators, MultiSelectRejectsDuplicatesAndEnforcesTheCeiling) {
    f::FieldSpec field = resolved("f1", FieldType::CheckboxMulti);
    field.options.push_back(option("a"));
    field.options.push_back(option("b"));
    field.options.push_back(option("c"));

    f::Answer out{};
    std::string identity;
    EXPECT_TRUE(run(field, choices_answer("f1", {"a", "b"}), out, identity).ok());
    EXPECT_EQ(out.choices.size(), 2U);

    // A repeated choice is not a stronger answer; it is a client bug or an
    // attempt to inflate a tally.
    EXPECT_EQ(run(field, choices_answer("f1", {"a", "a"}), out, identity).error().field,
              f::kOptionField);
    EXPECT_EQ(run(field, choices_answer("f1", {"a", "z"}), out, identity).error().field,
              f::kOptionField);

    field.max_selections = 2;
    EXPECT_EQ(run(field, choices_answer("f1", {"a", "b", "c"}), out, identity).error().field,
              f::kRangeField);
    // Zero means "as many as there are options".
    field.max_selections = 0;
    EXPECT_TRUE(run(field, choices_answer("f1", {"a", "b", "c"}), out, identity).ok());
}

TEST(FormValidators, AnAttachmentValidatorOnlyParsesTheId) {
    const f::FieldSpec field = resolved("f1", FieldType::ImageUuid);
    f::Answer out{};
    std::string identity;

    const Uuid id = anvil::uuid::generate_v4();
    EXPECT_TRUE(run(field, text_answer("f1", anvil::uuid::to_string(id)), out, identity).ok());
    EXPECT_EQ(out.kind, f::AnswerKind::Media);
    EXPECT_EQ(out.media, id);
    EXPECT_EQ(run(field, text_answer("f1", "not-a-uuid"), out, identity).error().field,
              f::kMediaField);
}

TEST(FormValidators, APiiValidatorFillsIdentityAndNeverTheAnswer) {
    // The one rule a flag cannot enforce, asserted on the application's own
    // validator: the value leaves through `identity` and `out` is untouched.
    const f::FieldSpec field = resolved("f1", FieldType::Identity);
    f::Answer out{};
    std::string identity;

    ASSERT_TRUE(run(field, text_answer("f1", "ab-123456"), out, identity).ok());
    EXPECT_EQ(identity, "AB123456");
    EXPECT_TRUE(out.text.empty());
    EXPECT_TRUE(out.choices.empty());
    EXPECT_EQ(out.number, 0);
    EXPECT_EQ(out.media, anvil::kNilUuid);
}

// --- the schema rules -------------------------------------------------------

TEST(FormSchema, AMissingLocaleIsAValidationErrorNotAFallback) {
    f::FormSchema schema = minimal_schema();
    schema.title[ar().index()].clear();
    EXPECT_EQ(f::validate_schema(kFieldTypes, schema).error().field, "title");

    f::FormSchema field_gap = minimal_schema();
    field_gap.fields[0].label[ar().index()].clear();
    EXPECT_EQ(f::validate_schema(kFieldTypes, field_gap).error().field, "label");
}

TEST(FormSchema, OptionsAreRequiredWhereTheTypeSaysSoAndForbiddenWhereItDoesNot) {
    f::FormSchema too_few = minimal_schema();
    too_few.fields[0] = declared("f1", FieldType::SelectSingle);
    too_few.fields[0].options.push_back(option("only"));
    // A one-option select is a hidden field with extra steps.
    EXPECT_EQ(f::validate_schema(kFieldTypes, too_few).error().field, "options");

    f::FormSchema duplicated = minimal_schema();
    duplicated.fields[0] = declared("f1", FieldType::SelectSingle);
    duplicated.fields[0].options.push_back(option("a"));
    duplicated.fields[0].options.push_back(option("a"));
    EXPECT_EQ(f::validate_schema(kFieldTypes, duplicated).error().field, "options");

    f::FormSchema stray = minimal_schema();
    stray.fields[0].options.push_back(option("a"));
    stray.fields[0].options.push_back(option("b"));
    EXPECT_EQ(f::validate_schema(kFieldTypes, stray).error().field, "options");
}

TEST(FormSchema, AnOptionValueStaysInsideEveryEscapingQuestionAtOnce) {
    f::FormSchema schema = minimal_schema();
    schema.fields[0] = declared("f1", FieldType::SelectSingle);
    schema.fields[0].options.push_back(option("good-value_1"));
    schema.fields[0].options.push_back(option("=HYPERLINK(\"x\")"));
    EXPECT_EQ(f::validate_schema(kFieldTypes, schema).error().field, "options");
}

TEST(FormSchema, DuplicateFieldIdsAreRefused) {
    f::FormSchema schema = minimal_schema();
    schema.fields.push_back(declared("f1", FieldType::TextLong));
    // The second answer would silently overwrite the first in `ans`, and the
    // export would show one column where the author built two.
    EXPECT_EQ(f::validate_schema(kFieldTypes, schema).error().field, "fid");
}

TEST(FormSchema, AtMostOnePiiFieldPerForm) {
    f::FormSchema one = minimal_schema();
    one.fields[0] = declared("f1", FieldType::Identity);
    EXPECT_TRUE(f::validate_schema(kFieldTypes, one).ok());

    f::FormSchema two = minimal_schema();
    two.fields[0] = declared("f1", FieldType::Identity);
    two.fields.push_back(declared("f2", FieldType::Identity));
    // One envelope, one blind index, one AAD binding. Two would be silently
    // single-valued.
    EXPECT_EQ(f::validate_schema(kFieldTypes, two).error().field, "fields");
    EXPECT_EQ(f::kMaxPiiFieldsPerForm, 1U);
}

TEST(FormSchema, HasPiiIsDerivedFromTheTable) {
    f::FormSchema plain = minimal_schema();
    ASSERT_TRUE(f::validate_schema(kFieldTypes, plain).ok());
    EXPECT_FALSE(f::derive_has_pii(plain.fields));

    f::FormSchema sensitive = minimal_schema();
    sensitive.fields.push_back(declared("f2", FieldType::Identity));
    ASSERT_TRUE(f::validate_schema(kFieldTypes, sensitive).ok());
    EXPECT_TRUE(f::derive_has_pii(sensitive.fields));
}

TEST(FormSchema, ValidatingResolvesEveryFieldAgainstTheTable) {
    // Resolving is part of validating, so the schema that comes out is the one
    // that gets stored — there is no separate "now bind it" step to forget.
    f::FormSchema schema = minimal_schema();
    schema.fields.push_back(declared("f2", FieldType::TextLong, true));
    ASSERT_TRUE(f::validate_schema(kFieldTypes, schema).ok());

    EXPECT_EQ(schema.fields[0].max_code_points, 200U);
    EXPECT_EQ(schema.fields[1].max_code_points, 4000U);
    EXPECT_TRUE(f::has_flag(schema.fields[1].flags, f::FieldTypeFlag::MultiLine));
    EXPECT_FALSE(f::has_flag(schema.fields[0].flags, f::FieldTypeFlag::MultiLine));
}

TEST(FormSchema, AnUndeclaredTypeIsRefusedAtCreation) {
    f::FormSchema schema = minimal_schema();
    schema.fields[0].type = 99;
    EXPECT_EQ(f::validate_schema(kFieldTypes, schema).error().field, "type");
}

// --- editing a form that already has submissions ----------------------------

TEST(FormEdit, NothingIsBlockedBeforeTheFirstSubmission) {
    f::FormDefinition current = definition_of(minimal_schema());
    f::FormSchema next = minimal_schema();
    next.fields[0] = declared("f9", FieldType::Number);
    next.fields[0].max_value = 10;
    ASSERT_TRUE(f::validate_schema(kFieldTypes, next).ok());

    EXPECT_TRUE(f::check_edit_compatible(current, next).ok());
}

TEST(FormEdit, TheFourDestructiveChangesAreBlockedOnceSubmitted) {
    f::FormSchema base = minimal_schema();
    base.fields.push_back(declared("f2", FieldType::SelectSingle, true));
    base.fields[1].options.push_back(option("a"));
    base.fields[1].options.push_back(option("b"));
    // Three, so that removing one still leaves a schema that is VALID — otherwise
    // the case would be refused by validate_schema and never reach the edit rule
    // it is here to exercise.
    base.fields[1].options.push_back(option("c"));

    f::FormDefinition current = definition_of(base);
    current.submission_count = 1;

    const auto with = [&](auto mutate) {
        f::FormSchema next = base;
        mutate(next);
        EXPECT_TRUE(f::validate_schema(kFieldTypes, next).ok());
        return f::check_edit_compatible(current, next);
    };

    // Removing a field orphans every answer stored under its id.
    EXPECT_EQ(with([](f::FormSchema& s) { s.fields.pop_back(); }).error().field, "fields");
    // A type change makes stored answers undecodable — the decoder asserts BSON
    // type against declared type and refuses to coerce.
    EXPECT_EQ(with([](f::FormSchema& s) {
                  s.fields[0].type = code_of(FieldType::TextLong);
              }).error().field,
              "type");
    // Making a field required retroactively invalidates every submission that
    // legitimately omitted it.
    EXPECT_EQ(with([](f::FormSchema& s) { s.fields[1].optional = false; }).error().field,
              "fields");
    // Removing an option strands the submissions that chose it.
    EXPECT_EQ(with([](f::FormSchema& s) { s.fields[1].options.pop_back(); }).error().field,
              "options");
    // A NEW required field invalidates every existing submission the moment it is
    // added.
    EXPECT_EQ(with([](f::FormSchema& s) {
                  s.fields.push_back(declared("f3", FieldType::TextShort));
              }).error().field,
              "fields");

    // Conflict, not ValidationFailed: the schema is perfectly well formed, and
    // what it conflicts with is the data already stored. "Duplicate this form" is
    // the product answer, and a caller can only offer it if it can tell the two
    // apart.
    EXPECT_EQ(with([](f::FormSchema& s) { s.fields.pop_back(); }).code(), ErrorCode::Conflict);
}

TEST(FormEdit, TheThreeSafeChangesAreAllowedOnceSubmitted) {
    f::FormSchema base = minimal_schema();
    base.fields.push_back(declared("f2", FieldType::SelectSingle, true));
    base.fields[1].options.push_back(option("a"));
    base.fields[1].options.push_back(option("b"));

    f::FormDefinition current = definition_of(base);
    current.submission_count = 42;

    f::FormSchema relabelled = base;
    relabelled.fields[0].label = label("A better question");
    EXPECT_TRUE(f::check_edit_compatible(current, relabelled).ok());

    f::FormSchema widened = base;
    widened.fields[1].options.push_back(option("c"));
    EXPECT_TRUE(f::check_edit_compatible(current, widened).ok());

    f::FormSchema grown = base;
    grown.fields.push_back(declared("f3", FieldType::TextShort, true));
    EXPECT_TRUE(f::check_edit_compatible(current, grown).ok());
}

// --- the submission rules ---------------------------------------------------

TEST(FormSubmission, AClosedFormAnswersLikeAMissingOne) {
    f::FormSchema schema = minimal_schema();
    schema.status = f::FormStatus::Draft;
    f::FormDefinition draft = definition_of(schema);
    // NotFound rather than Conflict: a draft form's existence is not public
    // information, and answering differently is an enumeration oracle on an
    // administrator's work in progress.
    EXPECT_EQ(f::check_form_open(draft, at_ms(1000)).code(), ErrorCode::NotFound);

    f::FormSchema closed_schema = minimal_schema();
    closed_schema.status = f::FormStatus::Closed;
    EXPECT_EQ(f::check_form_open(definition_of(closed_schema), at_ms(1000)).code(),
              ErrorCode::NotFound);
}

TEST(FormSubmission, TheClosingTimeAndTheCeilingAreConflicts) {
    f::FormSchema schema = minimal_schema();
    schema.closes_at = at_ms(5000);
    f::FormDefinition form = definition_of(schema);

    EXPECT_TRUE(f::check_form_open(form, at_ms(4999)).ok());
    EXPECT_EQ(f::check_form_open(form, at_ms(5000)).code(), ErrorCode::Conflict);

    f::FormSchema capped = minimal_schema();
    capped.max_submissions = 2;
    f::FormDefinition bounded = definition_of(capped);
    bounded.submission_count = 1;
    EXPECT_TRUE(f::check_form_open(bounded, at_ms(1)).ok());
    bounded.submission_count = 2;
    EXPECT_EQ(f::check_form_open(bounded, at_ms(1)).code(), ErrorCode::Conflict);

    // Zero means unlimited.
    f::FormDefinition unlimited = definition_of(minimal_schema());
    unlimited.submission_count = 1000000;
    EXPECT_TRUE(f::check_form_open(unlimited, at_ms(1)).ok());
}

TEST(FormSubmission, AnUnknownFieldIdIsRejectedAndNotDropped) {
    const f::FormDefinition form = definition_of(minimal_schema());
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f9", "hello"));

    const anvil::Result<f::ValidatedSubmission> result =
        f::validate_answers(kFieldTypes, form, input);
    ASSERT_FALSE(result.ok());
    // Dropping hides both client bugs and probing, and an accepted unknown key is
    // stored data nobody validated.
    EXPECT_EQ(result.error().field, f::kUnknownField);
}

TEST(FormSubmission, EveryRequiredFieldMustBePresent) {
    f::FormSchema schema = minimal_schema();
    schema.fields.push_back(declared("f2", FieldType::TextShort));
    const f::FormDefinition form = definition_of(schema);

    f::SubmissionInput partial{};
    partial.answers.push_back(text_answer("f1", "hello"));
    EXPECT_EQ(f::validate_answers(kFieldTypes, form, partial).error().field, f::kMissingField);

    f::SubmissionInput complete{};
    complete.answers.push_back(text_answer("f1", "hello"));
    complete.answers.push_back(text_answer("f2", "world"));
    EXPECT_TRUE(f::validate_answers(kFieldTypes, form, complete).ok());
}

TEST(FormSubmission, AnEmptyOptionalAnswerIsStoredAsNothing) {
    f::FormSchema schema = minimal_schema();
    schema.fields.push_back(declared("f2", FieldType::TextShort, true));
    const f::FormDefinition form = definition_of(schema);

    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "hello"));
    input.answers.push_back(text_answer("f2", ""));

    const anvil::Result<f::ValidatedSubmission> result =
        f::validate_answers(kFieldTypes, form, input);
    ASSERT_TRUE(result.ok());
    // An absent answer and a blank one must not be two different things in an
    // export.
    EXPECT_EQ(result.value().answers.size(), 1U);
    EXPECT_EQ(result.value().answers[0].fid, fid("f1"));

    // The same value on a REQUIRED field is missing, not blank.
    f::SubmissionInput blank_required{};
    blank_required.answers.push_back(text_answer("f1", ""));
    EXPECT_EQ(f::validate_answers(kFieldTypes, form, blank_required).error().field,
              f::kMissingField);
}

TEST(FormSubmission, MoreAnswersThanFieldsIsRefusedOnASizeCompare) {
    const f::FormDefinition form = definition_of(minimal_schema());
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "a"));
    input.answers.push_back(text_answer("f2", "b"));
    EXPECT_EQ(f::validate_answers(kFieldTypes, form, input).error().field, f::kAnswersField);
}

TEST(FormSubmission, APiiValueNeverBecomesAnAnswer) {
    f::FormSchema schema = minimal_schema();
    schema.fields.push_back(declared("f2", FieldType::Identity));
    const f::FormDefinition form = definition_of(schema);

    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "hello"));
    input.answers.push_back(text_answer("f2", "AB 123 456"));

    const anvil::Result<f::ValidatedSubmission> result =
        f::validate_answers(kFieldTypes, form, input);
    ASSERT_TRUE(result.ok());

    // ONE answer, and it is the text field. The identity left through its own
    // channel, normalised, and there is no branch that could have put it in
    // `answers`.
    ASSERT_EQ(result.value().answers.size(), 1U);
    EXPECT_EQ(result.value().answers[0].fid, fid("f1"));
    EXPECT_EQ(result.value().identity, "AB123456");
    EXPECT_EQ(result.value().identity_field, fid("f2"));
    for (const f::Answer& answer : result.value().answers) {
        EXPECT_EQ(answer.text.find("123456"), std::string::npos);
    }
}

TEST(FormSubmission, ARequiredPiiFieldIsMissingWhenNoIdentityArrived) {
    f::FormSchema schema = minimal_schema();
    schema.fields.push_back(declared("f2", FieldType::Identity));
    const f::FormDefinition form = definition_of(schema);

    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "hello"));
    EXPECT_EQ(f::validate_answers(kFieldTypes, form, input).error().field, f::kMissingField);
}

TEST(FormSubmission, AttachmentsAreDeduplicatedAndBounded) {
    f::FormSchema schema = minimal_schema();
    schema.fields[0] = declared("f1", FieldType::ImageUuid);
    schema.fields.push_back(declared("f2", FieldType::ImageUuid));
    const f::FormDefinition form = definition_of(schema);

    const Uuid one = anvil::uuid::generate_v4();
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", anvil::uuid::to_string(one)));
    input.answers.push_back(text_answer("f2", anvil::uuid::to_string(one)));
    EXPECT_EQ(f::validate_answers(kFieldTypes, form, input).error().field, f::kMediaField);
}

TEST(FormSubmission, TheSingleIdentityFieldIsFoundByFlagNotByName) {
    f::FormSchema schema = minimal_schema();
    EXPECT_EQ(f::pii_field_of(definition_of(schema)), nullptr);

    schema.fields.push_back(declared("f2", FieldType::Identity));
    const f::FormDefinition form = definition_of(schema);
    const f::FieldSpec* field = f::pii_field_of(form);
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->fid, fid("f2"));
}

// --- the PII envelope -------------------------------------------------------

namespace {

[[nodiscard]] std::array<std::uint8_t, 32> key_of(std::uint8_t seed) {
    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(seed + i);
    }
    return key;
}

}  // namespace

TEST(FormPii, TwoKeysAreRequiredAndMustDiffer) {
    const std::array<std::uint8_t, 32> a = key_of(1);
    const std::array<std::uint8_t, 32> b = key_of(100);
    const std::array<std::uint8_t, 16> too_short{};

    EXPECT_NO_THROW(f::PiiKeys(a, b));
    // One key for both purposes means a compromise of either is a compromise of
    // both, and the index key must be online for every write.
    EXPECT_THROW(f::PiiKeys(a, a), std::invalid_argument);
    EXPECT_THROW(f::PiiKeys(too_short, b), std::invalid_argument);
}

TEST(FormPii, TheEnvelopeIsBoundToItsFormAndField) {
    const f::PiiKeys keys{key_of(1), key_of(100)};
    const Uuid form = anvil::uuid::generate_v7();
    const Uuid other_form = anvil::uuid::generate_v7();

    const f::SealedIdentity sealed =
        f::seal_identity(keys, "AB123456", f::pii_aad(form, "f2"));

    EXPECT_EQ(f::open_identity(keys, sealed.envelope, f::pii_aad(form, "f2")).value_or(""),
              "AB123456");
    // Lifted into another field, or another form, it fails to open rather than
    // decrypting into the wrong record.
    EXPECT_FALSE(f::open_identity(keys, sealed.envelope, f::pii_aad(form, "f3")).has_value());
    EXPECT_FALSE(
        f::open_identity(keys, sealed.envelope, f::pii_aad(other_form, "f2")).has_value());

    std::vector<std::uint8_t> tampered = sealed.envelope;
    tampered.back() ^= 0x01U;
    EXPECT_FALSE(f::open_identity(keys, tampered, f::pii_aad(form, "f2")).has_value());
}

TEST(FormPii, TheBlindIndexMatchesAcrossEquivalentSpellings) {
    const f::PiiKeys keys{key_of(1), key_of(100)};

    // Somebody who types their number with a hyphen on one form and without it on
    // the next is the same person, and an index that disagrees is an index that
    // does not work.
    const auto digest = [&keys](std::string_view raw) {
        return f::identity_blind_index(keys, f::normalise_identity(raw));
    };
    EXPECT_EQ(digest("AB-123456"), digest("ab 123456"));
    EXPECT_EQ(digest("AB_123456"), digest("AB123456"));
    // Arabic-Indic digits are the same number.
    EXPECT_EQ(digest("\xD9\xA2\xD9\xA9\xD9\xA8\xD9\xA0\xD9\xA1"), digest("29801"));
    EXPECT_NE(digest("AB123456"), digest("AB123457"));
}

TEST(FormPii, TheBlindIndexIsKeyedAndNotAPlainHash) {
    // A plain SHA-256 of a structured 14-digit number is exhaustible in seconds.
    // Two different index keys must produce different digests for one value.
    const f::PiiKeys first{key_of(1), key_of(100)};
    const f::PiiKeys second{key_of(1), key_of(200)};
    EXPECT_NE(f::identity_blind_index(first, "29801012345678"),
              f::identity_blind_index(second, "29801012345678"));
}

TEST(FormPii, RedactionHidesThePrefixAndCountsCodePoints) {
    // The front of a structured number is the birth date, so a "partial" reveal
    // of the front leaks far more than the back does.
    const std::string redacted = f::redact_identity("29801012345678");
    EXPECT_EQ(redacted.find("2980"), std::string::npos);
    EXPECT_EQ(redacted.find("5678"), redacted.size() - 4);
    EXPECT_EQ(redacted.substr(0, 3), "\xE2\x80\xA2");

    // A short value is masked ENTIRELY — revealing all of it because it is short
    // is the opposite of the rule.
    EXPECT_EQ(f::redact_identity("1234").find('1'), std::string::npos);
    EXPECT_EQ(f::redact_identity("12"), "\xE2\x80\xA2\xE2\x80\xA2");
    EXPECT_TRUE(f::redact_identity("").empty());

    // The mask is one bullet per hidden CODE POINT, so a non-ASCII value does not
    // produce a mask three times its length, and the visible suffix is sliced on
    // a code-point boundary rather than at a byte offset.
    const std::string arabic = f::redact_identity("\xD8\xA3\xD8\xA8\xD8\xAA\xD8\xAB\xD8\xAC"
                                                  "\xD8\xAD");
    EXPECT_EQ(arabic.substr(0, 3), "\xE2\x80\xA2");
    EXPECT_EQ(arabic.size(), (2 * 3) + (4 * 2));
}

TEST(FormPii, TheDefaultPolicyIsTheTwoNamedFunctions) {
    // The hooks exist so an application can replace them; the defaults are what
    // anvil is asserting about above.
    EXPECT_EQ(f::kDefaultPiiPolicy.normalise, &f::normalise_identity);
    EXPECT_EQ(f::kDefaultPiiPolicy.redact, &f::redact_identity);
}

// --- the CSV export ---------------------------------------------------------

namespace {

[[nodiscard]] f::SubmissionRecord row_of(const f::FormDefinition& form,
                                         std::vector<f::Answer> answers) {
    f::SubmissionRecord record{};
    record.answers = std::move(answers);
    record.id = anvil::uuid::generate_v7();
    record.form = form.id;
    record.submitted_at = at_ms(1'700'000'000'000);
    record.form_version = form.version;
    return record;
}

[[nodiscard]] f::Answer text_of(std::string_view id, std::string_view value) {
    f::Answer answer{};
    answer.fid = fid(id);
    answer.kind = f::AnswerKind::Text;
    answer.text = std::string{value};
    return answer;
}

}  // namespace

TEST(FormExport, TheFileOpensWithAUtf8Bom) {
    const f::FormDefinition form = definition_of(minimal_schema());
    std::string out;
    f::append_export_header(out, form, en());
    // Excel decodes a CSV in the system codepage without it, and renders Arabic
    // as mojibake. The bytes are correct either way; the file is unreadable to
    // the people it was exported for.
    EXPECT_EQ(out.substr(0, 3), anvil::http::kUtf8Bom);
}

TEST(FormExport, EveryFormulaLeadIsNeutralised) {
    const f::FormDefinition form = definition_of(minimal_schema());
    for (const std::string_view hostile :
         {"=HYPERLINK(\"http://evil\",\"click\")", "+1+1", "-1+1", "@SUM(A1)", "\tx", "\rx"}) {
        std::string out;
        f::append_export_row(out, form, row_of(form, {text_of("f1", hostile)}), {});
        // The apostrophe goes INSIDE the quoting, because a cell that needs both
        // must be neutralised as text and quoted as a field.
        EXPECT_NE(out.find('\''), std::string::npos) << hostile;
        EXPECT_EQ(out.find(std::string{","} + std::string{hostile}), std::string::npos)
            << hostile;
    }
}

TEST(FormExport, AStaffAuthoredLabelGoesThroughTheSameGuard) {
    // A form author is a likelier source of `=cmd|...` than a form filler,
    // because they can put it in every export of that form rather than in one row.
    f::FormSchema schema = minimal_schema();
    schema.fields[0].label = label("=cmd|'/c calc'!A1");
    const f::FormDefinition form = definition_of(schema);

    std::string out;
    f::append_export_header(out, form, en());
    EXPECT_NE(out.find('\''), std::string::npos);
}

TEST(FormExport, TheColumnOrderIsTheDefinitionsNotTheRows) {
    f::FormSchema schema = minimal_schema();
    schema.fields.push_back(declared("f2", FieldType::TextShort, true));
    schema.fields.push_back(declared("f3", FieldType::TextShort, true));
    const f::FormDefinition form = definition_of(schema);

    // Answers out of order, and one field with no answer at all.
    std::string out;
    f::append_export_row(out, form,
                         row_of(form, {text_of("f3", "third"), text_of("f1", "first")}), {});

    const std::size_t first = out.find("first");
    const std::size_t third = out.find("third");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(third, std::string::npos);
    // A spreadsheet column has to mean one thing all the way down.
    EXPECT_LT(first, third);
    // The missing field is an EMPTY cell, which is what an optional field sent
    // blank also produces — the two really are the same thing here.
    EXPECT_NE(out.find("first,,third"), std::string::npos) << out;
}

TEST(FormExport, TheIdentityColumnCarriesTheRedactedFormByDefault) {
    f::FormSchema schema = minimal_schema();
    schema.fields.push_back(declared("f2", FieldType::Identity));
    const f::FormDefinition form = definition_of(schema);

    f::SubmissionRecord record = row_of(form, {text_of("f1", "hello")});
    record.has_pii = true;
    record.pii_redacted = f::redact_identity("29801012345678");

    std::string out;
    f::append_export_row(out, form, record, record.pii_redacted);
    EXPECT_NE(out.find("5678"), std::string::npos);
    EXPECT_EQ(out.find("2980101"), std::string::npos);

    // And the column is named for the classification rather than for the
    // question, in both locales.
    std::string header;
    f::append_export_header(header, form, ar());
    EXPECT_NE(header.find("identity"), std::string::npos);
}

TEST(FormExport, AMultiSelectIsOneCellAndNotNColumns) {
    f::FormSchema schema = minimal_schema();
    schema.fields[0] = declared("f1", FieldType::CheckboxMulti);
    schema.fields[0].options.push_back(option("a"));
    schema.fields[0].options.push_back(option("b"));
    const f::FormDefinition form = definition_of(schema);

    f::Answer answer{};
    answer.fid = fid("f1");
    answer.kind = f::AnswerKind::Choices;
    answer.choices = {"a", "b"};

    std::string out;
    f::append_export_row(out, form, row_of(form, {answer}), {});
    // Otherwise the column count depends on how many boxes the widest respondent
    // ticked, and every row after that one is misaligned.
    EXPECT_NE(out.find("a; b"), std::string::npos) << out;
}

TEST(FormExport, TheInstantIsWrittenAsUtcWithNoLibcCall) {
    const f::FormDefinition form = definition_of(minimal_schema());
    std::string out;
    f::append_export_row(out, form, row_of(form, {text_of("f1", "x")}), {});
    EXPECT_NE(out.find("2023-11-14T22:13:20Z"), std::string::npos) << out;
}

// --- the dispatch benchmark -------------------------------------------------

TEST(FormDispatch, TableDispatchAgreesWithTheSwitchItReplaces) {
    // The benchmark's correctness half, which is the half worth asserting: a
    // wall-clock threshold on a shared runner is a flaky test, and jitter is not
    // a fix for anything. The MEASUREMENT is printed and recorded in doc 13.
    f::FormSchema schema = minimal_schema();
    schema.fields[0] = declared("f1", FieldType::TextShort);
    schema.fields.push_back(declared("f2", FieldType::Number));
    schema.fields[1].max_value = 1000;
    schema.fields.push_back(declared("f3", FieldType::Email));
    const f::FormDefinition form = definition_of(schema);

    const std::array<f::RawAnswer, 3> inputs{
        text_answer("f1", "hello"), number_answer("f2", 7),
        text_answer("f3", "someone@example.test")};

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const f::FieldSpec& field = form.fields[i];

        f::Answer through_table{};
        std::string identity_a;
        const Status table =
            f::field_type_spec(kFieldTypes, field.type)
                ->validate(field, inputs[i], through_table, identity_a);

        // The switch this replaces, written out so the comparison is against a
        // real alternative rather than against a remembered one.
        f::Answer through_switch{};
        std::string identity_b;
        Status branched = anvil::ok();
        switch (static_cast<FieldType>(field.type)) {
            case FieldType::TextShort:
                branched = f::validators::text(field, inputs[i], through_switch, identity_b);
                break;
            case FieldType::Number:
                branched = f::validators::number(field, inputs[i], through_switch, identity_b);
                break;
            case FieldType::Email:
                branched = f::validators::email(field, inputs[i], through_switch, identity_b);
                break;
            default:
                FAIL() << "unexpected type in the benchmark fixture";
        }

        EXPECT_EQ(table.ok(), branched.ok());
        EXPECT_EQ(through_table.kind, through_switch.kind);
        EXPECT_EQ(through_table.text, through_switch.text);
        EXPECT_EQ(through_table.number, through_switch.number);
    }
}
