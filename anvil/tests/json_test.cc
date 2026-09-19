// Bounded JSON parse and schema binding.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

#include "anvil/input/json.h"
#include "anvil/input/schema.h"

namespace anvil::input {
namespace {

[[nodiscard]] JsonError error_of(std::string_view body, const JsonLimits& limits = {}) {
    BodyArena arena;
    return parse_json(body, arena, limits).error();
}

// --- 1: the NoSQL-injection shape ----------------------------------------

TEST(Json, OperatorObjectWhereAStringBelongsIsATypeError) {
    // The parse SUCCEEDS — {"email": {"$gt": ""}} is valid JSON — and the bind
    // is where it dies, as "not a string", before any value is inspected and
    // before any BSON exists to inject into.
    BodyArena arena;
    const JsonDocument document = parse_json(R"({"email": {"$gt": ""}})", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    std::string_view email;
    EXPECT_EQ(binder.text("email", kIdentifierRules, email), Reason::BadFormat);
    EXPECT_TRUE(email.empty()) << "nothing is written out of a failed bind";

    // And the operator document is not reachable as a string by any accessor.
    const JsonValue* raw = document.root().find("email");
    ASSERT_NE(raw, nullptr);
    EXPECT_FALSE(raw->as_string().has_value());
    EXPECT_FALSE(raw->as_int64().has_value());
}

// --- 2: depth ------------------------------------------------------------

TEST(Json, DepthNineIsRejectedAndDepthEightIsAccepted) {
    std::string deep;
    for (int i = 0; i < 9; ++i) { deep += "[";  }
    for (int i = 0; i < 9; ++i) { deep += "]"; }
    EXPECT_EQ(error_of(deep), JsonError::DepthExceeded);

    std::string allowed;
    for (int i = 0; i < 8; ++i) { allowed += "["; }
    for (int i = 0; i < 8; ++i) { allowed += "]"; }
    EXPECT_EQ(error_of(allowed), JsonError::Ok);
}

// --- 3: key and element counts -------------------------------------------

TEST(Json, TwoHundredKeysAndOversizedArraysAreRejected) {
    std::string many_keys = "{";
    for (int i = 0; i < 200; ++i) {
        if (i > 0) { many_keys += ","; }
        many_keys += "\"k" + std::to_string(i) + "\":1";
    }
    many_keys += "}";
    EXPECT_EQ(error_of(many_keys), JsonError::TooManyKeys);

    std::string long_array = "[";
    for (int i = 0; i < 257; ++i) {
        if (i > 0) { long_array += ","; }
        long_array += "1";
    }
    long_array += "]";
    EXPECT_EQ(error_of(long_array), JsonError::ArrayTooLong);

    std::string at_limit = "[";
    for (int i = 0; i < 256; ++i) {
        if (i > 0) { at_limit += ","; }
        at_limit += "1";
    }
    at_limit += "]";
    EXPECT_EQ(error_of(at_limit), JsonError::Ok);
}

// --- 4: duplicate keys ---------------------------------------------------

TEST(Json, DuplicateKeysAreRejected) {
    // A parser-differential vector: this reader would see the last value and a
    // proxy or logger the first, so the two disagree about what was submitted.
    EXPECT_EQ(error_of(R"({"role":"client","role":"superadmin"})"), JsonError::DuplicateKey);
}

// --- 5: the size cap comes first -----------------------------------------

TEST(Json, OversizedBodyIsRejectedBeforeUtf8ValidationRuns) {
    JsonLimits limits;
    limits.max_bytes = 1024;

    // Invalid UTF-8 *and* oversized. Reporting TooLarge proves the cheap check
    // ran first: a 100 KB body costs one comparison, not a full validation pass.
    std::string body(2048, '\xC0');
    EXPECT_EQ(error_of(body, limits), JsonError::TooLarge);
}

TEST(Json, MalformedUtf8IsRejectedNeverRepaired) {
    EXPECT_EQ(error_of("{\"t\":\"\xED\xA0\x80\"}"), JsonError::InvalidUtf8);   // lone surrogate
    EXPECT_EQ(error_of("{\"t\":\"\xC0\x80\"}"), JsonError::InvalidUtf8);       // overlong NUL
    // A raw control character is forbidden by the grammar, and a \u0000 escape
    // is well-formed JSON that would embed a NUL, which truncates the value in
    // every C API it later reaches.
    EXPECT_EQ(error_of("{\"t\":\"a\x01b\"}"), JsonError::Malformed);
    EXPECT_EQ(error_of(R"({"t":"a\u0000b"})"), JsonError::Malformed);
}

// --- 6: unknown fields ---------------------------------------------------

TEST(Json, UnknownFieldIsRejectedNeverSilentlyDropped) {
    BodyArena arena;
    const JsonDocument document =
        parse_json(R"({"username":"ahmedy","is_admin":true})", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    std::string_view username;
    ASSERT_EQ(binder.text("username", kIdentifierRules, username), Reason::Ok);

    const std::optional<FieldError> unknown = binder.finish();
    ASSERT_TRUE(unknown.has_value());
    EXPECT_EQ(unknown->reason, Reason::NotAllowed);
    // The key is NOT echoed back: naming it would put client-controlled bytes
    // into a response and a log line.
    EXPECT_TRUE(unknown->field.empty());
}

TEST(Json, BindingEveryFieldLeavesNothingUnclaimed) {
    BodyArena arena;
    const JsonDocument document =
        parse_json(R"({"username":"ahmedy","active":true,"age":33})", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    std::string_view username;
    bool active = false;
    std::int64_t age = 0;
    ASSERT_EQ(binder.text("username", kIdentifierRules, username), Reason::Ok);
    ASSERT_EQ(binder.boolean("active", active), Reason::Ok);
    ASSERT_EQ(binder.integer("age", 0, 200, age), Reason::Ok);

    EXPECT_FALSE(binder.finish().has_value());
    EXPECT_EQ(username, "ahmedy");
    EXPECT_TRUE(active);
    EXPECT_EQ(age, 33);
}

// --- 7-8: arena discipline and the parse budget --------------------------

TEST(Json, StringsWithoutEscapesAreViewsIntoTheBody) {
    // Not a micro-optimisation: it is what keeps an 8 KB body at one arena and
    // no per-field allocation. It is also the lifetime rule — the view dies
    // with the body (ENGINEERING_RULES.md §2.2).
    const std::string body = R"({"t":"no escapes here"})";
    BodyArena arena;
    const JsonDocument document = parse_json(body, arena);
    ASSERT_TRUE(document.ok());

    const std::optional<std::string_view> text = document.root().find("t")->as_string();
    ASSERT_TRUE(text.has_value());
    const char* const inside_body = body.data();
    EXPECT_GE(text->data(), inside_body);
    EXPECT_LT(text->data(), inside_body + body.size());
}

TEST(Json, EscapesAreDecodedIntoTheArena) {
    BodyArena arena;
    const JsonDocument document =
        parse_json(R"({"t":"line\nbreak قهوة 😀"})", arena);
    ASSERT_TRUE(document.ok());

    const std::optional<std::string_view> text = document.root().find("t")->as_string();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "line\nbreak قهوة 😀") << "surrogate pairs decode to one code point";
}

TEST(Json, LoneSurrogateEscapeIsRejected) {
    EXPECT_EQ(error_of(R"({"t":"\ud800"})"), JsonError::Malformed);
    EXPECT_EQ(error_of(R"({"t":"\udc00"})"), JsonError::Malformed);
    EXPECT_EQ(error_of(R"({"t":"\ud800A"})"), JsonError::Malformed);
}

// --- grammar strictness --------------------------------------------------

TEST(Json, TheGrammarIsStrictWithNoLenientMode) {
    EXPECT_EQ(error_of("{\"a\":1,}"), JsonError::Malformed);      // trailing comma
    EXPECT_EQ(error_of("{'a':1}"), JsonError::Malformed);         // single quotes
    EXPECT_EQ(error_of("{\"a\":01}"), JsonError::Malformed);      // leading zero
    EXPECT_EQ(error_of("{\"a\":.5}"), JsonError::Malformed);      // no integer part
    EXPECT_EQ(error_of("{\"a\":1} {\"b\":2}"), JsonError::TrailingData);
    EXPECT_EQ(error_of("// comment\n{}"), JsonError::Malformed);
    EXPECT_EQ(error_of("{\"a\":1"), JsonError::Malformed);        // unterminated
    EXPECT_EQ(error_of(""), JsonError::Malformed);
}

TEST(Json, NumbersReportOverflowRatherThanSaturating) {
    BodyArena arena;
    const JsonDocument document =
        parse_json(R"({"huge":1e309,"fraction":1.5,"big":9223372036854775808})", arena);
    ASSERT_TRUE(document.ok());

    // 1e309 is not a double; reported, never delivered as infinity.
    EXPECT_FALSE(document.root().find("huge")->as_double().has_value());
    // 1.5 is a number but not an integer: a caller asking for an integer gets
    // nothing rather than a silently rounded 1 or 2.
    EXPECT_FALSE(document.root().find("fraction")->as_int64().has_value());
    EXPECT_TRUE(document.root().find("fraction")->as_double().has_value());
    // int64 max plus one.
    EXPECT_FALSE(document.root().find("big")->as_int64().has_value());
}

TEST(Json, BinderRejectsOutOfRangeAndWrongTypedNumbers) {
    BodyArena arena;
    const JsonDocument document =
        parse_json(R"({"amt":-1,"price":"100","qty":1e309})", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    std::int64_t value = 0;
    EXPECT_EQ(binder.integer("amt", 0, 1000, value), Reason::OutOfRange);
    EXPECT_EQ(binder.integer("price", 0, 1000, value), Reason::BadFormat)
        << "a numeric string is not a number";
    EXPECT_EQ(binder.integer("qty", 0, 1000, value), Reason::BadFormat);
}

TEST(Json, OptionalFieldsDistinguishAbsenceFromTheWrongType) {
    BodyArena arena;
    const JsonDocument document = parse_json(R"({"note":null,"count":"7"})", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    std::optional<std::string_view> note;
    std::optional<std::int64_t> count;

    EXPECT_EQ(binder.optional_text("note", kShortProseRules, note), Reason::Ok);
    EXPECT_FALSE(note.has_value()) << "an explicit null means 'leave this alone'";
    EXPECT_EQ(binder.optional_text("missing", kShortProseRules, note), Reason::Ok);
    // Optional is about PRESENCE, never about type discipline.
    EXPECT_EQ(binder.optional_integer("count", 0, 10, count), Reason::BadFormat);
}

TEST(Json, ANonObjectRootFailsEveryBind) {
    BodyArena arena;
    const JsonDocument document = parse_json(R"(["not","an","object"])", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    EXPECT_FALSE(binder.is_object());
    std::string_view text;
    EXPECT_EQ(binder.text("anything", kIdentifierRules, text), Reason::Required);
    EXPECT_TRUE(binder.finish().has_value());
}

TEST(Json, NestedObjectsAreBoundByTheirOwnBinder) {
    BodyArena arena;
    const JsonDocument document =
        parse_json(R"({"title":{"en":"Welcome","ar":"أهلاً","xx":"no"}})", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder outer{document.root()};
    const JsonValue* title = outer.object("title");
    ASSERT_NE(title, nullptr);
    EXPECT_FALSE(outer.finish().has_value()) << "the parent's only key was claimed";

    ObjectBinder inner{*title};
    std::string_view en;
    std::string_view ar;
    ASSERT_EQ(inner.text("en", kShortProseRules, en), Reason::Ok);
    ASSERT_EQ(inner.text("ar", kShortProseRules, ar), Reason::Ok);
    EXPECT_EQ(ar, "أهلاً");
    // The unknown-field rule applies at every level, not only at the top.
    EXPECT_TRUE(inner.finish().has_value());
}

// --- the absent-array trap -------------------------------------------
//
// Every `optional_*` bind reports `Ok` for an absent key. `array` reports
// `Required`, and four shipped routes read that as a refusal — so a form field
// with no options, a permission grant with no revocation beside it, a catalogue
// item with no media and a timetable with no closures were each answered
// `{"<key>":"INVALID"}` for a body that was entirely well formed.
//
// The pair below is the contract that went wrong. It is worth pinning precisely
// because the failure is invisible at the call site: both overloads compile,
// both look right, and only the absent case tells them apart.

TEST(Json, ARequiredArrayReportsAnAbsentKeyAndAnOptionalOneDoesNot) {
    BodyArena arena;
    const JsonDocument document = parse_json(R"({"present":[1,2]})", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    Reason reason = Reason::Ok;

    EXPECT_NE(binder.array("present", 8, reason), nullptr);
    EXPECT_EQ(reason, Reason::Ok);

    // The required overload: absent is a refusal, which is right for `fields` on
    // a form and `week` on a timetable.
    EXPECT_EQ(binder.array("absent", 8, reason), nullptr);
    EXPECT_EQ(reason, Reason::Required);

    // The optional one: absent is not a refusal, matching optional_text and
    // every other optional bind. Same nullptr, different verdict — and the
    // verdict is the whole difference.
    EXPECT_EQ(binder.optional_array("absent", 8, reason), nullptr);
    EXPECT_EQ(reason, Reason::Ok);
}

// The fifth instance of one shape, and the one that shipped with a client in
// front of it: a field bound as REQUIRED where the handler treats it as one of
// several ways to say the same thing.
//
// `POST /notifications/read` takes `up_to` and/or `ids`, and its own handler
// says "one or the other, never neither" three lines further down. It bound
// `ids` with the REQUIRED overload, so the ordinary body the inbox sends was
// refused with `{"ids":"INVALID"}` before that check was ever reached, and
// nothing could be marked read.
TEST(Json, AMarkReadBodyCarryingOnlyAWatermarkIsAccepted) {
    BodyArena arena;
    const JsonDocument document =
        parse_json(R"({"up_to":"01a02f42-916a-7388-a6dd-a7bb808a3948"})", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    std::optional<std::string_view> watermark;
    EXPECT_TRUE(is_ok(binder.optional_text("up_to", kIdentifierRules, watermark)));
    EXPECT_TRUE(watermark.has_value());

    Reason reason = Reason::Ok;
    EXPECT_EQ(binder.optional_array("ids", 50, reason), nullptr);
    EXPECT_EQ(reason, Reason::Ok) << "an absent `ids` is an ordinary body, not a refusal";
    EXPECT_FALSE(binder.finish().has_value());
}

TEST(Json, AnOptionalArrayStillEnforcesTypeAndLength) {
    BodyArena arena;
    const JsonDocument document = parse_json(R"({"wrong":"no","long":[1,2,3]})", arena);
    ASSERT_TRUE(document.ok());

    ObjectBinder binder{document.root()};
    Reason reason = Reason::Ok;

    // Optional is about PRESENCE alone. A key that is there and wrong is still
    // wrong, exactly as optional_integer treats a quoted number.
    EXPECT_EQ(binder.optional_array("wrong", 8, reason), nullptr);
    EXPECT_EQ(reason, Reason::BadFormat);

    EXPECT_EQ(binder.optional_array("long", 2, reason), nullptr);
    EXPECT_EQ(reason, Reason::TooLong);
}

// --- the shape a form definition takes --------------------------------
//
// `parse_json` refuses at `depth >= max_depth`, and a form's deepest legal shape
// is a list question with bilingual option labels:
//
//   0 body → 1 `fields` → 2 a field → 3 `options` → 4 an option
//         → 5 its `label` → 6 the label's string
//
// The form routes capped that at six, so a body carrying any SELECT_SINGLE or
// CHECKBOX_MULTI was refused by the PARSER — before any binder ran, so the
// answer named no field at all. This pins the arithmetic rather than the
// constant: if the schema ever grows a level, this fails and says so.

TEST(Json, AFormDefinitionWithBilingualOptionLabelsNeedsSevenLevels) {
    constexpr std::string_view kListQuestion =
        R"({"title":{"en":"T","ar":"ت"},"fields":[{"fid":"f1","type":"SELECT_SINGLE",)"
        R"("label":{"en":"Size","ar":"مقاس"},)"
        R"("options":[{"value":"m","label":{"en":"Medium","ar":"متوسط"}}]}]})";

    BodyArena six;
    EXPECT_FALSE(parse_json(kListQuestion, six, JsonLimits{.max_bytes = 4096,
                                                           .max_string_bytes = 512,
                                                           .max_depth = 6,
                                                           .max_keys = 16,
                                                           .max_elements = 64})
                     .ok())
        << "six is one short, and this is the cap the form routes shipped with";

    BodyArena seven;
    EXPECT_TRUE(parse_json(kListQuestion, seven, JsonLimits{.max_bytes = 4096,
                                                            .max_string_bytes = 512,
                                                            .max_depth = 7,
                                                            .max_keys = 16,
                                                            .max_elements = 64})
                    .ok());
}

}  // namespace
}  // namespace anvil::input
