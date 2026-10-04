#pragma once

// The reference application's chat push nudges (anvil/chat/push.h,
// docs/01-seams.md §18).
//
// anvil ships the job body; the job KIND is this application's, a row in its
// own table (anvil_app_jobs.h), and the wording is two of its templates
// (topics.h). A job handler is a plain function pointer, so the handler reaches
// the process's ChatPush through the one pointer below, set once at boot.

#include <atomic>
#include <chrono>
#include <cstdint>

#include "anvil/chat/push.h"
#include "topics.h"

namespace testapp {

// The index of "chat.push" in kJobSpecs.
inline constexpr std::uint16_t kChatPushJob = 3;

// What a deployment would start from. A test shortens the window.
[[nodiscard]] constexpr anvil::chat::PushConfig chat_push_config(
    std::chrono::seconds window = std::chrono::seconds{5},
    std::chrono::seconds grace = std::chrono::seconds{3}) noexcept {
    return anvil::chat::PushConfig{
        .window = window,
        .grace = grace,
        .preview = static_cast<anvil::notifications::TemplateId>(Template::ChatPreview),
        .plain = static_cast<anvil::notifications::TemplateId>(Template::ChatPlain),
        .topic = static_cast<anvil::notifications::TopicCode>(Topic::ChatMessage)};
}

// The process's ChatPush, or null before boot built one. A job that arrives
// before then is retried rather than dropped.
[[nodiscard]] inline std::atomic<const anvil::chat::ChatPush*>& installed_chat_push() noexcept {
    static std::atomic<const anvil::chat::ChatPush*> push{nullptr};
    return push;
}

}  // namespace testapp
