#include "anvil/core/uuid.h"

#include <openssl/rand.h>

#include <chrono>
#include <stdexcept>

namespace anvil::uuid {
namespace {

constexpr std::array<char, 16> kHexDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

constexpr std::array<char, 64> kB64Url{
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_'};

// Reverse table built at compile time; 0xFF marks an invalid character.
constexpr std::array<std::uint8_t, 256> kB64UrlReverse = [] {
    std::array<std::uint8_t, 256> table{};
    for (auto& slot : table) { slot = 0xFFU; }
    for (std::size_t i = 0; i < kB64Url.size(); ++i) {
        table[static_cast<std::size_t>(static_cast<unsigned char>(kB64Url[i]))] =
            static_cast<std::uint8_t>(i);
    }
    return table;
}();

constexpr std::array<std::uint8_t, 256> kHexReverse = [] {
    std::array<std::uint8_t, 256> table{};
    for (auto& slot : table) { slot = 0xFFU; }
    for (std::uint8_t i = 0; i < 10; ++i) { table[static_cast<std::size_t>('0' + i)] = i; }
    for (std::uint8_t i = 0; i < 6; ++i) {
        table[static_cast<std::size_t>('a' + i)] = static_cast<std::uint8_t>(10 + i);
        table[static_cast<std::size_t>('A' + i)] = static_cast<std::uint8_t>(10 + i);
    }
    return table;
}();

// A CSPRNG failure is not recoverable and must never be papered over with a
// weaker source: silently degrading here produces guessable capability ids.
void fill_random(std::uint8_t* out, std::size_t len) {
    if (RAND_bytes(out, static_cast<int>(len)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
}

}  // namespace

Uuid generate_v4() {
    Uuid id{};
    fill_random(id.data(), id.size());
    id[6] = static_cast<std::uint8_t>((id[6] & 0x0FU) | 0x40U);  // version 4
    id[8] = static_cast<std::uint8_t>((id[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
    return id;
}

Uuid generate_v7() {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto ts = static_cast<std::uint64_t>(now_ms);

    Uuid id{};
    fill_random(id.data() + 6, id.size() - 6);

    id[0] = static_cast<std::uint8_t>((ts >> 40) & 0xFFU);
    id[1] = static_cast<std::uint8_t>((ts >> 32) & 0xFFU);
    id[2] = static_cast<std::uint8_t>((ts >> 24) & 0xFFU);
    id[3] = static_cast<std::uint8_t>((ts >> 16) & 0xFFU);
    id[4] = static_cast<std::uint8_t>((ts >> 8) & 0xFFU);
    id[5] = static_cast<std::uint8_t>(ts & 0xFFU);

    id[6] = static_cast<std::uint8_t>((id[6] & 0x0FU) | 0x70U);  // version 7
    id[8] = static_cast<std::uint8_t>((id[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
    return id;
}

std::int64_t v7_timestamp_ms(const Uuid& id) noexcept {
    std::uint64_t ts = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        ts = (ts << 8) | static_cast<std::uint64_t>(id[i]);
    }
    return static_cast<std::int64_t>(ts);
}

Uuid v7_boundary(std::int64_t epoch_ms) noexcept {
    constexpr std::uint64_t kMax48 = (std::uint64_t{1} << 48U) - 1U;
    const std::uint64_t ts =
        epoch_ms <= 0 ? 0U : std::min(static_cast<std::uint64_t>(epoch_ms), kMax48);

    Uuid id{};
    for (std::size_t i = 0; i < 6; ++i) {
        id[i] = static_cast<std::uint8_t>((ts >> ((5 - i) * 8U)) & 0xFFU);
    }
    // No version or variant nibble: this is a comparison bound, never a stored
    // id, and forcing 0x70 into byte 6 would raise the bound above ids created in
    // the same millisecond whose random bits happen to be lower.
    return id;
}

std::string to_string(const Uuid& id) {
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) { out.push_back('-'); }
        out.push_back(kHexDigits[static_cast<std::size_t>(id[i] >> 4)]);
        out.push_back(kHexDigits[static_cast<std::size_t>(id[i] & 0x0FU)]);
    }
    return out;
}

std::optional<Uuid> parse(std::string_view text) noexcept {
    if (text.size() != 36) { return std::nullopt; }
    if (text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        return std::nullopt;
    }

    Uuid id{};
    std::size_t byte = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { continue; }
        const std::uint8_t hi = kHexReverse[static_cast<unsigned char>(text[i])];
        const std::uint8_t lo = kHexReverse[static_cast<unsigned char>(text[i + 1])];
        if (hi == 0xFFU || lo == 0xFFU) { return std::nullopt; }
        id[byte++] = static_cast<std::uint8_t>((hi << 4) | lo);
        ++i;
    }
    return id;
}

std::array<char, 22> to_base64url(const Uuid& id) noexcept {
    std::array<char, 22> out{};
    std::size_t o = 0;
    // Five full 3-byte groups, then one leftover byte.
    for (std::size_t i = 0; i + 2 < id.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(id[i]) << 16) |
                                (static_cast<std::uint32_t>(id[i + 1]) << 8) |
                                static_cast<std::uint32_t>(id[i + 2]);
        out[o++] = kB64Url[(v >> 18) & 0x3FU];
        out[o++] = kB64Url[(v >> 12) & 0x3FU];
        out[o++] = kB64Url[(v >> 6) & 0x3FU];
        out[o++] = kB64Url[v & 0x3FU];
    }
    const std::uint32_t tail = static_cast<std::uint32_t>(id[15]);
    out[o++] = kB64Url[(tail >> 2) & 0x3FU];
    out[o] = kB64Url[(tail << 4) & 0x3FU];
    return out;
}

std::optional<Uuid> from_base64url(std::string_view text) noexcept {
    if (text.size() != 22) { return std::nullopt; }

    Uuid id{};
    std::size_t byte = 0;
    for (std::size_t i = 0; i + 3 < 20; i += 4) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::uint8_t d = kB64UrlReverse[static_cast<unsigned char>(text[i + k])];
            if (d == 0xFFU) { return std::nullopt; }
            v = (v << 6) | d;
        }
        id[byte++] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
        id[byte++] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
        id[byte++] = static_cast<std::uint8_t>(v & 0xFFU);
    }
    const std::uint8_t a = kB64UrlReverse[static_cast<unsigned char>(text[20])];
    const std::uint8_t b = kB64UrlReverse[static_cast<unsigned char>(text[21])];
    if (a == 0xFFU || b == 0xFFU) { return std::nullopt; }
    id[15] = static_cast<std::uint8_t>((a << 2) | (b >> 4));
    return id;
}

}  // namespace anvil::uuid
