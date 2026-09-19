#include "anvil/http/trace_context.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "anvil/crypto/random.h"

namespace anvil::http {
namespace {

// 256 bytes of `.rodata`, shared across every thread and every request, and the
// reason there is no branch per nibble. `kInvalid` is stored rather than a
// sentinel that could collide with a real value, so one table answers both "is
// this a hex digit" and "which one".
inline constexpr std::uint8_t kInvalidNibble = 0xFF;

[[nodiscard]] consteval std::array<std::uint8_t, 256> build_nibble_table() {
    std::array<std::uint8_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) { table[i] = kInvalidNibble; }
    for (std::uint8_t digit = 0; digit < 10U; ++digit) {
        table[static_cast<std::size_t>('0') + digit] = digit;
    }
    // LOWERCASE ONLY. The standard's grammar is lowercase hex, and accepting the
    // other case would give one trace two spellings on the wire — which costs
    // nothing here and costs an aggregator a join that silently misses half its
    // rows. Strict on the way in is the cheap side of that.
    for (std::uint8_t offset = 0; offset < 6U; ++offset) {
        table[static_cast<std::size_t>('a') + offset] = static_cast<std::uint8_t>(10U + offset);
    }
    return table;
}

inline constexpr std::array<std::uint8_t, 256> kNibbles = build_nibble_table();

// The version reserved as invalid by the standard. Refused rather than treated
// as an unknown future version, because it is the one value that is promised
// never to name a grammar.
inline constexpr std::uint8_t kInvalidVersion = 0xFF;

// Field offsets, stated once. Every one of them is fixed by the standard, which
// is the whole reason this parser is a length compare and not a scanner.
inline constexpr std::size_t kVersionAt = 0;
inline constexpr std::size_t kTraceIdAt = 3;
inline constexpr std::size_t kSpanIdAt = 36;
inline constexpr std::size_t kFlagsAt = 53;
// The three hyphens. Listed rather than derived from the offsets above, because
// a separator the parser computes from a field's length is a separator it agrees
// with itself about and not one the wire has to carry.
inline constexpr std::array<std::size_t, 3> kSeparatorsAt = {2, 35, 52};

[[nodiscard]] std::optional<std::uint8_t> hex_byte(std::string_view text,
                                                   std::size_t at) noexcept {
    const std::uint8_t high = kNibbles[static_cast<unsigned char>(text[at])];
    const std::uint8_t low = kNibbles[static_cast<unsigned char>(text[at + 1U])];
    if (high == kInvalidNibble || low == kInvalidNibble) { return std::nullopt; }
    return static_cast<std::uint8_t>((high << 4U) | low);
}

// Fills `out` and reports whether every nibble was hex AND whether any byte was
// non-zero. Both answers come from one pass: an all-zero id is invalid, and
// checking it afterwards would read the bytes a second time to learn something
// this loop already knew.
template <std::size_t N>
[[nodiscard]] bool read_id(std::string_view text, std::size_t at,
                           std::array<std::uint8_t, N>& out) noexcept {
    std::uint8_t any = 0;
    for (std::size_t i = 0; i < N; ++i) {
        const std::optional<std::uint8_t> byte = hex_byte(text, at + (i * 2U));
        if (!byte) { return false; }
        out[i] = *byte;
        any = static_cast<std::uint8_t>(any | *byte);
    }
    return any != 0;
}

inline constexpr std::string_view kHexDigits = "0123456789abcdef";

template <std::size_t N>
void write_id(std::array<char, kTraceparentChars>& out, std::size_t at,
              const std::array<std::uint8_t, N>& id) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        out[at + (i * 2U)] = kHexDigits[(id[i] >> 4U) & 0x0FU];
        out[at + (i * 2U) + 1U] = kHexDigits[id[i] & 0x0FU];
    }
}

// 25 bytes of thread-local storage, zero-initialised, which is the absent
// context. No mutex and no map keyed by a thread id, because neither buys
// anything a thread-local does not already have.
thread_local TraceContext t_current{};

}  // namespace

std::optional<TraceContext> parse_traceparent(std::string_view header) noexcept {
    if (header.size() < kTraceparentChars) { return std::nullopt; }

    const std::optional<std::uint8_t> version = hex_byte(header, kVersionAt);
    if (!version || *version == kInvalidVersion) { return std::nullopt; }

    // A future version may APPEND fields, and the standard says to parse its
    // first 55 bytes as version 00 — so a longer header is read, not refused,
    // and the 56th byte must be the separator that proves the extra bytes are a
    // new field rather than a longer id. Refusing an unknown version outright
    // breaks forward compatibility for no gain; accepting a longer version-00
    // header accepts a value nothing agrees on.
    if (header.size() > kTraceparentChars) {
        if (*version == 0 || header[kTraceparentChars] != '-') { return std::nullopt; }
    }

    for (const std::size_t at : kSeparatorsAt) {
        if (header[at] != '-') { return std::nullopt; }
    }

    TraceContext ctx{};
    if (!read_id(header, kTraceIdAt, ctx.trace_id)) { return std::nullopt; }
    if (!read_id(header, kSpanIdAt, ctx.span_id)) { return std::nullopt; }

    const std::optional<std::uint8_t> flags = hex_byte(header, kFlagsAt);
    if (!flags) { return std::nullopt; }
    ctx.flags = *flags;
    return ctx;
}

std::array<char, kTraceparentChars> format_traceparent(const TraceContext& ctx) noexcept {
    std::array<char, kTraceparentChars> out{};
    // Version 00 on the way out, always. This process implements one grammar,
    // and a hop that re-emitted the version it received would be claiming to
    // speak a grammar it has never read a field of.
    out[kVersionAt] = '0';
    out[kVersionAt + 1U] = '0';
    for (const std::size_t at : kSeparatorsAt) { out[at] = '-'; }
    write_id(out, kTraceIdAt, ctx.trace_id);
    write_id(out, kSpanIdAt, ctx.span_id);
    out[kFlagsAt] = kHexDigits[(ctx.flags >> 4U) & 0x0FU];
    out[kFlagsAt + 1U] = kHexDigits[ctx.flags & 0x0FU];
    return out;
}

TraceContext ingest_traceparent(TraceIngest policy, bool peer_is_trusted,
                                std::string_view header) noexcept {
    if (policy != TraceIngest::TrustedPeer || !peer_is_trusted) { return TraceContext{}; }
    return parse_traceparent(header).value_or(TraceContext{});
}

TraceContext root_linked_to(const std::array<std::uint8_t, 16>& link) {
    if (!has_trace_link(link)) { return TraceContext{}; }
    TraceContext root{};
    crypto::random_bytes(std::span<std::uint8_t>{root.trace_id});
    crypto::random_bytes(std::span<std::uint8_t>{root.span_id});
    // Sampled, because the link is only ever carried by a sampled enqueue. A
    // root that is not sampled produces nothing for the link to point at.
    root.flags = kTraceFlagSampled;
    return root;
}

TraceContext current_trace() noexcept { return t_current; }

TraceScope::TraceScope(const TraceContext& ctx) noexcept : previous_{t_current} {
    t_current = ctx;
}

TraceScope::~TraceScope() { t_current = previous_; }

}  // namespace anvil::http
