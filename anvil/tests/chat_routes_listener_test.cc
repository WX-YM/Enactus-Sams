// The chat routes, served: install_chat_routes behind the real access filter,
// over a real socket, against a live cluster.
//
// The service has a suite of its own (chat_service_db_test.cc), and nothing in
// it can see what this file is for. Each property here lives between the socket
// and the service: that a refusal about a conversation leaves the process as
// the same bytes as a path that does not exist, that a forged write is refused
// before its body is read, that the body a client sends is the body the service
// is handed, and that an attachment leaves as a grant and never as the id it
// names.
//
// Its own binary, because drogon::app() is one listener per process
// (tests/listener_fixture.h) and this one needs a database the unit-labelled
// listener binary does not have.

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "anvil/crypto/base64url.h"
#include "anvil/crypto/ed25519.h"
#include "anvil/crypto/x25519.h"
#include "chat_device_client.h"
#include "anvil/db/codec.h"
#include "anvil/db/mongo_pool.h"
#include "chat_listener_fixture.h"
#include "testapp/audit_actions.h"

namespace anvil {
namespace {

using namespace chatfixture;  // NOLINT(google-build-using-namespace)

// --- the stealth 404 --------------------------------------------------------

TEST_F(ChatRoutesListener, AStrangerIsAnsweredAsIfTheRouteDidNotExist) {
    const Uuid owner = uuid::generate_v4();
    const Uuid stranger = uuid::generate_v4();
    const Uuid c = group(owner);

    const Exchange absent = unmatched(drogon::Get, stranger);
    ASSERT_EQ(absent.response->statusCode(), drogon::k404NotFound);

    // A read, a write, a write with a body, and a message by number: each is one
    // filter, a member row that is not there, and each answers with one set of
    // bytes. A difference in any of them says the conversation exists.
    const std::array<Exchange, 5> refused{{
        call(drogon::Get, conversation_path(c), stranger),
        call(drogon::Get, conversation_path(c, "/messages"), stranger),
        call(drogon::Post, conversation_path(c, "/messages"), stranger,
             R"({"cid":")" + cid() + R"(","body":"hello"})"),
        call(drogon::Get, conversation_path(c, "/messages/1/readers"), stranger),
        call(drogon::Delete, conversation_path(c, "/members/" + uuid::to_string(owner)),
             stranger),
    }};
    for (const Exchange& exchange : refused) {
        ASSERT_EQ(exchange.result, drogon::ReqResult::Ok);
        EXPECT_TRUE(same_response(exchange.response, absent.response))
            << exchange.response->statusCode() << " " << exchange.response->body();
    }
}

TEST_F(ChatRoutesListener, AConversationThatDoesNotExistAndAMalformedIdAnswerAlike) {
    const Uuid user = uuid::generate_v4();
    const Exchange absent = unmatched(drogon::Get, user);
    const Exchange nobody = call(drogon::Get, conversation_path(uuid::generate_v4()), user);
    const Exchange malformed = call(drogon::Get, "/chat/conversations/not-an-id", user);
    const Exchange bad_seq =
        call(drogon::Get, conversation_path(uuid::generate_v4(), "/messages/-1/readers"), user);
    EXPECT_TRUE(same_response(nobody.response, absent.response));
    EXPECT_TRUE(same_response(malformed.response, absent.response));
    EXPECT_TRUE(same_response(bad_seq.response, absent.response));
}

// --- Origin -------------------------------------------------------------------

TEST_F(ChatRoutesListener, AWriteFromAForeignOriginIsRefusedAndStoresNothing) {
    const Uuid owner = uuid::generate_v4();
    const Uuid c = group(owner);
    const std::string body = R"({"cid":")" + cid() + R"(","body":"forged"})";

    const Exchange forged =
        call(drogon::Post, conversation_path(c, "/messages"), owner, body, kForeignOrigin);
    ASSERT_EQ(forged.result, drogon::ReqResult::Ok);
    EXPECT_EQ(forged.response->statusCode(), drogon::k403Forbidden);

    const Exchange history = call(drogon::Get, conversation_path(c, "/messages"), owner);
    ASSERT_EQ(history.response->statusCode(), drogon::k200OK);
    EXPECT_EQ(history.response->body().find("forged"), std::string_view::npos);
}

TEST_F(ChatRoutesListener, AReadNeedsNoOrigin) {
    const Uuid owner = uuid::generate_v4();
    const Uuid c = group(owner);
    const Exchange read = call(drogon::Get, conversation_path(c), owner, {}, {});
    EXPECT_EQ(read.response->statusCode(), drogon::k200OK);
}

// --- sending and reading back ------------------------------------------------

TEST_F(ChatRoutesListener, AMessageSentComesBackInHistoryAndARetryIsTheSameMessage) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const std::string body = R"({"cid":")" + cid() + R"(","body":"hello, team"})";

    const Exchange sent = call(drogon::Post, conversation_path(c, "/messages"), owner, body);
    ASSERT_EQ(sent.response->statusCode(), drogon::k201Created) << sent.response->body();
    const std::optional<std::int64_t> seq = json_int(sent.response->body(), "seq");
    ASSERT_TRUE(seq.has_value());

    // The lost-response retry: the same client id is the same message, a
    // success, and never a second row.
    const Exchange again = call(drogon::Post, conversation_path(c, "/messages"), owner, body);
    ASSERT_EQ(again.response->statusCode(), drogon::k200OK);
    EXPECT_EQ(json_int(again.response->body(), "seq"), seq);

    const Exchange history = call(drogon::Get, conversation_path(c, "/messages"), member);
    ASSERT_EQ(history.response->statusCode(), drogon::k200OK);
    const std::string_view page = history.response->body();
    EXPECT_NE(page.find("hello, team"), std::string_view::npos) << page;
    EXPECT_NE(page.find(R"("seq":)" + std::to_string(*seq)), std::string_view::npos);
    // Once, although it was sent twice.
    EXPECT_EQ(page.find("hello, team"), page.rfind("hello, team"));

    // One account's conversation: no shared cache may hold it, and a page that
    // shows it carries grants a Referer would hand on.
    EXPECT_EQ(history.response->getHeader("cache-control"), "private, no-store");
    EXPECT_EQ(history.response->getHeader("referrer-policy"), "no-referrer");
}

TEST_F(ChatRoutesListener, AnEditIsCaughtUpByTheMutationCursorAndNotTheSeqCursor) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const Exchange sent = call(drogon::Post, conversation_path(c, "/messages"), owner,
                               R"({"cid":")" + cid() + R"(","body":"first"})");
    const std::int64_t seq = *json_int(sent.response->body(), "seq");
    ASSERT_EQ(call(drogon::Patch, conversation_path(c, "/messages/" + std::to_string(seq)), owner,
                   R"({"body":"second"})")
                  .response->statusCode(),
              drogon::k204NoContent);

    // The conversation says its counter moved; the seq cursor at the head
    // reads nothing, because nothing was allocated a seq.
    const Exchange got = call(drogon::Get, conversation_path(c), member);
    EXPECT_NE(got.response->body().find(R"("mutations":1,)"), std::string::npos)
        << got.response->body();
    const Exchange by_seq = call(
        drogon::Get, conversation_path(c, "/messages?after=" + std::to_string(seq)), member);
    EXPECT_EQ(by_seq.response->body().find("second"), std::string::npos);
    const Exchange by_change =
        call(drogon::Get, conversation_path(c, "/messages?changed_after=0"), member);
    ASSERT_EQ(by_change.response->statusCode(), drogon::k200OK);
    EXPECT_NE(by_change.response->body().find(R"("body":"second")"), std::string::npos);
    EXPECT_NE(by_change.response->body().find(R"("mutation":1})"), std::string::npos);
    const Exchange caught_up =
        call(drogon::Get, conversation_path(c, "/messages?changed_after=1"), member);
    EXPECT_EQ(caught_up.response->body(), R"({"messages":[],"reactions":[],"older":null})");
    // A stranger asking is the stealth 404, as for every other read.
    EXPECT_TRUE(same_response(
        call(drogon::Get, conversation_path(c, "/messages?changed_after=0"), uuid::generate_v4())
            .response,
        unmatched(drogon::Get, uuid::generate_v4()).response));
}

TEST_F(ChatRoutesListener, TheReadersRouteSaysWhoHoldsAMessageAndWhoReadIt) {
    const Uuid owner = uuid::generate_v4();
    const Uuid reader = uuid::generate_v4();
    const Uuid holder = uuid::generate_v4();
    const Uuid c = group(owner, {reader, holder});
    const Exchange sent = call(drogon::Post, conversation_path(c, "/messages"), owner,
                               R"({"cid":")" + cid() + R"(","body":"tick"})");
    const std::string seq = std::to_string(*json_int(sent.response->body(), "seq"));
    ASSERT_EQ(call(drogon::Post, conversation_path(c, "/receipts"), reader,
                   R"({"delivered":)" + seq + R"(,"read":)" + seq + "}")
                  .response->statusCode(),
              drogon::k200OK);
    ASSERT_EQ(call(drogon::Post, conversation_path(c, "/receipts"), holder,
                   R"({"delivered":)" + seq + "}")
                  .response->statusCode(),
              drogon::k200OK);
    const Exchange readers =
        call(drogon::Get, conversation_path(c, "/messages/" + seq + "/readers"), owner);
    ASSERT_EQ(readers.response->statusCode(), drogon::k200OK);
    input::BodyArena arena;
    const input::JsonDocument body = input::parse_json(readers.response->body(), arena);
    const auto ids = [&](std::string_view key) {
        std::set<std::string> out;
        for (const input::JsonValue& id : body.root().find(key)->elements()) {
            out.insert(std::string{*id.as_string()});
        }
        return out;
    };
    EXPECT_EQ(ids("read_by"), std::set<std::string>{uuid::to_string(reader)});
    EXPECT_EQ(ids("delivered_to"),
              (std::set<std::string>{uuid::to_string(reader), uuid::to_string(holder)}));
}

TEST_F(ChatRoutesListener, AMuteIsADurationOnTheServersClockAndAnswersTheInstant) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const std::string path = conversation_path(c, "/preferences");
    const auto muted_until = [](const Exchange& answer) -> std::string {
        input::BodyArena arena;
        const input::JsonDocument body = input::parse_json(answer.response->body(), arena);
        const input::JsonValue* at = body.root().find("muted_until");
        return at == nullptr || at->is_null() ? std::string{} : std::string{*at->as_string()};
    };

    const std::int64_t before_ms = now_unix() * 1000;
    const Exchange eight_hours = call(drogon::Patch, path, member, R"({"mute_for_s":28800})");
    ASSERT_EQ(eight_hours.response->statusCode(), drogon::k200OK) << eight_hours.response->body();
    const std::string until = muted_until(eight_hours);
    ASSERT_FALSE(until.empty()) << eight_hours.response->body();
    // The instant is the server's now plus eight hours, which the client never
    // had to compute: the stored row agrees with what was answered.
    auto client = db::MongoPool::instance().acquire();
    const auto row = stack().repository.find_member(*client, c, member);
    ASSERT_TRUE(row.ok() && row.value().has_value() && row.value()->muted_until.has_value());
    const std::int64_t at_ms = row.value()->muted_until->time_since_epoch().count();
    EXPECT_GE(at_ms, before_ms + 28'800'000);
    EXPECT_LE(at_ms, before_ms + 28'800'000 + 60'000);

    const Exchange forever = call(drogon::Patch, path, member, R"({"mute_indefinitely":true})");
    ASSERT_EQ(forever.response->statusCode(), drogon::k200OK);
    EXPECT_EQ(muted_until(forever), "9999-12-31T23:59:59.999Z");

    const Exchange unmuted = call(drogon::Patch, path, member, R"({"mute_for_s":0})");
    ASSERT_EQ(unmuted.response->statusCode(), drogon::k200OK);
    EXPECT_TRUE(muted_until(unmuted).empty());

    // Two ways at once is refused by name, and so is a duration past a year.
    const Exchange both = call(drogon::Patch, path, member,
                               R"({"mute_for_s":60,"mute_indefinitely":true})");
    EXPECT_EQ(both.response->statusCode(), drogon::k400BadRequest);
    EXPECT_NE(both.response->body().find(R"("mute_for_s":"NOT_ALLOWED")"), std::string::npos);
    const Exchange too_long = call(drogon::Patch, path, member, R"({"mute_for_s":31708801})");
    EXPECT_NE(too_long.response->body().find(R"("mute_for_s":"OUT_OF_RANGE")"),
              std::string::npos);
}

TEST_F(ChatRoutesListener, ACreateNeedsAClientIdAndARetryIsTheFirstConversation) {
    const Uuid owner = uuid::generate_v4();
    const Exchange keyless = call(drogon::Post, "/chat/conversations", owner,
                                  R"({"kind":"group","title":"Team"})");
    EXPECT_EQ(keyless.response->statusCode(), drogon::k400BadRequest);
    EXPECT_NE(keyless.response->body().find(R"("cid":"REQUIRED")"), std::string::npos);

    const std::string body = R"({"cid":")" + cid() + R"(","kind":"group","title":"Team"})";
    const Exchange first = call(drogon::Post, "/chat/conversations", owner, body);
    ASSERT_EQ(first.response->statusCode(), drogon::k201Created) << first.response->body();
    const Exchange again = call(drogon::Post, "/chat/conversations", owner, body);
    ASSERT_EQ(again.response->statusCode(), drogon::k200OK) << again.response->body();
    input::BodyArena a;
    input::BodyArena b;
    const input::JsonDocument one = input::parse_json(first.response->body(), a);
    const input::JsonDocument two = input::parse_json(again.response->body(), b);
    EXPECT_EQ(*one.root().find("conversation")->find("id")->as_string(),
              *two.root().find("conversation")->find("id")->as_string());
}

// --- staff review and reports (docs/22-chat.md §9.2) -----------------------------

[[nodiscard]] std::int64_t audit_rows_for(const Uuid& reviewer, const Uuid& conversation) {
    auto client = db::MongoPool::instance().acquire();
    return (*client)[std::string{testfixture::scratch_names().for_collection("audit_log")}]
                    ["audit_log"]
        .count_documents(bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("actor", db::codec::uuid_bin(reviewer)),
            bsoncxx::builder::basic::kvp("sub", db::codec::uuid_bin(conversation)),
            bsoncxx::builder::basic::kvp(
                "act", static_cast<std::int32_t>(testapp::Action::ChatConversationReviewed))));
}

TEST_F(ChatRoutesListener, StaffReadAConversationAuditedAndWithoutMovingAnyWatermark) {
    const Uuid buyer = uuid::generate_v4();
    const Uuid seller = uuid::generate_v4();
    const Uuid staff = uuid::generate_v4();
    const Uuid c = group(buyer, {seller});
    const Exchange sent = call(drogon::Post, conversation_path(c, "/messages"), seller,
                               R"({"cid":")" + cid() + R"(","body":"the item was fine"})");
    const std::int64_t seq = *json_int(sent.response->body(), "seq");
    auto client = db::MongoPool::instance().acquire();
    const auto before = stack().repository.find_member(*client, c, buyer).value();

    const std::string review = "/chat/review/" + uuid::to_string(c);
    const Exchange page = call_as_staff(drogon::Get, review + "/messages", staff);
    ASSERT_EQ(page.response->statusCode(), drogon::k200OK) << page.response->body();
    EXPECT_NE(page.response->body().find("the item was fine"), std::string::npos);
    const Exchange who = call_as_staff(drogon::Get, review, staff);
    ASSERT_EQ(who.response->statusCode(), drogon::k200OK);
    EXPECT_NE(who.response->body().find(uuid::to_string(seller)), std::string::npos);
    // Every read recorded, with the reader and the conversation.
    EXPECT_EQ(audit_rows_for(staff, c), 2);

    // Nobody's watermark moved, and the reader is in nobody's receipts.
    const auto after = stack().repository.find_member(*client, c, buyer).value();
    EXPECT_EQ(after->read, before->read);
    EXPECT_EQ(after->delivered, before->delivered);
    const Exchange readers = call(
        drogon::Get, conversation_path(c, "/messages/" + std::to_string(seq) + "/readers"),
        seller);
    EXPECT_EQ(readers.response->body().find(uuid::to_string(staff)), std::string::npos);

    // Without the permission the route is as absent as one that never existed.
    const Exchange member = call(drogon::Get, review + "/messages", buyer);
    EXPECT_TRUE(same_response(member.response, unmatched(drogon::Get, buyer).response));

    // An encrypted conversation is refused, with why.
    const Exchange secret = call(drogon::Post, "/chat/conversations", buyer,
                                 R"({"cid":")" + cid() +
                                     R"(","kind":"group","title":"S","encrypted":true})");
    input::BodyArena arena;
    const input::JsonDocument made = input::parse_json(secret.response->body(), arena);
    const std::string hidden{*made.root().find("conversation")->find("id")->as_string()};
    const Exchange refused = call_as_staff(drogon::Get, "/chat/review/" + hidden, staff);
    EXPECT_EQ(refused.response->statusCode(), drogon::k403Forbidden);
    EXPECT_NE(refused.response->body().find(R"("reason":"chat.not_reviewable")"),
              std::string::npos);
}

TEST_F(ChatRoutesListener, AMemberReportsARangeAndStaffListIt) {
    const Uuid buyer = uuid::generate_v4();
    const Uuid seller = uuid::generate_v4();
    const Uuid c = group(buyer, {seller});
    std::int64_t last = 0;
    for (int i = 0; i < 3; ++i) {
        last = *json_int(call(drogon::Post, conversation_path(c, "/messages"), seller,
                              R"({"cid":")" + cid() + R"(","body":"pay outside the app"})")
                             .response->body(),
                         "seq");
    }
    const std::string body = R"({"from":)" + std::to_string(last - 2) + R"(,"to":)" +
                             std::to_string(last) + R"(,"note":"asked me to pay elsewhere"})";
    const Exchange filed = call(drogon::Post, conversation_path(c, "/reports"), buyer, body);
    ASSERT_EQ(filed.response->statusCode(), drogon::k201Created) << filed.response->body();
    input::BodyArena arena;
    const input::JsonDocument answer = input::parse_json(filed.response->body(), arena);
    const std::string id{*answer.root().find("id")->as_string()};
    // The same range again is the same report.
    const Exchange again = call(drogon::Post, conversation_path(c, "/reports"), buyer, body);
    EXPECT_EQ(again.response->statusCode(), drogon::k200OK);
    EXPECT_NE(again.response->body().find(id), std::string::npos);
    // Past the head, or by somebody not in it, it is not a report.
    EXPECT_EQ(call(drogon::Post, conversation_path(c, "/reports"), buyer,
                   R"({"from":1,"to":)" + std::to_string(last + 5) + "}")
                  .response->statusCode(),
              drogon::k400BadRequest);
    EXPECT_EQ(call(drogon::Post, conversation_path(c, "/reports"), uuid::generate_v4(), body)
                  .response->statusCode(),
              drogon::k404NotFound);

    const Exchange listed = call_as_staff(drogon::Get, "/chat/reports", uuid::generate_v4());
    ASSERT_EQ(listed.response->statusCode(), drogon::k200OK);
    EXPECT_NE(listed.response->body().find(id), std::string::npos);
    EXPECT_NE(listed.response->body().find("asked me to pay elsewhere"), std::string::npos);
    EXPECT_EQ(call(drogon::Get, "/chat/reports", buyer).response->statusCode(),
              drogon::k404NotFound);
}

TEST_F(ChatRoutesListener, ABodyWithoutAClientIdNamesTheFieldAndNotTheValue) {
    const Uuid owner = uuid::generate_v4();
    const Uuid c = group(owner);
    const Exchange refused = call(drogon::Post, conversation_path(c, "/messages"), owner,
                                  R"({"cid":"not-sixteen-bytes","body":"x"})");
    ASSERT_EQ(refused.response->statusCode(), drogon::k400BadRequest);
    const std::string_view body = refused.response->body();
    EXPECT_NE(body.find(R"("cid")"), std::string_view::npos) << body;
    EXPECT_EQ(body.find("not-sixteen-bytes"), std::string_view::npos) << body;
}

TEST_F(ChatRoutesListener, AnEditKeepsItsMentionsAndRefusesAMalformedList) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const Exchange sent = call(drogon::Post, conversation_path(c, "/messages"), owner,
                               R"({"cid":")" + cid() + R"(","body":"hi all"})");
    const std::int64_t seq = *json_int(sent.response->body(), "seq");
    const std::string path = conversation_path(c, "/messages/" + std::to_string(seq));

    const Exchange malformed =
        call(drogon::Patch, path, owner, R"({"body":"hi you","mentions":"everyone"})");
    EXPECT_EQ(malformed.response->statusCode(), drogon::k400BadRequest);

    // An edit carries no client id, and its mentions still reach the service:
    // the mention below is checked and stored, not dropped for want of a cid.
    const std::string edit = R"({"body":"hi you","mentions":[{"user":")" +
                             uuid::to_string(member) + R"(","offset":3,"length":3}]})";
    const Exchange edited = call(drogon::Patch, path, owner, edit);
    ASSERT_EQ(edited.response->statusCode(), drogon::k204NoContent)
        << edited.response->body();
    const Exchange history = call(drogon::Get, conversation_path(c, "/messages"), member);
    EXPECT_NE(history.response->body().find(R"("offset":3)"), std::string_view::npos)
        << history.response->body();
}

// --- attachments --------------------------------------------------------------

TEST_F(ChatRoutesListener, AnAttachmentLeavesAsAGrantAndNeverAsTheIdItNames) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const auto [object, handle] = uploaded(owner);

    const std::string body = R"({"cid":")" + cid() + R"(","body":"the plan","attachments":[{)" +
                             R"("handle":")" + handle + R"(","name":"plan.pdf"}]})";
    const Exchange sent = call(drogon::Post, conversation_path(c, "/messages"), owner, body);
    ASSERT_EQ(sent.response->statusCode(), drogon::k201Created) << sent.response->body();

    const Exchange history = call(drogon::Get, conversation_path(c, "/messages"), member);
    ASSERT_EQ(history.response->statusCode(), drogon::k200OK);
    const std::string page{history.response->body()};

    // The grant opens to the object, under the keys the service minted with.
    constexpr std::string_view kGrantKey = R"("grant":")";
    const std::size_t at = page.find(kGrantKey);
    ASSERT_NE(at, std::string::npos) << page;
    const std::string grant = page.substr(at + kGrantKey.size(), media::kGrantChars);
    const std::optional<media::MediaGrant> opened =
        media::open_grant(grant_keys(), grant, now_unix());
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(opened->id(), object);
    EXPECT_NE(page.find(R"("name":"plan.pdf")"), std::string::npos);

    // And the id itself, in every encoding a careless writer would reach for, is
    // nowhere in what the member was sent (docs/22 §6.2).
    std::string hex;
    for (const std::uint8_t byte : object) {
        constexpr std::string_view kDigits = "0123456789abcdef";
        hex += kDigits[byte >> 4U];
        hex += kDigits[byte & 0xFU];
    }
    for (const std::string& encoding :
         {uuid::to_string(object), hex, crypto::base64url_encode(object)}) {
        EXPECT_EQ(page.find(encoding), std::string::npos) << encoding;
        EXPECT_EQ(sent.response->body().find(encoding), std::string_view::npos) << encoding;
    }
}

// --- the grant route ----------------------------------------------------------

// The grant out of the first attachment of a history page.
[[nodiscard]] std::string first_grant(std::string_view page) {
    constexpr std::string_view kGrantKey = R"("grant":")";
    const std::size_t at = page.find(kGrantKey);
    if (at == std::string_view::npos) { return {}; }
    return std::string{page.substr(at + kGrantKey.size(), media::kGrantChars)};
}

TEST_F(ChatRoutesListener, AGrantServesItsObjectUntilNothingHoldsIt) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const auto [object, handle] = uploaded(owner);
    (void)object;
    const Exchange sent = call(drogon::Post, conversation_path(c, "/messages"), owner,
                               R"({"cid":")" + cid() + R"(","attachments":[{"handle":")" +
                                   handle + R"(","name":"plan.pdf"}]})");
    ASSERT_EQ(sent.response->statusCode(), drogon::k201Created) << sent.response->body();
    const std::int64_t seq = *json_int(sent.response->body(), "seq");
    const Exchange history = call(drogon::Get, conversation_path(c, "/messages"), member);
    const std::string grant = first_grant(history.response->body());
    ASSERT_EQ(grant.size(), media::kGrantChars) << history.response->body();

    // No cookie: the media origin never receives one, and the grant is the
    // whole of the authority.
    const Exchange served = testfixture::get("/m/" + grant + "/full", "");
    ASSERT_EQ(served.response->statusCode(), drogon::k200OK) << served.response->body();
    EXPECT_NE(served.response->getHeader("x-accel-redirect").find("/protected_storage/"),
              std::string::npos);
    EXPECT_EQ(served.response->getHeader("content-type"), "application/pdf");
    // Cached for no longer than the grant opens: one to two buckets.
    const std::string cache = served.response->getHeader("cache-control");
    ASSERT_EQ(cache.rfind("private, max-age=", 0), 0U) << cache;
    const long max_age = std::stol(cache.substr(std::string_view{"private, max-age="}.size()));
    EXPECT_GT(max_age, 0);
    EXPECT_LE(max_age, 2 * media::kGrantBucketSeconds);

    // Delete-for-everyone releases the last reference, and the grant the
    // member already holds stops at the next request rather than at its expiry.
    const Exchange revoked =
        call(drogon::Delete, conversation_path(c, "/messages/" + std::to_string(seq)), owner);
    ASSERT_EQ(revoked.response->statusCode(), drogon::k204NoContent);
    const Exchange gone = testfixture::get("/m/" + grant + "/full", "");
    EXPECT_TRUE(same_response(gone.response, testfixture::get("/no-such-path", "").response));
}

TEST_F(ChatRoutesListener, AForgedGrantAndAMadeUpRoleAnswerAsNothing) {
    const Exchange absent = testfixture::get("/no-such-path", "");
    const std::string forged(media::kGrantChars, 'A');
    const std::string minted =
        media::mint_grant(grant_keys(), testapp::kChat, uuid::generate_v4(), now_unix());
    // A forgery, a real grant for an object that was never stored, and a real
    // grant under a role that does not exist.
    for (const std::string& path :
         {"/m/" + forged + "/full", "/m/" + minted + "/full", "/m/" + minted + "/master"}) {
        const Exchange refused = testfixture::get(path, "");
        ASSERT_EQ(refused.result, drogon::ReqResult::Ok);
        EXPECT_TRUE(same_response(refused.response, absent.response)) << path;
    }
}

TEST_F(ChatRoutesListener, AHandleIsSpentOnlyByItsUploader) {
    const Uuid owner = uuid::generate_v4();
    const Uuid member = uuid::generate_v4();
    const Uuid c = group(owner, {member});
    const auto [object, handle] = uploaded(owner);
    (void)object;

    const std::string body = R"({"cid":")" + cid() + R"(","attachments":[{"handle":")" +
                             handle + R"("}]})";
    const Exchange stolen = call(drogon::Post, conversation_path(c, "/messages"), member, body);
    EXPECT_EQ(stolen.response->statusCode(), drogon::k400BadRequest) << stolen.response->body();
}

// --- the encrypted send (docs/22-chat.md §7.6) ---------------------------------

TEST_F(ChatRoutesListener, AStaleEncryptedSendIsAnsweredWithTheSetItWasStaleAgainst) {
    const Uuid alice = uuid::generate_v4();
    const Uuid bob = uuid::generate_v4();
    auto client = db::MongoPool::instance().acquire();
    const db::TimeMs now = db::now_ms();
    const chattest::Client phone = chattest::make_client();
    const chattest::Client bobs = chattest::make_client();
    ASSERT_TRUE(stack().devices.register_first_device(*client, alice, phone.device, now, now).ok());
    ASSERT_TRUE(stack().devices.register_first_device(*client, bob, bobs.device, now, now).ok());
    ASSERT_TRUE(stack().service.propagate_devices(*client, alice).ok());
    ASSERT_TRUE(stack().service.propagate_devices(*client, bob).ok());

    const Exchange opened = call(drogon::Put, "/chat/direct/" + uuid::to_string(bob), alice,
                                 R"({"kind":"direct","encrypted":true})");
    ASSERT_EQ(opened.response->statusCode(), drogon::k200OK) << opened.response->body();
    input::BodyArena arena;
    const input::JsonDocument state = input::parse_json(opened.response->body(), arena);
    const input::JsonValue* conversation = state.root().find("conversation");
    const Uuid c = *uuid::parse(*conversation->find("id")->as_string());
    const std::int64_t dsv = *conversation->find("dsv")->as_int64();

    const std::string to_bob = crypto::base64url_encode(std::array<std::uint8_t, 4>{1, 2, 3, 4});
    const auto body = [&](std::int64_t version) {
        return R"({"cid":")" + cid() + R"(","device":")" + uuid::to_string(phone.device.id) +
               R"(","dsv":)" + std::to_string(version) + R"(,"devices":[{"device":")" +
               uuid::to_string(bobs.device.id) + R"(","ciphertext":")" + to_bob + R"("}]})";
    };

    // A version that is not the conversation's: the 409 carries the set, so
    // the client re-encrypts without asking again.
    const Exchange stale = call(drogon::Post, conversation_path(c, "/messages"), alice,
                                body(dsv - 1));
    ASSERT_EQ(stale.response->statusCode(), drogon::k409Conflict) << stale.response->body();
    input::BodyArena stale_arena;
    const input::JsonDocument refused = input::parse_json(stale.response->body(), stale_arena);
    ASSERT_TRUE(refused.ok());
    EXPECT_EQ(refused.root().find("error")->find("code")->as_string(), "CONFLICT");
    EXPECT_EQ(refused.root().find("reason")->as_string(), "chat.devices_stale");
    const input::JsonValue* page = refused.root().find("devices");
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->find("dsv")->as_int64(), dsv);
    EXPECT_NE(stale.response->body().find(uuid::to_string(bobs.device.id)), std::string::npos);
    EXPECT_NE(stale.response->body().find(crypto::base64url_encode(bobs.device.keys.signing)),
              std::string::npos);

    // At the version the 409 named, the same send goes through, and history
    // serves the message as ciphertext from the device that sent it.
    const Exchange sent = call(drogon::Post, conversation_path(c, "/messages"), alice,
                               body(*page->find("dsv")->as_int64()));
    ASSERT_EQ(sent.response->statusCode(), drogon::k201Created) << sent.response->body();
    const Exchange history = call(drogon::Get, conversation_path(c, "/messages"), bob);
    ASSERT_EQ(history.response->statusCode(), drogon::k200OK);
    EXPECT_NE(history.response->body().find(R"("kind":"encrypted")"), std::string::npos);
    EXPECT_NE(history.response->body().find(R"("device":")" + uuid::to_string(phone.device.id)),
              std::string::npos);

    // Plaintext into it is refused, naming the field and not the value.
    const Exchange plain = call(drogon::Post, conversation_path(c, "/messages"), alice,
                                R"({"cid":")" + cid() + R"(","body":"secret words"})");
    EXPECT_EQ(plain.response->statusCode(), drogon::k400BadRequest);
    EXPECT_EQ(plain.response->body().find("secret words"), std::string::npos);
}

// --- devices, keys and the queue (docs/22-chat.md §7.3–§7.6) ------------------

// A request as `user` on one particular session, which the device routes care
// about: a first device is admitted only on a session signed in recently.
[[nodiscard]] Exchange call_on(drogon::HttpMethod method, std::string_view path, const Uuid& user,
                               const Uuid& session, std::string_view body = {}) {
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(std::string{path});
    req->addHeader("Cookie", cookie_for(user, session));
    req->addHeader("Origin", std::string{kAllowedOrigin});
    if (!body.empty()) {
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->setBody(std::string{body});
    }
    return testfixture::send(req);
}

// A device's bundle as its client uploads it.
[[nodiscard]] std::string bundle_json(const chattest::Client& client) {
    const chat::DeviceKeys& keys = client.device.keys;
    const auto b64 = [](std::span<const std::uint8_t> bytes) {
        return crypto::base64url_encode(bytes);
    };
    return R"("device_id":")" + uuid::to_string(client.device.id) + R"(","suite":1,)" +
           R"("agreement_key":")" + b64(keys.agreement) + R"(","signing_key":")" +
           b64(keys.signing) + R"(","signed_prekey":")" + b64(keys.signed_prekey) +
           R"(","signed_prekey_signature":")" + b64(keys.signed_prekey_signature) +
           R"(","last_resort_key":")" + b64(keys.last_resort) +
           R"(","last_resort_signature":")" + b64(keys.last_resort_signature) + '"';
}

// The first device of `user`, through the route on a session just signed in.
[[nodiscard]] chattest::Client registered_over_http(const Uuid& user) {
    chattest::Client client = chattest::make_client();
    const Exchange made = call_on(drogon::Put, "/chat/devices", user, uuid::generate_v7(),
                                  "{" + bundle_json(client) + "}");
    EXPECT_EQ(made.response->statusCode(), drogon::k204NoContent) << made.response->body();
    return client;
}

TEST_F(ChatRoutesListener, ADeviceIsRegisteredLinkedListedAndUnlinkedOverTheRoutes) {
    const Uuid alice = uuid::generate_v4();
    const chattest::Client phone = registered_over_http(alice);

    // A second first device is refused: the next one needs a signature.
    const chattest::Client stray = chattest::make_client();
    const Exchange again = call_on(drogon::Put, "/chat/devices", alice, uuid::generate_v7(),
                                   "{" + bundle_json(stray) + "}");
    EXPECT_EQ(again.response->statusCode(), drogon::k409Conflict);

    // The laptop, admitted by the phone's signature over its keys.
    const chattest::Client laptop = chattest::make_client();
    const auto at = static_cast<std::uint64_t>(now_unix());
    const crypto::Ed25519Signature signature = crypto::ed25519_sign(
        phone.seed, chat::link_message(alice, laptop.device.id, laptop.device.keys.agreement,
                                       laptop.device.keys.signing, at));
    const std::string link = "{" + bundle_json(laptop) + R"(,"approver":")" +
                             uuid::to_string(phone.device.id) + R"(","timestamp":)" +
                             std::to_string(at) + R"(,"link_signature":")" +
                             crypto::base64url_encode(signature) + R"("})";
    const Exchange linked = call(drogon::Post, "/chat/devices/link", alice, link);
    EXPECT_EQ(linked.response->statusCode(), drogon::k204NoContent) << linked.response->body();

    const Exchange mine = call(drogon::Get, "/chat/devices", alice);
    ASSERT_EQ(mine.response->statusCode(), drogon::k200OK);
    const std::string_view listed = mine.response->body();
    EXPECT_NE(listed.find(uuid::to_string(phone.device.id)), std::string_view::npos);
    EXPECT_NE(listed.find(uuid::to_string(laptop.device.id)), std::string_view::npos);
    EXPECT_NE(listed.find(R"("last_seen":)"), std::string_view::npos);
    // The approver and its signature, so the owner's client can verify the
    // chain; never the session that registered a device.
    EXPECT_NE(listed.find(crypto::base64url_encode(signature)), std::string_view::npos);
    EXPECT_EQ(listed.find(R"("sid")"), std::string_view::npos);
    EXPECT_EQ(listed.find(R"("session")"), std::string_view::npos);

    const std::string path = "/chat/devices/" + uuid::to_string(laptop.device.id);
    EXPECT_EQ(call(drogon::Delete, path, alice).response->statusCode(), drogon::k204NoContent);
    EXPECT_EQ(call(drogon::Delete, path, alice).response->statusCode(), drogon::k204NoContent);
    EXPECT_EQ(call(drogon::Get, "/chat/devices", alice).response->body().find(
                  uuid::to_string(laptop.device.id)),
              std::string::npos);
}

TEST_F(ChatRoutesListener, TheListingMarksTheDeviceTheCallingSessionRegistered) {
    const Uuid alice = uuid::generate_v4();
    const Uuid phone_session = uuid::generate_v7();
    const chattest::Client phone = chattest::make_client();
    ASSERT_EQ(call_on(drogon::Put, "/chat/devices", alice, phone_session,
                      "{" + bundle_json(phone) + "}")
                  .response->statusCode(),
              drogon::k204NoContent);
    const std::string device = R"("id":")" + uuid::to_string(phone.device.id) + '"';

    // The session that registered it is told it is its own...
    const Exchange from_phone = call_on(drogon::Get, "/chat/devices", alice, phone_session);
    ASSERT_EQ(from_phone.response->statusCode(), drogon::k200OK);
    EXPECT_NE(from_phone.response->body().find(R"("low":false,"mine":true})"), std::string::npos)
        << from_phone.response->body();
    // ...and every other session of the account is told it is not, which is
    // all it learns about the session: never its id.
    const Exchange elsewhere = call(drogon::Get, "/chat/devices", alice);
    EXPECT_NE(elsewhere.response->body().find(R"("mine":false})"), std::string::npos);
    EXPECT_EQ(elsewhere.response->body().find(R"("mine":true)"), std::string::npos);
    EXPECT_EQ(elsewhere.response->body().find(uuid::to_string(phone_session)), std::string::npos);
    EXPECT_NE(elsewhere.response->body().find(device), std::string::npos);
}

TEST_F(ChatRoutesListener, ADeviceRotatesItsSignedKeysOverTheRoute) {
    const Uuid alice = uuid::generate_v4();
    const chattest::Client phone = registered_over_http(alice);
    const std::string path = "/chat/devices/" + uuid::to_string(phone.device.id) + "/keys";
    const crypto::X25519PublicKey spk = crypto::x25519_generate_keypair().public_key;
    const crypto::Ed25519Signature good = crypto::ed25519_sign(
        phone.seed, chat::prekey_message(chat::kSignedPrekeyDomain, phone.device.id, spk));
    const std::string body = R"({"signed_prekey":")" + crypto::base64url_encode(spk) +
                             R"(","signed_prekey_signature":")" +
                             crypto::base64url_encode(good) + R"("})";
    EXPECT_EQ(call(drogon::Put, path, alice, body).response->statusCode(), drogon::k204NoContent);
    EXPECT_NE(call(drogon::Get, "/chat/devices", alice)
                  .response->body()
                  .find(crypto::base64url_encode(spk)),
              std::string::npos);

    // Signed over another key: refused by name, nothing written.
    const crypto::X25519PublicKey other = crypto::x25519_generate_keypair().public_key;
    const Exchange forged =
        call(drogon::Put, path, alice,
             R"({"signed_prekey":")" + crypto::base64url_encode(other) +
                 R"(","signed_prekey_signature":")" + crypto::base64url_encode(good) + R"("})");
    EXPECT_EQ(forged.response->statusCode(), drogon::k400BadRequest);
    EXPECT_NE(forged.response->body().find(R"("signed_prekey_signature":)"), std::string::npos);
    // Somebody else's device is not theirs to rotate.
    EXPECT_EQ(call(drogon::Put, path, uuid::generate_v4(), body).response->statusCode(),
              drogon::k403Forbidden);
}

TEST_F(ChatRoutesListener, ALinkApprovalTravelsBackThroughTheRelayOnce) {
    const Uuid alice = uuid::generate_v4();
    const chattest::Client phone = registered_over_http(alice);
    const chattest::Client laptop = chattest::make_client();
    const Uuid laptop_session = uuid::generate_v7();
    const Uuid phone_session = uuid::generate_v7();
    const auto b64 = [](std::span<const std::uint8_t> bytes) {
        return crypto::base64url_encode(bytes);
    };

    // The laptop leaves its keys and shows the phone the token.
    const Exchange asked = call_on(
        drogon::Post, "/chat/link-requests", alice, laptop_session,
        R"({"device_id":")" + uuid::to_string(laptop.device.id) + R"(","agreement_key":")" +
            b64(laptop.device.keys.agreement) + R"(","signing_key":")" +
            b64(laptop.device.keys.signing) + R"("})");
    ASSERT_EQ(asked.response->statusCode(), drogon::k201Created) << asked.response->body();
    input::BodyArena asked_arena;
    const input::JsonDocument ticket = input::parse_json(asked.response->body(), asked_arena);
    const std::string token{*ticket.root().find("token")->as_string()};
    EXPECT_EQ(ticket.root().find("expires_in_s")->as_int64(), 600);
    const std::string by_token = R"({"token":")" + token + R"("})";

    // Pending, to the session that asked; nothing at all to anybody else.
    const Exchange pending =
        call_on(drogon::Post, "/chat/link-requests/collect", alice, laptop_session, by_token);
    EXPECT_EQ(pending.response->body(), R"({"approval":null})");
    const Exchange nothing = unmatched(drogon::Post, alice);
    EXPECT_TRUE(same_response(
        call(drogon::Post, "/chat/link-requests/read", uuid::generate_v4(), by_token).response,
        nothing.response));
    EXPECT_TRUE(same_response(
        call_on(drogon::Post, "/chat/link-requests/collect", alice, phone_session, by_token)
            .response,
        nothing.response));

    // The phone reads what it signs, and leaves its signature.
    const Exchange read =
        call_on(drogon::Post, "/chat/link-requests/read", alice, phone_session, by_token);
    ASSERT_EQ(read.response->statusCode(), drogon::k200OK);
    EXPECT_NE(read.response->body().find(b64(laptop.device.keys.signing)), std::string::npos);
    const auto at = static_cast<std::uint64_t>(now_unix());
    const crypto::Ed25519Signature signature = crypto::ed25519_sign(
        phone.seed, chat::link_message(alice, laptop.device.id, laptop.device.keys.agreement,
                                       laptop.device.keys.signing, at));
    const std::string approval_tail = R"(","approver":")" + uuid::to_string(phone.device.id) +
                                      R"(","timestamp":)" + std::to_string(at) +
                                      R"(,"link_signature":")";
    // A signature over anything else is refused to the approver, now.
    const Exchange forged = call_on(drogon::Post, "/chat/link-requests/approve", alice,
                                    phone_session,
                                    R"({"token":")" + token + approval_tail +
                                        b64(crypto::ed25519_sign(phone.seed, std::array<std::uint8_t, 1>{1})) +
                                        R"("})");
    EXPECT_EQ(forged.response->statusCode(), drogon::k403Forbidden);
    const std::string approve = R"({"token":")" + token + approval_tail + b64(signature) + R"("})";
    EXPECT_EQ(call_on(drogon::Post, "/chat/link-requests/approve", alice, phone_session, approve)
                  .response->statusCode(),
              drogon::k204NoContent);
    EXPECT_EQ(call_on(drogon::Post, "/chat/link-requests/approve", alice, phone_session, approve)
                  .response->statusCode(),
              drogon::k409Conflict);

    // Collected once, and the approval it carries links the laptop.
    const Exchange collected =
        call_on(drogon::Post, "/chat/link-requests/collect", alice, laptop_session, by_token);
    ASSERT_EQ(collected.response->statusCode(), drogon::k200OK);
    EXPECT_NE(collected.response->body().find(b64(signature)), std::string::npos);
    EXPECT_EQ(call_on(drogon::Post, "/chat/link-requests/collect", alice, laptop_session, by_token)
                  .response->statusCode(),
              drogon::k404NotFound);
    const Exchange linked = call_on(drogon::Post, "/chat/devices/link", alice, laptop_session,
                                    "{" + bundle_json(laptop) + R"(,"approver":")" +
                                        uuid::to_string(phone.device.id) + R"(","timestamp":)" +
                                        std::to_string(at) + R"(,"link_signature":")" +
                                        b64(signature) + R"("})");
    EXPECT_EQ(linked.response->statusCode(), drogon::k204NoContent) << linked.response->body();
}

TEST_F(ChatRoutesListener, AFirstDeviceNeedsARecentSignInNotJustASession) {
    const Uuid alice = uuid::generate_v4();
    // A session signed in ten minutes ago is a cookie, and a cookie is what a
    // thief has.
    const Uuid old_session = uuid::v7_boundary(now_unix() * 1000 - 10 * 60 * 1000);
    const chattest::Client client = chattest::make_client();
    const Exchange refused = call_on(drogon::Put, "/chat/devices", alice, old_session,
                                     "{" + bundle_json(client) + "}");
    // 428 and never 401. A client answers a 401 by refreshing its access token
    // and replaying; the replay is refused again, and a second 401 after a
    // good refresh signs the person out of every tab. The body names what to
    // do instead: sign in again with a primary credential, and retry.
    EXPECT_EQ(refused.response->statusCode(), drogon::k428PreconditionRequired);
    const std::string_view body = refused.response->body();
    EXPECT_NE(body.find(R"("code":"CAPABILITY_REQUIRED")"), std::string_view::npos) << body;
    EXPECT_NE(body.find(R"(},"reason":"chat.fresh_authentication","field":"authenticated_at"})"),
              std::string_view::npos)
        << body;
    EXPECT_EQ(body.find("UNAUTHENTICATED"), std::string_view::npos);
    EXPECT_TRUE(refused.response->getHeader("www-authenticate").empty());
    EXPECT_EQ(call(drogon::Get, "/chat/devices", alice).response->body().find(
                  uuid::to_string(client.device.id)),
              std::string::npos);
}

TEST_F(ChatRoutesListener, KeysAreClaimedOnlyBetweenMembersAndTheTargetsBudgetHolds) {
    const Uuid alice = uuid::generate_v4();
    const Uuid bob = uuid::generate_v4();
    const Uuid stranger = uuid::generate_v4();
    (void)registered_over_http(alice);
    const chattest::Client bobs = registered_over_http(bob);
    (void)registered_over_http(stranger);

    std::string keys = R"({"device":")" + uuid::to_string(bobs.device.id) + R"(","prekeys":[)";
    for (int i = 0; i < 5; ++i) {
        if (i != 0) { keys += ','; }
        keys += R"({"kid":)" + std::to_string(i) + R"(,"prekey":")" +
                crypto::base64url_encode(crypto::x25519_generate_keypair().public_key) + R"("})";
    }
    keys += "]}";
    EXPECT_EQ(call(drogon::Post, "/chat/keys", bob, keys).response->statusCode(),
              drogon::k204NoContent);

    const Exchange opened = call(drogon::Put, "/chat/direct/" + uuid::to_string(bob), alice,
                                 R"({"kind":"direct","encrypted":true})");
    input::BodyArena arena;
    const input::JsonDocument state = input::parse_json(opened.response->body(), arena);
    const Uuid c = *uuid::parse(*state.root().find("conversation")->find("id")->as_string());
    const std::string claim_path = conversation_path(c, "/keys/claim");
    const std::string claim_bob = R"({"users":[")" + uuid::to_string(bob) + R"("]})";

    // A stranger is answered as if nothing were there, and spends nothing of
    // bob's budget doing it.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(call(drogon::Post, claim_path, stranger, claim_bob).response->statusCode(),
                  drogon::k404NotFound);
    }

    // Alice gets a bundle with a one-time key that is then gone from the pool.
    std::set<std::string> handed;
    for (int i = 0; i < 3; ++i) {
        const Exchange claimed = call(drogon::Post, claim_path, alice, claim_bob);
        ASSERT_EQ(claimed.response->statusCode(), drogon::k200OK) << claimed.response->body();
        input::BodyArena bundle_arena;
        const input::JsonDocument bundles = input::parse_json(claimed.response->body(), bundle_arena);
        const input::JsonValue& mine = bundles.root().find("claims")->elements()[0];
        EXPECT_EQ(mine.find("user")->as_string(), uuid::to_string(bob));
        EXPECT_TRUE(mine.find("refused")->is_null());
        const input::JsonValue& first = mine.find("bundles")->elements()[0];
        EXPECT_EQ(first.find("device")->as_string(), uuid::to_string(bobs.device.id));
        handed.insert(std::string{*first.find("one_time")->find("key")->as_string()});
    }
    EXPECT_EQ(handed.size(), 3U);

    // Three claims a minute against one account, in this stack: the fourth is
    // refused for that account with when to come back, and no key leaves the
    // pool for it. In the same request alice's own other devices are claimed,
    // and a stranger named in it is refused alone, spending nothing.
    const Exchange throttled = call(
        drogon::Post, claim_path, alice,
        R"({"users":[")" + uuid::to_string(bob) + R"(",")" + uuid::to_string(alice) + R"(",")" +
            uuid::to_string(stranger) + R"(",")" + uuid::to_string(bob) + R"("]})");
    ASSERT_EQ(throttled.response->statusCode(), drogon::k200OK) << throttled.response->body();
    input::BodyArena throttled_arena;
    const input::JsonDocument answer = input::parse_json(throttled.response->body(), throttled_arena);
    const auto& claims = answer.root().find("claims")->elements();
    ASSERT_EQ(claims.size(), 3U) << "in the order asked, a duplicate once";
    EXPECT_EQ(claims[0].find("refused")->as_string(), "RATE_LIMITED");
    EXPECT_TRUE(claims[0].find("bundles")->is_null());
    EXPECT_GT(claims[0].find("retry_after")->as_int64().value_or(0), 0);
    EXPECT_EQ(claims[1].find("user")->as_string(), uuid::to_string(alice));
    EXPECT_TRUE(claims[1].find("refused")->is_null());
    EXPECT_EQ(claims[2].find("refused")->as_string(), "NOT_FOUND");
    auto client = db::MongoPool::instance().acquire();
    EXPECT_EQ(stack().prekeys.remaining(*client, bob, bobs.device.id).value(), 2);

    // The single form is gone: one shape, and a batch past its bound is refused.
    EXPECT_EQ(call(drogon::Post, claim_path, alice,
                   R"({"user":")" + uuid::to_string(bob) + R"("})")
                  .response->statusCode(),
              drogon::k400BadRequest);
    std::string many = R"({"users":[)";
    for (std::size_t i = 0; i <= chat::kMaxClaimBatch; ++i) {
        if (i != 0) { many += ','; }
        many += '"' + uuid::to_string(uuid::generate_v4()) + '"';
    }
    many += "]}";
    const Exchange too_many = call(drogon::Post, claim_path, alice, many);
    EXPECT_NE(too_many.response->body().find(R"("users":"TOO_LONG")"), std::string::npos);
}

TEST_F(ChatRoutesListener, TheDeviceQueueIsReadAndAcknowledgedByItsOwnAccountOnly) {
    const Uuid alice = uuid::generate_v4();
    const Uuid bob = uuid::generate_v4();
    const chattest::Client phone = registered_over_http(alice);
    const chattest::Client bobs = registered_over_http(bob);
    const Exchange opened = call(drogon::Put, "/chat/direct/" + uuid::to_string(bob), alice,
                                 R"({"kind":"direct","encrypted":true})");
    input::BodyArena arena;
    const input::JsonDocument state = input::parse_json(opened.response->body(), arena);
    const input::JsonValue* conversation = state.root().find("conversation");
    const Uuid c = *uuid::parse(*conversation->find("id")->as_string());

    // The listing of the conversation's devices names the version to send at.
    const Exchange devices = call(drogon::Get, conversation_path(c, "/devices"), alice);
    ASSERT_EQ(devices.response->statusCode(), drogon::k200OK);
    const std::optional<std::int64_t> dsv = json_int(devices.response->body(), "dsv");
    ASSERT_TRUE(dsv.has_value());
    EXPECT_NE(devices.response->body().find(uuid::to_string(bobs.device.id)), std::string::npos);

    const std::string ciphertext = crypto::base64url_encode(std::array<std::uint8_t, 3>{7, 8, 9});
    const Exchange sent = call(
        drogon::Post, conversation_path(c, "/messages"), alice,
        R"({"cid":")" + cid() + R"(","device":")" + uuid::to_string(phone.device.id) +
            R"(","dsv":)" + std::to_string(*dsv) + R"(,"devices":[{"device":")" +
            uuid::to_string(bobs.device.id) + R"(","ciphertext":")" + ciphertext + R"("}]})");
    ASSERT_EQ(sent.response->statusCode(), drogon::k201Created) << sent.response->body();

    const std::string queue = "/chat/device-queue?device=" + uuid::to_string(bobs.device.id);
    // Somebody else's device is not the caller's to read or to empty.
    EXPECT_EQ(call(drogon::Get, queue, alice).response->statusCode(), drogon::k403Forbidden);
    EXPECT_EQ(call(drogon::Get, "/chat/device-queue", bob).response->statusCode(),
              drogon::k400BadRequest);

    const Exchange read = call(drogon::Get, queue, bob);
    ASSERT_EQ(read.response->statusCode(), drogon::k200OK);
    EXPECT_NE(read.response->body().find(ciphertext), std::string::npos);
    input::BodyArena rows_arena;
    const input::JsonDocument rows = input::parse_json(read.response->body(), rows_arena);
    const std::string_view id = *rows.root().find("rows")->elements()[0].find("id")->as_string();

    const std::string through = queue + "&through=" + std::string{id};
    EXPECT_EQ(call(drogon::Delete, through, alice).response->statusCode(), drogon::k403Forbidden);
    EXPECT_EQ(call(drogon::Delete, through, bob).response->statusCode(), drogon::k204NoContent);
    EXPECT_EQ(call(drogon::Get, queue, bob).response->body().find(ciphertext), std::string::npos);
}

// --- removal (docs/22-chat.md §7.1) ------------------------------------------------

// A member removed from an encrypted group is refused, or served only up to
// where they left, on EVERY route that names the conversation. The socket's
// half is ChatSocketListener's "a removed member is not woken": the removal
// moves `mv`, so the member list a wake is published to no longer holds them.
TEST_F(ChatRoutesListener, ARemovedMembersDeviceIsRefusedPastItsLeavingOnEveryRoute) {
    const Uuid alice = uuid::generate_v4();
    const Uuid bob = uuid::generate_v4();
    const Uuid carol = uuid::generate_v4();
    const chattest::Client phone = registered_over_http(alice);
    const chattest::Client bobs = registered_over_http(bob);
    (void)registered_over_http(carol);
    const Exchange made = call(drogon::Post, "/chat/conversations", alice,
                               R"({"cid":")" + cid() +
                                   R"(","kind":"group","title":"Team","encrypted":true,"members":[")" +
                                   uuid::to_string(bob) + R"(",")" + uuid::to_string(carol) +
                                   R"("]})");
    ASSERT_EQ(made.response->statusCode(), drogon::k201Created) << made.response->body();
    input::BodyArena arena;
    const input::JsonDocument state = input::parse_json(made.response->body(), arena);
    const Uuid c = *uuid::parse(*state.root().find("conversation")->find("id")->as_string());

    // One message bob is entitled to, with a ciphertext of his own in his queue.
    const auto send_to = [&](const std::vector<const chattest::Client*>& to, std::uint8_t tag) {
        const Exchange page = call(drogon::Get, conversation_path(c, "/devices"), alice);
        const std::int64_t dsv = *json_int(page.response->body(), "dsv");
        std::string devices;
        for (const chattest::Client* client : to) {
            if (!devices.empty()) { devices += ','; }
            devices += R"({"device":")" + uuid::to_string(client->device.id) +
                       R"(","ciphertext":")" +
                       crypto::base64url_encode(std::array<std::uint8_t, 2>{tag, tag}) + R"("})";
        }
        const Exchange sent = call(
            drogon::Post, conversation_path(c, "/messages"), alice,
            R"({"cid":")" + cid() + R"(","device":")" + uuid::to_string(phone.device.id) +
                R"(","dsv":)" + std::to_string(dsv) + R"(,"ciphertext":")" +
                crypto::base64url_encode(std::array<std::uint8_t, 2>{tag, 0}) +
                R"(","devices":[)" + devices + "]}");
        EXPECT_EQ(sent.response->statusCode(), drogon::k201Created) << sent.response->body();
        return *json_int(sent.response->body(), "seq");
    };
    std::vector<const chattest::Client*> everyone_else{&bobs};
    auto client = db::MongoPool::instance().acquire();
    const auto devices_of = [&](const Uuid& user) {
        const std::array<Uuid, 1> one{user};
        return stack().devices.devices_of(*client, one, 1).value()[0].devices;
    };
    chattest::Client carols_view{};
    carols_view.device.id = devices_of(carol)[0].id;
    everyone_else.push_back(&carols_view);
    const std::int64_t before = send_to(everyone_else, 1);

    const Exchange removed = call(drogon::Delete,
                                  conversation_path(c, "/members/" + uuid::to_string(bob)), alice);
    ASSERT_EQ(removed.response->statusCode(), drogon::k204NoContent);
    const std::int64_t after = send_to({&carols_view}, 2);

    // The remaining members are told, at a position in the log: that is the
    // signal their clients rotate their sender keys on.
    const Exchange log = call(drogon::Get, conversation_path(c, "/messages"), carol);
    EXPECT_NE(log.response->body().find(R"("event":"member_removed")"), std::string::npos);

    // Reads: up to where he left, never past it.
    const Exchange history = call(drogon::Get, conversation_path(c, "/messages"), bob);
    ASSERT_EQ(history.response->statusCode(), drogon::k200OK);
    EXPECT_NE(history.response->body().find(R"("seq":)" + std::to_string(before)),
              std::string::npos);
    EXPECT_EQ(history.response->body().find(R"("seq":)" + std::to_string(after)),
              std::string::npos);
    const Exchange caught = call(drogon::Get, conversation_path(c, "/messages?after=0"), bob);
    EXPECT_EQ(caught.response->body().find(R"("seq":)" + std::to_string(after)),
              std::string::npos);
    const Exchange got = call(drogon::Get, conversation_path(c), bob);
    ASSERT_EQ(got.response->statusCode(), drogon::k200OK);
    input::BodyArena got_arena;
    const input::JsonDocument seen = input::parse_json(got.response->body(), got_arena);
    EXPECT_LT(*seen.root().find("conversation")->find("head")->as_int64(), after);
    const Exchange queue = call(drogon::Get, "/chat/device-queue?device=" +
                                                 uuid::to_string(bobs.device.id), bob);
    ASSERT_EQ(queue.response->statusCode(), drogon::k200OK);
    EXPECT_NE(queue.response->body().find(R"("seq":)" + std::to_string(before)),
              std::string::npos);
    EXPECT_EQ(queue.response->body().find(R"("seq":)" + std::to_string(after)),
              std::string::npos);

    // Everything else that names the conversation answers as if it did not
    // exist, byte for byte with a route that does not.
    const Exchange nothing = unmatched(drogon::Get, bob);
    const std::string seq = std::to_string(before);
    const std::vector<std::pair<drogon::HttpMethod, std::string>> refused{
        {drogon::Get, conversation_path(c, "/members")},
        {drogon::Get, conversation_path(c, "/devices")},
        {drogon::Get, conversation_path(c, "/messages/" + seq + "/readers")},
    };
    for (const auto& [method, path] : refused) {
        const Exchange answer = call(method, path, bob);
        EXPECT_EQ(answer.response->statusCode(), drogon::k404NotFound) << path;
        EXPECT_TRUE(same_response(answer.response, nothing.response)) << path;
    }
    const std::string user_b = uuid::to_string(bob);
    const std::vector<std::tuple<drogon::HttpMethod, std::string, std::string>> writes{
        {drogon::Post, conversation_path(c, "/messages"),
         R"({"cid":")" + cid() + R"(","device":")" + uuid::to_string(bobs.device.id) +
             R"(","dsv":1,"ciphertext":"AAAA"})"},
        {drogon::Post, conversation_path(c, "/messages"), R"({"cid":")" + cid() + R"(","body":"hi"})"},
        {drogon::Patch, conversation_path(c, "/messages/" + seq), R"({"body":"x"})"},
        {drogon::Delete, conversation_path(c, "/messages/" + seq), ""},
        {drogon::Put, conversation_path(c, "/messages/" + seq + "/reaction"), R"({"reaction":"x"})"},
        {drogon::Post, conversation_path(c, "/receipts"), R"({"read":1})"},
        {drogon::Post, conversation_path(c, "/keys/claim"),
         R"({"users":[")" + uuid::to_string(carol) + R"("]})"},
        {drogon::Patch, conversation_path(c), R"({"title":"mine now"})"},
        {drogon::Put, conversation_path(c, "/timer"), R"({"timer":86400})"},
        {drogon::Post, conversation_path(c, "/members"), R"({"members":[")" + user_b + R"("]})"},
        {drogon::Delete, conversation_path(c, "/members/" + uuid::to_string(carol)), ""},
        {drogon::Post, conversation_path(c, "/invites"), R"({"uses":1})"},
    };
    for (const auto& [method, path, body] : writes) {
        const Exchange answer = call(method, path, bob, body);
        EXPECT_EQ(answer.response->statusCode(), drogon::k404NotFound) << path << " " << body;
    }
    // His own row stays his: he may still mute, archive or clear what he holds.
    EXPECT_EQ(call(drogon::Patch, conversation_path(c, "/preferences"), bob, R"({"archived":true})")
                  .response->statusCode(),
              drogon::k200OK);
}

}  // namespace
}  // namespace anvil
