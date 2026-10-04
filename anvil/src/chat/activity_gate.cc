#include "anvil/chat/activity_gate.h"

#include <array>
#include <exception>
#include <string_view>

#include <trantor/utils/Logger.h>

#include "anvil/redis/redis_client.h"

namespace anvil::chat {
namespace {

constexpr std::string_view kPrefix = "anvil:chat:act:";

// The key in a fixed buffer: a prefix and 32 hex digits, no allocation.
struct GateKey final {
    std::array<char, kPrefix.size() + 32> chars{};

    explicit GateKey(const Uuid& conversation) noexcept {
        constexpr std::string_view kHex = "0123456789abcdef";
        std::size_t at = 0;
        for (const char c : kPrefix) { chars[at++] = c; }
        for (const std::uint8_t byte : conversation) {
            chars[at++] = kHex[byte >> 4U];
            chars[at++] = kHex[byte & 0x0FU];
        }
    }

    [[nodiscard]] std::string_view view() const noexcept { return {chars.data(), chars.size()}; }
};

}  // namespace

std::function<bool(const Uuid& conversation)> redis_activity_gate(
    std::chrono::milliseconds window) {
    return [window](const Uuid& conversation) {
        try {
            const GateKey key{conversation};
            const std::string_view name = key.view();
            return redis::RedisClient::instance().set(
                sw::redis::StringView{name.data(), name.size()}, sw::redis::StringView{"1", 1},
                window, sw::redis::UpdateType::NOT_EXIST);
        } catch (const std::exception& e) {
            // Open, and logged once per failure rather than silenced: under an
            // outage this is every send, which is the rate the log can carry
            // only because it is also every send's own failure line elsewhere.
            LOG_WARN << "chat activity gate unavailable, bumping: " << e.what();
            return true;
        }
    };
}

}  // namespace anvil::chat
