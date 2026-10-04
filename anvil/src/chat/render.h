#pragma once

// One message, and the names a message is written with, as a client reads
// them (docs/22-chat.md §9).
//
// Private to `src/`. Two places in this library write a message for a client:
// the history route, and a wake that carries the message inline (§5.4). A wake
// is the same message reaching the same client by a faster path, so it must be
// the same bytes — the same field names, the same grants instead of ids — and
// a second writer is how the two would quietly come to differ.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "anvil/chat/record.h"
#include "anvil/chat/service.h"
#include "anvil/db/codec.h"

namespace anvil::chat::detail {

[[nodiscard]] constexpr std::string_view role_name(Role role) noexcept {
    switch (role) {
        case Role::Member: return "member";
        case Role::Admin:  return "admin";
        case Role::Owner:  return "owner";
    }
    return "member";
}

[[nodiscard]] constexpr std::string_view event_name(SystemEvent event) noexcept {
    switch (event) {
        case SystemEvent::Created:         return "created";
        case SystemEvent::MemberAdded:     return "member_added";
        case SystemEvent::MemberRemoved:   return "member_removed";
        case SystemEvent::MemberLeft:      return "member_left";
        case SystemEvent::RoleChanged:     return "role_changed";
        case SystemEvent::InfoChanged:     return "info_changed";
        case SystemEvent::TimerChanged:    return "timer_changed";
        case SystemEvent::JoinedByInvite:  return "joined";
        case SystemEvent::OwnerSucceeded:  return "owner_succeeded";
        case SystemEvent::DevicesChanged:  return "devices_changed";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view message_kind_name(MessageKind kind) noexcept {
    switch (kind) {
        case MessageKind::Text:   return "text";
        case MessageKind::System: return "system";
        case MessageKind::Card:   return "card";
        case MessageKind::Encrypted: return "encrypted";
    }
    return "text";
}

void append_time(std::string& out, db::TimeMs at);

void append_optional_time(std::string& out, const std::optional<db::TimeMs>& at);

// One message as a client sees it. An attachment is a GRANT, never an id: the
// client is told it can pull this, and nothing taken from storage
// (docs/22 §6.1, §6.2). Every message has the same members: `ciphertext` and
// `device` are null on a plaintext one, so a client parses one shape.
void append_message(std::string& out, const ChatService& service, const MessageRecord& row,
                    std::int64_t now_unix);

// One device as any member of a conversation may know it: enough to encrypt to
// it and to verify the chain that admitted it (docs/22 §7.3). Every key and
// signature is unpadded base64url.
void append_published_device(std::string& out, const PublishedDevice& device);

// A page of members and their devices, with the version read before them:
// `{"dsv":N,"members":[{"user":…,"devices":[…]}],"next":…|null}`. The device
// route and a stale send's 409 write the same bytes.
void append_device_page(std::string& out, const ConversationDevices& page);

}  // namespace anvil::chat::detail
