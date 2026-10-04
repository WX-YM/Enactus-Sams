#pragma once

// The reference application's conversation kinds (docs/22-chat.md §2).
//
// The WhatsApp-shaped defaults live HERE, as one application's choices, and not
// in anvil: a direct chat and a group a member may encrypt, an announcements
// group only admins post in, and a channel anyone may follow. Every number below
// is a product decision, which is the point of it being in this file.

#include <array>
#include <cstdint>

#include "anvil/chat/kind_spec.h"
#include "namespaces.h"
#include "perms.h"

namespace testapp {

namespace chat = anvil::chat;
using chat::Right;

// The timers a member may pick: a day, a week, ninety days.
inline constexpr std::array<std::uint32_t, 3> kChatTimers{{86'400U, 604'800U, 7'776'000U}};

inline constexpr chat::RightMask kMemberRights = Right::Post | Right::React;
inline constexpr chat::RightMask kAdminRights =
    kMemberRights | Right::AddMember | Right::RemoveMember | Right::EditInfo | Right::SetTimer |
    Right::CreateInvite | Right::RevokeAny | Right::Pin;
inline constexpr chat::RightMask kOwnerRights = kAdminRights | Right::ManageAdmins;

enum class ChatKind : chat::KindCode {
    Direct   = 0,
    Group    = 1,
    Announce = 2,
    Channel  = 3,
};

inline constexpr std::array<chat::ConversationKindSpec, 4> kChatKinds{{
    // Either person may set the timer, and both may delete for everyone within
    // two days. Encryption is offered, so one pair can hold one conversation of
    // each mode.
    {.key = "direct",
     .timers_s = kChatTimers,
     .create_requires = anvil::PermSet{},
     .max_members = 2,
     .retention_days = 0,
     .edit_window_s = 15U * 60U,
     .revoke_window_s = 2U * 86'400U,
     .max_text_code_points = chat::kMaxMessageCodePoints,
     .rights = {.member = kMemberRights | Right::SetTimer,
                .admin = kMemberRights | Right::SetTimer,
                .owner = kMemberRights | Right::SetTimer},
     .code = static_cast<chat::KindCode>(ChatKind::Direct),
     .shape = chat::Shape::Direct,
     .e2ee = chat::E2ee::Optional,
     .history = chat::History::FromJoin,
     .receipts = chat::Receipts::Read,
     .mentions_break_mute = false,
     // A marketplace's buyer and seller talk here, and a dispute between them
     // is reviewed by staff: the plaintext ones, never an encrypted one.
     .reviewable = true,
     .media_ns = kChat,
     .sealed_ns = kSealed},
    {.key = "group",
     .timers_s = kChatTimers,
     .create_requires = anvil::perm_mask(Perm::ChatCreateGroup),
     .max_members = 1024,
     .retention_days = 0,
     .edit_window_s = 15U * 60U,
     .revoke_window_s = 2U * 86'400U,
     .max_text_code_points = chat::kMaxMessageCodePoints,
     .rights = {.member = kMemberRights, .admin = kAdminRights, .owner = kOwnerRights},
     .code = static_cast<chat::KindCode>(ChatKind::Group),
     .shape = chat::Shape::Group,
     .e2ee = chat::E2ee::Optional,
     .history = chat::History::FromJoin,
     .receipts = chat::Receipts::Read,
     .mentions_break_mute = true,
     .reviewable = true,
     .media_ns = kChat,
     .sealed_ns = kSealed},
    // An announcements group: everybody reads and reacts, admins post. That is
    // a group whose member role lacks Post, not a shape of its own. Kept in
    // plaintext with full history, so a new member reads what was announced
    // before they joined.
    {.key = "announce",
     .timers_s = {},
     .create_requires = anvil::perm_mask(Perm::ChatCreateGroup),
     .max_members = 1024,
     .retention_days = 365,
     .edit_window_s = 0,
     .revoke_window_s = 7U * 86'400U,
     .max_text_code_points = chat::kMaxMessageCodePoints,
     .rights = {.member = static_cast<chat::RightMask>(Right::React), .admin = kAdminRights,
                .owner = kOwnerRights},
     .code = static_cast<chat::KindCode>(ChatKind::Announce),
     .shape = chat::Shape::Group,
     .e2ee = chat::E2ee::Never,
     .history = chat::History::Full,
     .receipts = chat::Receipts::Delivered,
     .mentions_break_mute = true,
     .media_ns = kChat,
     .sealed_ns = std::nullopt},
    {.key = "channel",
     .timers_s = {},
     .create_requires = anvil::perm_mask(Perm::ChatCreateChannel),
     .max_members = chat::kMaxFollowers,
     .retention_days = 90,
     .edit_window_s = 15U * 60U,
     .revoke_window_s = 30U * 86'400U,
     .max_text_code_points = 2048,
     .rights = {.member = static_cast<chat::RightMask>(Right::React), .admin = kAdminRights,
                .owner = kOwnerRights},
     .code = static_cast<chat::KindCode>(ChatKind::Channel),
     .shape = chat::Shape::Channel,
     .e2ee = chat::E2ee::Never,
     .history = chat::History::Full,
     .receipts = chat::Receipts::Off,
     .mentions_break_mute = false,
     .media_ns = kChat,
     .sealed_ns = std::nullopt},
}};

static_assert(chat::kinds_are_well_formed(kChatKinds));

}  // namespace testapp
