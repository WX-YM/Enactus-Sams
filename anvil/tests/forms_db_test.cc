// The form maker against a live cluster.
//
// These cases cannot be unit tests. What they assert is a property of the SERVER
// — that a partial unique index rejects the second submission of a one-per-user
// form, that a multikey unique index rejects a second binding of one object, that
// a versioned update matches nothing once the version has moved, that a drop is
// all-or-nothing — and none of that is observable without one.
//
// The other half is what the STORED DOCUMENT actually contains. "A PII value
// never appears in `ans`" is asserted by reading the raw BSON back and looking,
// rather than by trusting the code path that wrote it: the code path is exactly
// what a future change breaks.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <map>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>

#include "anvil/core/locale.h"
#include "anvil/core/uuid.h"
#include "anvil/forms/export.h"
#include "anvil/forms/pii.h"
#include "anvil/forms/repository.h"
#include "anvil/forms/service.h"
#include "anvil/forms/submission_service.h"
#include "anvil/http/csv_writer.h"
#include "app_fixture.h"
#include "db_fixture.h"
#include "testapp/field_types.h"

namespace {

using anvil::ErrorCode;
using anvil::Locale;
using anvil::Uuid;
using anvil::testfixture::scratch_names;
namespace f = anvil::forms;

using testapp::FieldType;
using testapp::kFieldTypes;

constexpr std::string_view kDefinitions = "form_definitions";
constexpr std::string_view kSubmissions = "form_submissions";

[[nodiscard]] Locale en() { return *Locale::from_tag("en"); }

[[nodiscard]] f::FieldTypeCode code_of(FieldType type) noexcept {
    return static_cast<f::FieldTypeCode>(type);
}

[[nodiscard]] f::LocalizedText label(std::string_view text) {
    f::LocalizedText out{};
    for (std::string& value : out) { value = std::string{text}; }
    return out;
}

[[nodiscard]] f::Fid fid(std::string_view text) { return *f::Fid::parse(text); }

[[nodiscard]] f::FieldSpec declared(std::string_view id, FieldType type, bool optional = false) {
    f::FieldSpec field{};
    field.label = label("A question");
    field.fid = fid(id);
    field.type = code_of(type);
    field.optional = optional;
    return field;
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

// The two keys a deployment holds. Distinct, because one key for both purposes
// means a compromise of either is a compromise of both.
[[nodiscard]] const f::PiiKeys& pii_keys() {
    static const std::array<std::uint8_t, 32> sealing = [] {
        std::array<std::uint8_t, 32> key{};
        for (std::size_t i = 0; i < key.size(); ++i) { key[i] = static_cast<std::uint8_t>(i + 1); }
        return key;
    }();
    static const std::array<std::uint8_t, 32> indexing = [] {
        std::array<std::uint8_t, 32> key{};
        for (std::size_t i = 0; i < key.size(); ++i) {
            key[i] = static_cast<std::uint8_t>(i + 200);
        }
        return key;
    }();
    static const f::PiiKeys keys{sealing, indexing};
    return keys;
}

// A stand-in for whatever owns the objects a form attaches. anvil cannot know
// what "owned by" means for a submitter with no account, so the hooks are the
// application's — and a hand-written one proves the seam from outside, which is
// the only place it can fail cheaply.
class FakeAttachments final {
public:
    void allow(const Uuid& id, const std::optional<Uuid>& owner) { owners_[id] = owner; }

    [[nodiscard]] std::int64_t bound_count(const Uuid& id) const {
        const auto found = bound_.find(id);
        return found == bound_.end() ? 0 : found->second;
    }

    [[nodiscard]] f::AttachmentHooks hooks() {
        f::AttachmentHooks hooks{};
        hooks.may_bind = [this](mongocxx::client&, const Uuid& id,
                                const std::optional<Uuid>& submitter) -> anvil::Result<bool> {
            const auto found = owners_.find(id);
            // "Does not exist" and "is not yours" answer identically. Which ids
            // exist is not something a failed submission confirms.
            if (found == owners_.end()) { return false; }
            return found->second == submitter;
        };
        hooks.bind = [this](mongocxx::client&, mongocxx::client_session&,
                            const Uuid& id) -> anvil::Status {
            ++bound_[id];
            return anvil::ok();
        };
        hooks.release = [this](mongocxx::client&, mongocxx::client_session&,
                               const Uuid& id) -> anvil::Status {
            --bound_[id];
            return anvil::ok();
        };
        return hooks;
    }

private:
    std::map<Uuid, std::optional<Uuid>> owners_;
    std::map<Uuid, std::int64_t>        bound_;
};

class FormDb : public ::testing::Test {
protected:
    void SetUp() override {
        if (!anvil::testfixture::pool_ready()) {
            GTEST_SKIP() << "no MongoDB at " << anvil::testfixture::test_uri();
        }
        anvil::testfixture::ensure_indexes();
        client_ = std::make_unique<mongocxx::pool::entry>(
            anvil::db::MongoPool::instance().acquire());
        anvil::testfixture::clear_collection(**client_, kDefinitions);
        anvil::testfixture::clear_collection(**client_, kSubmissions);
    }

    [[nodiscard]] mongocxx::client& db() { return **client_; }

    [[nodiscard]] static std::string database() {
        return std::string{scratch_names().for_collection(kSubmissions)};
    }

    [[nodiscard]] static f::FormRepository forms() {
        return f::FormRepository{database(), kDefinitions, kSubmissions, kFieldTypes};
    }

    [[nodiscard]] static f::FormSchema plain_schema() {
        f::FormSchema schema{};
        schema.title = label("A form");
        schema.fields.push_back(declared("f1", FieldType::TextShort));
        schema.status = f::FormStatus::Active;
        return schema;
    }

    // The raw stored document, so a test can assert what is ON DISK rather than
    // what the decoder chose to hand back.
    [[nodiscard]] std::string raw_submission_json(const Uuid& id) {
        const auto found =
            db()[database()][std::string{kSubmissions}].find_one(
                bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
                    "_id", anvil::db::codec::uuid_bin(id))));
        return found.has_value() ? bsoncxx::to_json(found->view()) : std::string{};
    }

    std::unique_ptr<mongocxx::pool::entry> client_;
};

[[nodiscard]] anvil::db::TimeMs now() { return anvil::db::now_ms(); }

}  // namespace

// --- definitions ------------------------------------------------------------

TEST_F(FormDb, ADefinitionRoundTripsThroughEveryFieldShape) {
    f::FormSchema schema = plain_schema();
    schema.fields.push_back(declared("f2", FieldType::Number, true));
    schema.fields.back().min_value = -5;
    schema.fields.back().max_value = 5;
    schema.fields.push_back(declared("f3", FieldType::CheckboxMulti, true));
    schema.fields.back().options.push_back({"a", label("Alpha")});
    schema.fields.back().options.push_back({"b", label("Beta")});
    schema.fields.back().max_selections = 1;
    schema.fields.push_back(declared("f4", FieldType::Identity, true));
    schema.max_submissions = 100;
    schema.one_per_user = true;

    f::FormService service{database(), kDefinitions, kSubmissions, kFieldTypes,
                           f::AttachmentHooks{}};
    const anvil::Result<Uuid> created = service.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok()) << static_cast<int>(created.code());

    const anvil::Result<std::optional<f::FormDefinition>> read =
        forms().find_definition(db(), created.value());
    ASSERT_TRUE(read.ok()) << static_cast<int>(read.code());
    ASSERT_TRUE(read.value().has_value());
    const f::FormDefinition& form = *read.value();

    EXPECT_EQ(form.title[en().index()], "A form");
    ASSERT_EQ(form.fields.size(), 4U);
    // The cap was resolved from the TABLE on the way out, not stored twice.
    EXPECT_EQ(form.fields[0].max_code_points, 200U);
    EXPECT_EQ(form.fields[1].min_value, -5);
    EXPECT_EQ(form.fields[1].max_value, 5);
    ASSERT_EQ(form.fields[2].options.size(), 2U);
    EXPECT_EQ(form.fields[2].options[1].value, "b");
    EXPECT_EQ(form.fields[2].options[1].label[en().index()], "Beta");
    EXPECT_EQ(form.fields[2].max_selections, 1U);
    // has_pii is DERIVED from the table and stored, so a listing can read it
    // without decoding every field.
    EXPECT_TRUE(form.has_pii);
    EXPECT_TRUE(form.one_per_user);
    EXPECT_EQ(form.version, 1);
    EXPECT_EQ(form.submission_count, 0);
}

TEST_F(FormDb, ATypeIsStoredAsAnIntAndNotAsItsName) {
    f::FormService service{database(), kDefinitions, kSubmissions, kFieldTypes,
                           f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        service.create(db(), plain_schema(), anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());

    const auto found = db()[database()][std::string{kDefinitions}].find_one(
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("_id", anvil::db::codec::uuid_bin(created.value()))));
    ASSERT_TRUE(found.has_value());
    const std::string json = bsoncxx::to_json(found->view());
    // A type string repeated across every field of every form is wasted bytes on
    // disk and a string compare per field at validation time.
    EXPECT_EQ(json.find("TEXT_SHORT"), std::string::npos) << json;
    EXPECT_NE(json.find("\"type\""), std::string::npos) << json;
}

TEST_F(FormDb, AnEditIsVersionedSoTwoTabsCannotLoseAWrite) {
    f::FormService service{database(), kDefinitions, kSubmissions, kFieldTypes,
                           f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        service.create(db(), plain_schema(), anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());

    f::FormSchema next = plain_schema();
    next.title = label("Renamed");
    const anvil::Result<std::int64_t> first = service.edit(db(), created.value(), 1, next);
    ASSERT_TRUE(first.ok()) << static_cast<int>(first.code());
    EXPECT_EQ(first.value(), 2);

    // The second editor read version 1 as well. It loses, and it is TOLD it lost
    // rather than silently overwriting the first.
    const anvil::Result<std::int64_t> stale = service.edit(db(), created.value(), 1, next);
    EXPECT_EQ(stale.code(), ErrorCode::VersionMismatch);
}

TEST_F(FormDb, AnEditDoesNotRewriteTheSubmissionCounter) {
    // An edit that rewrote it would race every in-flight submission's `$inc` and
    // silently lose rows from the count that bounds the form.
    f::FormService service{database(), kDefinitions, kSubmissions, kFieldTypes,
                           f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        service.create(db(), plain_schema(), anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());

    const std::array<std::pair<Uuid, std::int64_t>, 1> counts{{{created.value(), 7}}};
    ASSERT_TRUE(forms().add_submission_counts(db(), counts).ok());

    f::FormSchema next = plain_schema();
    next.title = label("Renamed");
    ASSERT_TRUE(service.edit(db(), created.value(), 1, next).ok());

    const anvil::Result<std::optional<f::FormDefinition>> read =
        forms().find_definition(db(), created.value());
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value()->submission_count, 7);
}

TEST_F(FormDb, TheListingIsPagedByTheIdAloneAndInOneLocale) {
    f::FormService service{database(), kDefinitions, kSubmissions, kFieldTypes,
                           f::AttachmentHooks{}};
    std::vector<Uuid> created;
    for (int i = 0; i < 5; ++i) {
        f::FormSchema schema = plain_schema();
        schema.title = label("Form " + std::to_string(i));
        const anvil::Result<Uuid> id = service.create(db(), schema, anvil::uuid::generate_v7());
        ASSERT_TRUE(id.ok());
        created.push_back(id.value());
    }

    // Newest first, which is descending `_id` because the ids are v7 — and that
    // is also why there is no stored created_at to disagree with it.
    //
    // The expectation is built from the IDS and not from the insertion order,
    // because two v7 ids minted in the same millisecond order by their random
    // tails (anvil/core/uuid.h). Five creations in a tight loop share a
    // millisecond routinely, and this assertion failed exactly that way on a
    // loaded machine while passing every time it was run alone. What is under
    // test is that the page IS the descending `_id` walk and that the cursor
    // continues it, which is true whatever order the tails fall in.
    std::vector<Uuid> descending = created;
    std::sort(descending.begin(), descending.end(), std::greater<Uuid>{});

    const auto title_of = [&created](const Uuid& id) {
        for (std::size_t i = 0; i < created.size(); ++i) {
            if (created[i] == id) { return "Form " + std::to_string(i); }
        }
        return std::string{"no such form"};
    };

    const anvil::Result<f::FormListPage> first = service.list(db(), std::nullopt, 2, en());
    ASSERT_TRUE(first.ok()) << static_cast<int>(first.code());
    ASSERT_EQ(first.value().entries.size(), 2U);
    ASSERT_TRUE(first.value().next.has_value());
    EXPECT_EQ(first.value().entries[0].id, descending[0]);
    EXPECT_EQ(first.value().entries[1].id, descending[1]);
    EXPECT_EQ(first.value().entries[0].title, title_of(descending[0]));
    EXPECT_GT(first.value().entries[0].created_at.time_since_epoch().count(), 0);

    const anvil::Result<f::FormListPage> second =
        service.list(db(), first.value().next, 2, en());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().entries.size(), 2U);
    EXPECT_EQ(second.value().entries[0].id, descending[2]);
    EXPECT_EQ(second.value().entries[1].id, descending[3]);
}

TEST_F(FormDb, TheDefinitionCacheServesTheHotPathAndAnEditEvictsIt) {
    f::FormService service{database(), kDefinitions, kSubmissions, kFieldTypes,
                           f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        service.create(db(), plain_schema(), anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());

    // A miss is not a cache entry: the first peek answers nullptr, which means
    // "not cached here" and never "does not exist".
    EXPECT_EQ(service.cached(created.value(), now()), nullptr);

    const anvil::Result<std::shared_ptr<const f::FormDefinition>> loaded =
        service.definition(db(), created.value(), now());
    ASSERT_TRUE(loaded.ok());
    const std::shared_ptr<const f::FormDefinition> hit = service.cached(created.value(), now());
    ASSERT_NE(hit, nullptr);
    // The same object, not a second decode.
    EXPECT_EQ(hit.get(), loaded.value().get());

    f::FormSchema next = plain_schema();
    next.title = label("Renamed");
    ASSERT_TRUE(service.edit(db(), created.value(), 1, next).ok());
    EXPECT_EQ(service.cached(created.value(), now()), nullptr);

    // An expired entry is a miss too, and the holder of the old pointer keeps it
    // alive rather than reading a half-updated one.
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> again =
        service.definition(db(), created.value(), now());
    ASSERT_TRUE(again.ok());
    const anvil::db::TimeMs later =
        now() + std::chrono::seconds{f::kDefinitionCacheTtl.count() + 1};
    EXPECT_EQ(service.cached(created.value(), later), nullptr);
}

// --- submissions ------------------------------------------------------------

TEST_F(FormDb, ASubmissionWithNoAttachmentIsOneWriteAndRoundTrips) {
    f::FormSchema schema = plain_schema();
    schema.fields.push_back(declared("f2", FieldType::Number, true));
    schema.fields.back().max_value = 100;

    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "hello"));
    input.answers.push_back(number_answer("f2", 42));

    const anvil::Result<f::SubmissionAccepted> accepted =
        submissions.submit(db(), *form.value(), input, now());
    ASSERT_TRUE(accepted.ok()) << static_cast<int>(accepted.code());
    EXPECT_EQ(accepted.value().form_version, 1);

    const anvil::Result<f::SubmissionPage> page =
        definitions.submissions(db(), created.value(), std::nullopt, 10);
    ASSERT_TRUE(page.ok()) << static_cast<int>(page.code());
    ASSERT_EQ(page.value().entries.size(), 1U);
    const f::SubmissionRecord& row = page.value().entries[0];
    ASSERT_EQ(row.answers.size(), 2U);
    EXPECT_EQ(row.answers[0].text, "hello");
    // int64 on the way out, because int64 is what went in. The stored type is the
    // guarantee, and the decoder refuses to coerce.
    EXPECT_EQ(row.answers[1].kind, f::AnswerKind::Number);
    EXPECT_EQ(row.answers[1].number, 42);
}

TEST_F(FormDb, APiiValueNeverAppearsInTheStoredAnswers) {
    f::FormSchema schema = plain_schema();
    schema.fields.push_back(declared("f2", FieldType::Identity));

    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "hello"));
    input.answers.push_back(text_answer("f2", "29801012345678"));

    const anvil::Result<f::SubmissionAccepted> accepted =
        submissions.submit(db(), *form.value(), input, now());
    ASSERT_TRUE(accepted.ok()) << static_cast<int>(accepted.code());

    // Asserted by INSPECTING THE DOCUMENT, not by trusting the code path that
    // wrote it — the code path is exactly what a future change breaks.
    const std::string json = raw_submission_json(accepted.value().id);
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(json.find("29801012345678"), std::string::npos) << json;
    EXPECT_EQ(json.find("2980101"), std::string::npos) << json;
    // The redacted form IS there, and it discloses only the last four.
    EXPECT_NE(json.find("5678"), std::string::npos) << json;
    EXPECT_NE(json.find("pii_index"), std::string::npos) << json;

    const anvil::Result<f::SubmissionPage> page =
        definitions.submissions(db(), created.value(), std::nullopt, 10);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().entries.size(), 1U);
    const f::SubmissionRecord& row = page.value().entries[0];
    // ONE answer on a two-field form: the identity left through its own channel.
    EXPECT_EQ(row.answers.size(), 1U);
    EXPECT_TRUE(row.has_pii);
    EXPECT_EQ(row.pii_redacted.find("2980"), std::string::npos);

    // The default read path is key-free. Unsealing is a separate call, and it
    // only opens against the form and field the envelope was bound to.
    EXPECT_EQ(submissions.reveal_identity(*form.value(), row).value_or(""), "29801012345678");
}

TEST_F(FormDb, TheSameIdentityTwiceIsCaughtWithoutDecryptingAnything) {
    f::FormSchema schema = plain_schema();
    schema.fields.push_back(declared("f2", FieldType::Identity));

    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    f::SubmissionInput first{};
    first.answers.push_back(text_answer("f1", "hello"));
    first.answers.push_back(text_answer("f2", "29801012345678"));
    ASSERT_TRUE(submissions.submit(db(), *form.value(), first, now()).ok());

    // A different SPELLING of the same identity: separators are stripped by the
    // normaliser, so the blind index collides and the duplicate is caught.
    f::SubmissionInput second{};
    second.answers.push_back(text_answer("f1", "again"));
    second.answers.push_back(text_answer("f2", "2980101-234567-8"));
    EXPECT_EQ(submissions.submit(db(), *form.value(), second, now()).code(), ErrorCode::Conflict);
}

TEST_F(FormDb, OnePerUserIsEnforcedByTheIndexAndNotByACount) {
    f::FormSchema schema = plain_schema();
    schema.one_per_user = true;

    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    const Uuid person = anvil::uuid::generate_v7();
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "hello"));
    input.user = person;

    ASSERT_TRUE(submissions.submit(db(), *form.value(), input, now()).ok());
    // A count-then-insert loses this race to a double-click. The unique index
    // does not, and the duplicate-key error becomes a Conflict.
    EXPECT_EQ(submissions.submit(db(), *form.value(), input, now()).code(), ErrorCode::Conflict);

    // Somebody else is unaffected, and so is an anonymous submitter — `uniq` is
    // written only when the form asks for it, which is what keeps every
    // account-less row out of the index entirely.
    f::SubmissionInput other = input;
    other.user = anvil::uuid::generate_v7();
    EXPECT_TRUE(submissions.submit(db(), *form.value(), other, now()).ok());
}

TEST_F(FormDb, AnonymousSubmissionsDoNotCollideOnAOnePerUserForm) {
    f::FormSchema schema = plain_schema();
    schema.one_per_user = true;

    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "hello"));

    // Two guests. A null `uniq` would have made every anonymous row collide with
    // every other one, which is what the partial `$exists` filter prevents.
    EXPECT_TRUE(submissions.submit(db(), *form.value(), input, now()).ok());
    EXPECT_TRUE(submissions.submit(db(), *form.value(), input, now()).ok());
}

TEST_F(FormDb, AnAttachmentMustBeOwnedAndCanBeBoundOnlyOnce) {
    ANVIL_REQUIRE_TRANSACTIONS();

    f::FormSchema schema = plain_schema();
    schema.fields[0] = declared("f1", FieldType::ImageUuid);

    FakeAttachments attachments;
    const Uuid owner = anvil::uuid::generate_v7();
    const Uuid stranger = anvil::uuid::generate_v7();
    const Uuid object = anvil::uuid::generate_v4();
    attachments.allow(object, owner);

    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               attachments.hooks()};
    const anvil::Result<Uuid> created =
        definitions.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     attachments.hooks(), pii_keys()};
    f::SubmissionInput by_stranger{};
    by_stranger.answers.push_back(text_answer("f1", anvil::uuid::to_string(object)));
    by_stranger.user = stranger;
    // An IDOR with a confidentiality impact if this succeeded: the submitter
    // references somebody else's private object and a staff review screen renders
    // it. NotFound, the same answer a nonexistent id gets.
    EXPECT_EQ(submissions.submit(db(), *form.value(), by_stranger, now()).code(),
              ErrorCode::NotFound);

    // An anonymous submitter cannot upload, so an anonymous body carrying an id
    // is referencing somebody else's object by definition.
    f::SubmissionInput anonymous = by_stranger;
    anonymous.user = std::nullopt;
    EXPECT_EQ(submissions.submit(db(), *form.value(), anonymous, now()).code(),
              ErrorCode::NotFound);

    f::SubmissionInput by_owner = by_stranger;
    by_owner.user = owner;
    ASSERT_TRUE(submissions.submit(db(), *form.value(), by_owner, now()).ok());
    // The reference moved inside the SAME transaction as the row that owns it.
    EXPECT_EQ(attachments.bound_count(object), 1);

    // The unique multikey index is what stops a second submission naming the same
    // object — a read-then-write loses that race.
    EXPECT_EQ(submissions.submit(db(), *form.value(), by_owner, now()).code(),
              ErrorCode::Conflict);
}

TEST_F(FormDb, AFormWithNoAttachmentHooksRefusesAnAttachment) {
    f::FormSchema schema = plain_schema();
    schema.fields[0] = declared("f1", FieldType::ImageUuid);

    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", anvil::uuid::to_string(anvil::uuid::generate_v4())));
    input.user = anvil::uuid::generate_v7();
    // Accepting an id nothing will ever release is how a file becomes unreachable
    // and uncollectable at the same time.
    EXPECT_EQ(submissions.submit(db(), *form.value(), input, now()).code(), ErrorCode::NotFound);
}

TEST_F(FormDb, TheCounterIsBufferedAndCollapsesIntoOneIncrement) {
    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), plain_schema(), anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "hello"));
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(submissions.submit(db(), *form.value(), input, now()).ok());
    }

    // Nothing has been written yet: the count is advisory, and keeping it off the
    // request path is the whole point.
    EXPECT_EQ(submissions.pending_counts(), 1U);
    const anvil::Result<std::optional<f::FormDefinition>> before =
        forms().find_definition(db(), created.value());
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(before.value()->submission_count, 0);

    const f::FormRepository repository = forms();
    std::size_t writes = 0;
    submissions.flush_counts(
        [&](std::span<const std::pair<Uuid, std::int64_t>> counts) -> anvil::Result<std::int64_t> {
            ++writes;
            return repository.add_submission_counts(db(), counts);
        });
    // FIVE submissions, ONE bulk write, one `$inc` of 5.
    EXPECT_EQ(writes, 1U);
    EXPECT_EQ(submissions.pending_counts(), 0U);

    const anvil::Result<std::optional<f::FormDefinition>> after =
        forms().find_definition(db(), created.value());
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value()->submission_count, 5);
}

TEST_F(FormDb, AFlushForADroppedFormReportsItRatherThanFailing) {
    // A form whose `$inc` matches nothing at flush time was dropped while its
    // submissions were in flight. Those rows are already committed and cannot be
    // withdrawn, so the count is discarded and the drop reaps the rows.
    const std::array<std::pair<Uuid, std::int64_t>, 1> orphan{
        {{anvil::uuid::generate_v7(), 3}}};
    const anvil::Result<std::int64_t> missing = forms().add_submission_counts(db(), orphan);
    ASSERT_TRUE(missing.ok());
    EXPECT_EQ(missing.value(), 1);
}

TEST_F(FormDb, ThePageIsCursorOrderedAndNeverSkips) {
    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), plain_schema(), anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    for (int i = 0; i < 5; ++i) {
        f::SubmissionInput input{};
        input.answers.push_back(text_answer("f1", "row " + std::to_string(i)));
        ASSERT_TRUE(submissions
                        .submit(db(), *form.value(), input,
                                anvil::db::TimeMs{std::chrono::milliseconds{1000 + i}})
                        .ok());
    }

    const anvil::Result<f::SubmissionPage> first =
        definitions.submissions(db(), created.value(), std::nullopt, 2);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().entries.size(), 2U);
    ASSERT_TRUE(first.value().next.has_value());
    EXPECT_EQ(first.value().entries[0].answers[0].text, "row 4");

    const anvil::Result<f::SubmissionPage> second =
        definitions.submissions(db(), created.value(), first.value().next, 2);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().entries.size(), 2U);
    EXPECT_EQ(second.value().entries[0].answers[0].text, "row 2");

    const anvil::Result<f::SubmissionPage> third =
        definitions.submissions(db(), created.value(), second.value().next, 2);
    ASSERT_TRUE(third.ok());
    ASSERT_EQ(third.value().entries.size(), 1U);
    EXPECT_FALSE(third.value().next.has_value());
}

TEST_F(FormDb, OneFormsSubmissionsAreNeverVisibleFromAnother) {
    // The discriminator is in EVERY filter against this collection. It is what
    // makes one partitioned collection safe where one collection per form was
    // not, so it is asserted rather than assumed.
    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> left =
        definitions.create(db(), plain_schema(), anvil::uuid::generate_v7());
    const anvil::Result<Uuid> right =
        definitions.create(db(), plain_schema(), anvil::uuid::generate_v7());
    ASSERT_TRUE(left.ok());
    ASSERT_TRUE(right.ok());

    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), left.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    f::SubmissionInput input{};
    input.answers.push_back(text_answer("f1", "left only"));
    ASSERT_TRUE(submissions.submit(db(), *form.value(), input, now()).ok());

    const anvil::Result<f::SubmissionPage> theirs =
        definitions.submissions(db(), right.value(), std::nullopt, 10);
    ASSERT_TRUE(theirs.ok());
    EXPECT_TRUE(theirs.value().entries.empty());
}

// --- dropping ---------------------------------------------------------------

TEST_F(FormDb, ADropDestroysTheDefinitionAndEverySubmissionTogether) {
    ANVIL_REQUIRE_TRANSACTIONS();

    f::FormSchema schema = plain_schema();
    schema.fields.push_back(declared("f2", FieldType::ImageUuid, true));

    FakeAttachments attachments;
    const Uuid owner = anvil::uuid::generate_v7();
    const Uuid object = anvil::uuid::generate_v4();
    attachments.allow(object, owner);

    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               attachments.hooks()};
    const anvil::Result<Uuid> created =
        definitions.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     attachments.hooks(), pii_keys()};
    f::SubmissionInput with_object{};
    with_object.answers.push_back(text_answer("f1", "hello"));
    with_object.answers.push_back(text_answer("f2", anvil::uuid::to_string(object)));
    with_object.user = owner;
    ASSERT_TRUE(submissions.submit(db(), *form.value(), with_object, now()).ok());
    ASSERT_EQ(attachments.bound_count(object), 1);

    f::SubmissionInput plain{};
    plain.answers.push_back(text_answer("f1", "also hello"));
    ASSERT_TRUE(submissions.submit(db(), *form.value(), plain, now()).ok());

    const anvil::Result<f::DropReport> report = definitions.drop(db(), created.value());
    ASSERT_TRUE(report.ok()) << static_cast<int>(report.code());
    EXPECT_EQ(report.value().submissions_destroyed, 2);
    EXPECT_EQ(report.value().attachments_released, 1);
    // Released inside the same transaction that destroyed the rows holding them.
    // A decrement that committed while the delete aborted is a file the collector
    // takes out from under a live submission.
    EXPECT_EQ(attachments.bound_count(object), 0);

    // No DDL and no orphans: the definition is gone and so are its rows.
    const anvil::Result<std::optional<f::FormDefinition>> gone =
        forms().find_definition(db(), created.value());
    ASSERT_TRUE(gone.ok());
    EXPECT_FALSE(gone.value().has_value());
    EXPECT_EQ(definitions.submissions(db(), created.value(), std::nullopt, 10).code(),
              ErrorCode::NotFound);
    // And the cache cannot still be serving it — invalidation runs BEFORE the
    // reap, which is what makes the reap terminate.
    EXPECT_EQ(definitions.cached(created.value(), now()), nullptr);
}

TEST_F(FormDb, DroppingAFormThatDoesNotExistIsNotFound) {
    ANVIL_REQUIRE_TRANSACTIONS();
    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    EXPECT_EQ(definitions.drop(db(), anvil::uuid::generate_v7()).code(), ErrorCode::NotFound);
}

// --- the export -------------------------------------------------------------

TEST_F(FormDb, TheExportStreamsEveryRowAndNeverMaterialisesThem) {
    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), plain_schema(), anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    constexpr int kRows = 40;
    for (int i = 0; i < kRows; ++i) {
        f::SubmissionInput input{};
        // A hostile cell in every row, so the guard is exercised by the streaming
        // path rather than only by the row builder.
        input.answers.push_back(text_answer("f1", "=cmd|'/c calc'!A1"));
        ASSERT_TRUE(submissions.submit(db(), *form.value(), input, now()).ok());
    }

    std::string collected;
    f::ExportOptions options{};
    options.locale = en();
    options.page_rows = 7;
    const anvil::Status streamed = f::export_submissions(
        db(), forms(), *form.value(), options,
        [&collected](std::string_view chunk) {
            collected.append(chunk);
            return true;
        });
    ASSERT_TRUE(streamed.ok()) << static_cast<int>(streamed.code());

    EXPECT_EQ(collected.substr(0, 3), anvil::http::kUtf8Bom);
    std::size_t rows = 0;
    for (std::size_t at = collected.find("\r\n"); at != std::string::npos;
         at = collected.find("\r\n", at + 2)) {
        ++rows;
    }
    // The header plus one row per submission, whatever the page size was.
    EXPECT_EQ(rows, static_cast<std::size_t>(kRows) + 1);
    EXPECT_EQ(collected.find(",=cmd"), std::string::npos) << "a formula reached a cell unquoted";
    EXPECT_NE(collected.find('\''), std::string::npos);
}

TEST_F(FormDb, AnExportOfAnEmptyFormIsStillAFileWithItsColumns) {
    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), plain_schema(), anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    std::string collected;
    f::ExportOptions options{};
    options.locale = en();
    ASSERT_TRUE(f::export_submissions(db(), forms(), *form.value(), options,
                                      [&collected](std::string_view chunk) {
                                          collected.append(chunk);
                                          return true;
                                      })
                    .ok());
    // Which is what tells an operator the form is empty rather than the export
    // broken.
    EXPECT_NE(collected.find("submission_id"), std::string::npos);
    EXPECT_NE(collected.find("\r\n"), std::string::npos);
}

TEST_F(FormDb, ASinkThatStopsEarlyStopsTheExport) {
    // A form whose rows are large enough that the export flushes MID-STREAM
    // rather than only at the tail: the point is that a refusal stops the paging
    // as well as the writing.
    f::FormSchema schema = plain_schema();
    schema.fields[0] = declared("f1", FieldType::TextLong);

    f::FormService definitions{database(), kDefinitions, kSubmissions, kFieldTypes,
                               f::AttachmentHooks{}};
    const anvil::Result<Uuid> created =
        definitions.create(db(), schema, anvil::uuid::generate_v7());
    ASSERT_TRUE(created.ok());
    const anvil::Result<std::shared_ptr<const f::FormDefinition>> form =
        definitions.definition(db(), created.value(), now());
    ASSERT_TRUE(form.ok());

    f::SubmissionService submissions{database(), kDefinitions, kSubmissions, kFieldTypes,
                                     f::AttachmentHooks{}, pii_keys()};
    constexpr int kRows = 40;
    constexpr std::size_t kCellBytes = 4000;
    for (int i = 0; i < kRows; ++i) {
        f::SubmissionInput input{};
        input.answers.push_back(text_answer("f1", std::string(kCellBytes, 'x')));
        ASSERT_TRUE(submissions.submit(db(), *form.value(), input, now()).ok());
    }

    std::size_t calls = 0;
    std::size_t bytes = 0;
    f::ExportOptions options{};
    options.locale = en();
    options.page_rows = 2;
    // A disk that filled, or a client that disconnected.
    const anvil::Status streamed =
        f::export_submissions(db(), forms(), *form.value(), options,
                              [&calls, &bytes](std::string_view chunk) {
                                  ++calls;
                                  bytes += chunk.size();
                                  return false;
                              });
    EXPECT_TRUE(streamed.ok());
    EXPECT_EQ(calls, 1U);
    // Stopped after the first chunk rather than paging the rest into a buffer
    // nobody was going to take.
    EXPECT_LT(bytes, static_cast<std::size_t>(kRows) * kCellBytes);
}
