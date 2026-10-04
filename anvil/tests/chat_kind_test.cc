// The conversation-kind seam: every refusal in docs/22-chat.md §2.3, each as a
// table that differs from the reference application's by one field.
//
// They are static_asserts rather than runtime cases on purpose. The seam's
// whole promise is that a malformed table is a COMPILE error in the
// application's build, so the check has to be usable in a constant expression,
// and the only proof of that is a constant expression that uses it.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "anvil/chat/kind_spec.h"
#include "chat_kinds.h"
#include "namespaces.h"

namespace {

namespace chat = anvil::chat;
using chat::Right;
using Table = std::array<chat::ConversationKindSpec, 4>;

constexpr std::size_t kDirect = 0;
constexpr std::size_t kGroup = 1;
constexpr std::size_t kChannel = 3;

template <typename Mutate>
[[nodiscard]] constexpr bool accepted_after(Mutate mutate) {
    Table table = testapp::kChatKinds;
    mutate(table);
    return chat::kinds_are_well_formed(table);
}

constexpr std::array<std::uint32_t, 2> kDescending{{604'800U, 86'400U}};
constexpr std::array<std::uint32_t, 1> kNinetyDays{{7'776'000U}};

// --- the table as a whole ----------------------------------------------------

static_assert(chat::kinds_are_well_formed(testapp::kChatKinds));
static_assert(!chat::kinds_are_well_formed(std::span<const chat::ConversationKindSpec>{}),
              "an empty table is a seam nobody filled");
static_assert(!accepted_after([](Table& t) { t[kGroup].code = 7; }),
              "the table is indexed by its stored code");
static_assert(!accepted_after([](Table& t) { t[kGroup].key = "direct"; }), "keys are unique");
static_assert(!accepted_after([](Table& t) { t[kGroup].key = "Group"; }), "keys are lowercase");
static_assert(!accepted_after([](Table& t) { t[kGroup].key = ""; }));

static_assert(!accepted_after([](Table& t) {
                  t[kGroup].e2ee = chat::E2ee::Required;
                  t[kGroup].media_ns = std::nullopt;
                  t[kGroup].reviewable = true;
              }),
              "a kind that is always encrypted holds nothing a reviewer could read");
static_assert(accepted_after([](Table& t) { t[kChannel].reviewable = true; }),
              "any plaintext kind may be reviewable");

// --- shapes --------------------------------------------------------------------

static_assert(!accepted_after([](Table& t) { t[kDirect].max_members = 3; }),
              "a direct conversation is exactly two people");
static_assert(!accepted_after([](Table& t) {
                  t[kDirect].rights.owner = t[kDirect].rights.owner | Right::AddMember;
              }),
              "nobody may add a third person to a direct conversation");
static_assert(!accepted_after([](Table& t) { t[kGroup].max_members = chat::kMaxMembers + 1; }),
              "a group's member list is cached per process and bumped per message");
static_assert(!accepted_after([](Table& t) { t[kChannel].e2ee = chat::E2ee::Optional; }),
              "an encrypted channel's key is effectively public");
static_assert(!accepted_after([](Table& t) { t[kChannel].receipts = chat::Receipts::Read; }),
              "receipts from every follower are a write storm");
static_assert(!accepted_after([](Table& t) {
                  t[kChannel].rights.member = t[kChannel].rights.member | Right::Post;
              }),
              "a follower who can post is a member of a group");

// --- rights ----------------------------------------------------------------------

static_assert(!accepted_after([](Table& t) {
                  t[kGroup].rights.member = t[kGroup].rights.member | Right::ManageAdmins;
              }),
              "a member who can do what an admin cannot is two names swapped");
static_assert(!accepted_after([](Table& t) { t[kGroup].rights.owner = 0xFFFFU; }),
              "a right anvil does not define is a right nothing checks");

// --- timers and windows --------------------------------------------------------

static_assert(!accepted_after([](Table& t) { t[kGroup].timers_s = kDescending; }),
              "timers are strictly ascending");
static_assert(!accepted_after([](Table& t) {
                  t[kGroup].timers_s = kNinetyDays;
                  t[kGroup].retention_days = 30;
              }),
              "a timer longer than retention promises a lifetime the server will not keep");
static_assert(!accepted_after([](Table& t) { t[kGroup].edit_window_s = chat::kMaxWindowSeconds + 1; }));
static_assert(!accepted_after([](Table& t) {
                  t[kGroup].max_text_code_points = chat::kMaxMessageCodePoints + 1;
              }),
              "a kind may lower the text bound, never raise it");

// --- storage --------------------------------------------------------------------

static_assert(!accepted_after([](Table& t) { t[kGroup].media_ns = testapp::kMedia; }),
              "chat media in a public namespace is served by an id alone");
static_assert(!accepted_after([](Table& t) { t[kGroup].sealed_ns = std::nullopt; }),
              "an encrypted kind needs somewhere to put ciphertext");
static_assert(!accepted_after([](Table& t) { t[kGroup].sealed_ns = testapp::kChat; }),
              "a sealed namespace never deduplicates, and takes only the sealed class");
static_assert(!accepted_after([](Table& t) { t[kGroup].sealed_ns = testapp::kContent; }),
              "a sealed namespace takes only the sealed class");
static_assert(!accepted_after([](Table& t) { t[kChannel].media_ns = testapp::kSealed; }),
              "plaintext attachments never go where nothing is sniffed");
static_assert(!accepted_after([](Table& t) {
                  t[kGroup].e2ee = chat::E2ee::Required;
                  t[kGroup].media_ns = std::nullopt;
                  t[kGroup].sealed_ns = testapp::kGuest;
              }),
              "a sealed namespace that takes images is refused");
static_assert(!accepted_after([](Table& t) { t[kGroup].history = chat::History::Full; }),
              "a joiner cannot decrypt what was sent before they joined");
static_assert(!accepted_after([](Table& t) { t[kGroup].e2ee = chat::E2ee::Required; }),
              "a required-encryption kind cannot also take plaintext attachments");
static_assert(accepted_after([](Table& t) {
                  t[kGroup].e2ee = chat::E2ee::Required;
                  t[kGroup].media_ns = std::nullopt;
                  t[kGroup].reviewable = false;
              }));
static_assert(!accepted_after([](Table& t) { t[kChannel].sealed_ns = testapp::kSealed; }),
              "a plaintext kind has no use for a sealed namespace");

TEST(ChatKinds, LookupsGoThroughTheTableAndNowhereElse) {
    const auto group = chat::kind_from_key(testapp::kChatKinds, "group");
    ASSERT_TRUE(group.has_value());
    EXPECT_EQ(*group, static_cast<chat::KindCode>(testapp::ChatKind::Group));
    EXPECT_FALSE(chat::kind_from_key(testapp::kChatKinds, "GROUP").has_value());
    EXPECT_FALSE(chat::kind_from_key(testapp::kChatKinds, "").has_value());

    EXPECT_TRUE(chat::kind_from_stored(testapp::kChatKinds, 3).has_value());
    EXPECT_FALSE(chat::kind_from_stored(testapp::kChatKinds, 4).has_value());
    EXPECT_FALSE(chat::kind_from_stored(testapp::kChatKinds, -1).has_value());
}

TEST(ChatKinds, RightsAreReadFromTheKindForTheRole) {
    const chat::ConversationKindSpec& announce = testapp::kChatKinds[2];
    EXPECT_FALSE(chat::role_may(announce, chat::Role::Member, Right::Post));
    EXPECT_TRUE(chat::role_may(announce, chat::Role::Member, Right::React));
    EXPECT_TRUE(chat::role_may(announce, chat::Role::Admin, Right::Post));
    EXPECT_TRUE(chat::role_may(announce, chat::Role::Owner, Right::ManageAdmins));
    EXPECT_FALSE(chat::role_may(announce, chat::Role::Admin, Right::ManageAdmins));
}

TEST(ChatKinds, OnlyADeclaredTimerOrOffMayBeChosen) {
    const chat::ConversationKindSpec& group = testapp::kChatKinds[1];
    EXPECT_TRUE(chat::timer_allowed(group, 0));
    EXPECT_TRUE(chat::timer_allowed(group, 86'400U));
    EXPECT_FALSE(chat::timer_allowed(group, 3'600U));
    EXPECT_FALSE(chat::timer_allowed(testapp::kChatKinds[2], 86'400U));
}

}  // namespace
