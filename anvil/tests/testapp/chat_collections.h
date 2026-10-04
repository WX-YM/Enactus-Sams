#pragma once

// The reference application's chat collection names, as the one struct the
// chat repository takes. The names are declared in anvil_app_config.h with the
// rest; this only gathers them.

#include <chrono>

#include "anvil/chat/devices.h"
#include "anvil/chat/repository.h"

namespace testapp {

inline constexpr anvil::chat::ChatCollections kChatCollections{
    .conversations = "chat_conversations",
    .members = "chat_members",
    .messages = "chat_messages",
    .reactions = "chat_reactions",
    .invites = "chat_invites",
    .blocks = "chat_blocks",
    .reports = "chat_reports",
};

// --- chat devices (docs/22-chat.md §7.3) -------------------------------------

inline constexpr anvil::chat::DeviceCollections kDeviceCollections{
    .identities = "chat_identities",
    .prekeys = "chat_prekeys",
    .queue = "chat_device_queue",
    .links = "chat_link_requests",
};

// The library's defaults, stated so the assertion below is about this
// application's choice rather than about anvil's.
inline constexpr anvil::chat::DeviceConfig kDeviceConfig{
    .max_devices = 5,
    .touch_interval = std::chrono::minutes{60},
};
static_assert(anvil::chat::device_config_is_well_formed(kDeviceConfig));

// --- end chat devices --------------------------------------------------------

}  // namespace testapp
