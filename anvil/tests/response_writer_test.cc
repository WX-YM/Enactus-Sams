// The writer that is the declaration.
//
// What these cases assert is the BYTES a declared shape produces, because the
// descriptor emits that declaration as a schema and a generated client is built
// from it. A schema and a body that disagree is the one defect the whole design
// exists to make impossible, and it is invisible to a test that parses the body
// back with something forgiving.
//
// --- what is NOT here, and cannot be ----------------------------------------
//
// The refusals. Naming a key that is not next, naming the right key with the
// wrong type, and closing an object before its last field are `static_assert`s,
// so a case exercising one does not fail — it does not compile. They appear in
// no test count at all, which is the same standing `descriptions_match`'s own
// refusals have (docs/16-test-plan.md), and the way to check one is to write it
// and watch the build stop.
//
// They are not written as `requires` clauses instead, although that would make
// them detectable by a concept. The diagnostic is the point: "this is not the
// next declared field" names the mistake, and "no matching function for call to
// text" names the symptom.

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

#include "anvil/core/types.h"
#include "anvil/http/response_spec.h"
#include "anvil/http/response_writer.h"

namespace anvil::http {
namespace {

// One of each kind the grammar has, so the case below records every spelling
// json_writer.h will produce for a declared field.
inline constexpr std::array<ResponseField, 6> kEveryKind{{
    {"id", FieldKind::Uuid, false},
    {"name", FieldKind::String, false},
    {"count", FieldKind::Int, false},
    {"active", FieldKind::Bool, false},
    {"seen_at_utc", FieldKind::Time, false},
    {"tags", FieldKind::Strings, true},
}};

inline constexpr std::array<ResponseField, 2> kItem{{
    {"key", FieldKind::String, false},
    {"width", FieldKind::Int, false},
}};

inline constexpr std::array<ResponseField, 0> kNothing{};

constexpr Uuid kId{0x01, 0xa0, 0xb4, 0x97, 0x9e, 0x69, 0x7b, 0xd0,
                   0x87, 0xad, 0x15, 0x6b, 0x01, 0xc7, 0x5a, 0x0a};

TEST(ResponseWriter, WritesEachKindInTheSpellingJsonWriterProduces) {
    std::string out;
    constexpr std::array<std::string_view, 2> tags{"one", "two"};
    write_object<kEveryKind>(out)
        .uuid<"id">(kId)
        .text<"name">("a name")
        .number<"count">(7)
        .boolean<"active">(true)
        .time_ms<"seen_at_utc">(1'760'000'000'000)
        .strings<"tags">(tags)
        .done();

    // Literal bytes, in declaration order. A client generated from
    // `"fields":[…]` reads them positionally in some languages and by name in
    // others, and a server free to reorder them breaks the first kind silently.
    EXPECT_EQ(out,
              R"({"id":"01a0b497-9e69-7bd0-87ad-156b01c75a0a",)"
              R"("name":"a name","count":7,"active":true,)"
              R"("seen_at_utc":"2025-10-09T08:53:20.000Z","tags":["one","two"]})");
}

TEST(ResponseWriter, ANullableFieldIsWrittenAsNullAndNotAsAnEmptyValue) {
    // `null` and `[]` are different answers, and the difference is the reason
    // `nullable` is a field on the declaration rather than a convention: a
    // client's type for a nullable field is a different type, and an empty list
    // where null was meant reads as "holds none of these" rather than as "this
    // question does not apply".
    std::string out;
    write_object<kEveryKind>(out)
        .uuid<"id">(kId)
        .text<"name">("a name")
        .number<"count">(0)
        .boolean<"active">(false)
        .time_ms<"seen_at_utc">(0)
        .null_field<"tags">()
        .done();

    EXPECT_NE(out.find(R"("tags":null)"), std::string::npos) << out;
    EXPECT_EQ(out.find(R"("tags":[])"), std::string::npos) << out;
}

TEST(ResponseWriter, AnArrayOfOneShapeSeparatesItsElementsAndNothingElse) {
    // The separator is the one piece of state a compile-time position cannot
    // carry, because the element COUNT is a run-time fact. Everything inside an
    // element is checked exactly as a standalone object is.
    std::string out;
    ArrayWriter<kItem> array{out};
    array.item().text<"key">("thumb").number<"width">(320).done();
    array.item().text<"key">("card").number<"width">(1024).done();
    array.done();

    EXPECT_EQ(out, R"([{"key":"thumb","width":320},{"key":"card","width":1024}])");
}

TEST(ResponseWriter, AnArrayWithNoElementsIsAnEmptyArrayAndNeverNull) {
    // A list route that matched nothing answers with a list. `null` there is a
    // second shape for a client to branch on, for a case that is not an error.
    std::string out;
    ArrayWriter<kItem> array{out};
    array.done();
    EXPECT_EQ(out, "[]");
}

TEST(ResponseWriter, AnObjectDeclaringNothingIsStillAnObject) {
    // The degenerate shape, asserted because it is the one a table reaches by
    // accident — an empty span reads as "undescribed" in the DESCRIPTION and as
    // "describes an empty object" here, and the two are distinguished by which
    // one the emitter sees, not by the writer.
    std::string out;
    write_object<kNothing>(out).done();
    EXPECT_EQ(out, "{}");
}

TEST(ResponseShape, RefusesAnEmptyOrDuplicatedKey) {
    // The two ways a declaration is unusable by a client generator, refused in
    // the `static_assert` an application writes beside its own table. A
    // duplicate is the one that matters: JSON does not forbid it, every parser
    // resolves it differently, and a client generated from the schema would
    // declare one field while the server sent two.
    static constexpr std::array<ResponseField, 1> kEmptyName{{
        {"", FieldKind::String, false},
    }};
    static constexpr std::array<ResponseField, 2> kTwiceNamed{{
        {"id", FieldKind::Uuid, false},
        {"id", FieldKind::String, false},
    }};

    static_assert(!response_shape_is_well_formed(kEmptyName));
    static_assert(!response_shape_is_well_formed(kTwiceNamed));
    static_assert(response_shape_is_well_formed(kEveryKind));
    // An empty shape is well formed. It is how a route says "undescribed", and
    // refusing it here would make the default illegal.
    static_assert(response_shape_is_well_formed(kNothing));
    SUCCEED();
}

TEST(ResponseShape, EveryKindHasADistinctWireName) {
    // A client maps these to its own types, so two kinds sharing a name is two
    // distinctions collapsing into one on the way out.
    constexpr std::array<FieldKind, 6> kAll{FieldKind::String, FieldKind::Int,
                                            FieldKind::Bool,   FieldKind::Uuid,
                                            FieldKind::Time,   FieldKind::Strings};
    for (std::size_t i = 0; i < kAll.size(); ++i) {
        EXPECT_FALSE(kind_name(kAll[i]).empty());
        for (std::size_t j = i + 1; j < kAll.size(); ++j) {
            EXPECT_NE(kind_name(kAll[i]), kind_name(kAll[j]));
        }
    }
}

}  // namespace
}  // namespace anvil::http
