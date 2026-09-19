#pragma once

// The topic seam: what an application needs to declare its notification channels,
// with no dependency on its own configuration header.
//
// A topic is a COMPILE-TIME channel kind, optionally scoped to a subject
// resource. It is never derived from request data, for the same reason a
// collection name and a storage namespace are not — and here the reason is
// sharper than usual: SUBSCRIPTION IS THE DISCLOSURE. A topic a caller can name
// as a string is a topic a caller can name as anything, and a caller who can
// subscribe to another account's topic receives a stream of their activity.
//
// Everything that decides a topic's behaviour is `constexpr` metadata: fan-out
// strategy, default channels, required permission, coalescing window, retention.
// That matters beyond tidiness — a fan-out strategy chosen at runtime is a
// strategy that can flip under load, and the whole point of the two strategies is
// that a broadcast to 20 000 subscribers costs what a broadcast to three costs.
//
// Same shape and same reason as anvil/db/collection_spec.h and
// anvil/identity/capability_spec.h: an application's <anvil_app_config.h> or its
// own `topics.h` includes THIS to spell its table, and hands the table over as a
// `std::span`.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include "anvil/core/perm_set.h"
#include "anvil/core/types.h"

namespace anvil::notifications {

// A topic kind as it is STORED, on every notification row, and as a BIT POSITION
// inside the per-client preference masks.
//
// 0..63. NEVER renumber: a stored row refers to a code for as long as its
// retention lasts, and a renumbering silently reinterprets every one of them.
using TopicCode = std::uint8_t;

// The preference masks are 64 bits per channel, indexed by topic code. A 65th
// topic would fall silently outside every client's preferences and would then be
// undisableable, so it is a build error instead.
inline constexpr std::size_t kMaxTopicKinds = 64;

// |       | Publish cost           | Read cost                      |
// | Write | O(subscribers) inserts | one indexed range scan         |
// | Read  | one insert             | merge across subscribed topics |
//
// Picking one GLOBALLY is the mistake. A broadcast under fan-out-on-write is
// 20 000 inserts and a write storm that stalls db_pool; a targeted notification
// under fan-out-on-read makes every reader merge a topic only one of them can
// see. So it is per topic, and it is compile-time.
enum class FanOut : std::uint8_t {
    // Targeted: the audience is one account or a handful. One inbox row each, and
    // read state is per row and exact.
    Write = 0,
    // Broadcast: ONE row however large the audience. A subscriber's view is a
    // range query over the topics they subscribe to, and read state is a
    // watermark.
    Read = 1,
};

[[nodiscard]] constexpr std::string_view fanout_name(FanOut fanout) noexcept {
    switch (fanout) {
        case FanOut::Write: return "write";
        case FanOut::Read:  return "read";
    }
    return "write";
}

// What a topic's SUBJECT identifies. It decides whether a default subscription
// can be written for a client the subsystem creates on its own.
//
// It is metadata rather than a rule at the call site because the call site is
// where such a rule drifts: a hand-written list of "topics a new account is
// subscribed to" is a second copy of the table, and the second copy is the one
// nobody updates when a topic is added.
enum class Scope : std::uint8_t {
    // No subject. One channel, anybody may subscribe, and subscribing discloses
    // nothing.
    Global = 0,
    // The subject is a RESOURCE id. A blanket subscription is refused by
    // construction: whoever owns the resource subscribes deliberately, because
    // subscribing to a scoped, permission-gated topic IS the disclosure.
    Resource = 1,
    // The subject is the ACCOUNT the notification is about — the reader's own id.
    // Self-scoped, so a default subscription is safe, and it is what keeps a
    // targeted security topic from reaching every other account's endpoints.
    Account = 2,
};

[[nodiscard]] constexpr std::string_view scope_name(Scope scope) noexcept {
    switch (scope) {
        case Scope::Global:   return "global";
        case Scope::Resource: return "resource";
        case Scope::Account:  return "account";
    }
    return "global";
}

// One collection holds every endpoint kind because they share lifecycle,
// preferences and delivery bookkeeping. The values are STORED.
//
// anvil owns this list rather than the application, because these are the
// transports anvil itself implements: adding a fifth means shipping a fifth
// sender, not declaring a row.
enum class ClientType : std::uint8_t {
    InApp   = 0,
    WebPush = 1,
    Email   = 2,
    Webhook = 3,
};

inline constexpr std::size_t kChannelCount = 4;
inline constexpr ClientType kMaxClientType = ClientType::Webhook;

// The names these three enums go on the wire under, for a client generated from
// the descriptor (anvil/descriptor/descriptor.h).
//
// None of the three is persisted as a name — the code is what a row carries — so
// a name costs nothing per stored document and saves every consumer a copy of an
// enum whose values only matter inside this process. The same argument
// sections::field_type_name makes, and the same rule: naming every enumerator
// rather than defaulting means adding one and forgetting the switch is a
// -Wswitch error instead of an empty string on the wire.
[[nodiscard]] constexpr std::string_view channel_name(ClientType type) noexcept {
    switch (type) {
        case ClientType::InApp:   return "in_app";
        case ClientType::WebPush: return "web_push";
        case ClientType::Email:   return "email";
        case ClientType::Webhook: return "webhook";
    }
    return "in_app";
}

// A set of channels as a bitmask over ClientType. One byte, and a delivery
// decision is one AND.
using ChannelMask = std::uint8_t;

[[nodiscard]] constexpr ChannelMask channel_bit(ClientType type) noexcept {
    return static_cast<ChannelMask>(1U << static_cast<std::uint8_t>(type));
}

template <typename... Types>
[[nodiscard]] constexpr ChannelMask channels(Types... types) noexcept {
    return static_cast<ChannelMask>((channel_bit(types) | ... | 0U));
}

inline constexpr ChannelMask kNoChannels = 0;

// "Whatever the topic declares". A publish that passed 0 would otherwise be
// indistinguishable from one that meant "no transports at all", and the two want
// opposite behaviour.
inline constexpr ChannelMask kDefaultChannels = 0xFF;

[[nodiscard]] constexpr bool has_channel(ChannelMask mask, ClientType type) noexcept {
    return (mask & channel_bit(type)) != 0U;
}

// 24 bytes, trivially copyable: a register pair plus one load.
struct TopicRef final {
    Uuid      subject;   // all-zero => unscoped/global
    TopicCode kind;
    std::array<std::uint8_t, 7> pad;   // keeps sizeof stable and explicit
};

static_assert(sizeof(TopicRef) == 24);
static_assert(std::is_trivially_copyable_v<TopicRef>);

[[nodiscard]] constexpr TopicRef global_topic(TopicCode kind) noexcept {
    return TopicRef{{}, kind, {}};
}

[[nodiscard]] constexpr TopicRef scoped_topic(TopicCode kind, const Uuid& subject) noexcept {
    return TopicRef{subject, kind, {}};
}

[[nodiscard]] constexpr bool is_scoped(const TopicRef& topic) noexcept {
    return !is_nil(topic.subject);
}

[[nodiscard]] constexpr bool same_topic(const TopicRef& a, const TopicRef& b) noexcept {
    return a.kind == b.kind && a.subject == b.subject;
}

// Ordered largest-alignment-first so the table packs.
struct TopicSpec final {
    // For logs and for the preferences API. NEVER translated and never parsed
    // from a request: an investigator lining a screen up against a server log
    // needs the same word on both.
    std::string_view key;               // 16

    // Empty for a public topic. A subscribe to a topic whose permissions the
    // caller does not hold is refused — and on a staff-facing topic it is refused
    // with the stealth 404, because the existence of the topic is itself the
    // thing being protected.
    PermSet          required;          // 16

    std::uint32_t    retention_days;    //  4

    // 0 disables coalescing. Non-zero merges repeats inside the window into one
    // row with `count` incremented — "3 new form submissions", not three rows.
    std::uint16_t    coalesce_window_s; //  2

    // STORED, and a bit position in the preference masks. Append only.
    TopicCode        code;              //  1
    FanOut           fanout;            //  1
    ChannelMask      default_channels;  //  1
    Scope            scope;             //  1

    // Whether a denial on this topic uses the stealth 404 rather than a real
    // status. Staff-facing topics are secret; an account's own topics are not.
    bool             stealth_on_denial; //  1

    // May the reader turn it off? A security notification may not: an account
    // that can silence its own "new sign-in" alert has no alert, which is
    // precisely the state an attacker wants it in.
    bool             user_optional;     //  1
};

static_assert(sizeof(TopicSpec) == 48, "TopicSpec must not grow padding");

// --- table conformance ------------------------------------------------------

// A malformed table is a build error, not a runtime surprise. Each condition is a
// mistake that would otherwise ship.
[[nodiscard]] constexpr bool topic_table_is_well_formed(
    std::span<const TopicSpec> table) noexcept {
    if (table.empty() || table.size() > kMaxTopicKinds) { return false; }
    for (std::size_t i = 0; i < table.size(); ++i) {
        const TopicSpec& spec = table[i];
        if (spec.key.empty()) { return false; }
        if (spec.code >= kMaxTopicKinds) { return false; }
        // A topic nothing can be delivered on is a topic that publishes into
        // nothing — and it looks entirely correct while doing it.
        if (spec.default_channels == kNoChannels) { return false; }
        if (spec.default_channels == kDefaultChannels) { return false; }
        // Retention is what the row's TTL is built from. Zero would expire every
        // notification the instant it was written.
        if (spec.retention_days == 0) { return false; }
        // A security topic the reader cannot disable must also be one the storm
        // breaker will not shed, and both are the same flag — so a topic that is
        // not user_optional says so once and means it everywhere.
        for (std::size_t j = 0; j < i; ++j) {
            if (table[j].code == spec.code) { return false; }
            if (table[j].key == spec.key) { return false; }
        }
    }
    return true;
}

// Codes are exactly 0..N-1 in table order, so a lookup is one bounds check and
// one index rather than a scan. Assert it and the fan-out decision costs nothing.
[[nodiscard]] constexpr bool topics_are_dense_from_zero(
    std::span<const TopicSpec> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].code != static_cast<TopicCode>(i)) { return false; }
    }
    return true;
}

// --- lookup -----------------------------------------------------------------

// nullptr for a code this build does not declare — a real state during a rolling
// deploy, when a row written by a newer process names a topic an older one has
// never heard of. Refusing to interpret it is the correct direction to fail.
[[nodiscard]] constexpr const TopicSpec* topic_spec(std::span<const TopicSpec> table,
                                                    TopicCode code) noexcept {
    const std::size_t index = code;
    if (index < table.size() && table[index].code == code) { return &table[index]; }
    for (const TopicSpec& spec : table) {
        if (spec.code == code) { return &spec; }
    }
    return nullptr;
}

// By wire key, for the preferences API. Linear over a few dozen string_views is a
// handful of cache lines and a length-first compare — cheaper than the hash a map
// would compute, with no runtime initialisation.
[[nodiscard]] constexpr const TopicSpec* topic_by_key(std::span<const TopicSpec> table,
                                                      std::string_view key) noexcept {
    for (const TopicSpec& spec : table) {
        if (spec.key == key) { return &spec; }
    }
    return nullptr;
}

[[nodiscard]] constexpr std::string_view topic_key(std::span<const TopicSpec> table,
                                                   TopicCode code) noexcept {
    const TopicSpec* spec = topic_spec(table, code);
    return spec == nullptr ? std::string_view{} : spec->key;
}

// --- preferences ------------------------------------------------------------
//
// Four 64-bit masks, one per channel: 32 bytes, and a delivery decision is one
// shift and one AND. A document of per-topic booleans would be roughly 20 bytes
// EACH, per client, and would have to be read before every delivery.
//
// A SET bit means enabled. New clients are created with every bit set, so adding
// a topic does not silently mute it for everyone who registered earlier.
inline constexpr std::size_t kPrefBytes = kChannelCount * 8;

struct Preferences final {
    std::array<std::uint64_t, kChannelCount> masks;

    [[nodiscard]] constexpr bool enabled(TopicCode kind, ClientType type) const noexcept {
        const std::size_t topic = kind;
        const auto channel = static_cast<std::size_t>(type);
        if (topic >= kMaxTopicKinds || channel >= kChannelCount) { return false; }
        return ((masks[channel] >> topic) & 1U) != 0U;
    }

    constexpr void set(TopicCode kind, ClientType type, bool on) noexcept {
        const std::size_t topic = kind;
        const auto channel = static_cast<std::size_t>(type);
        if (topic >= kMaxTopicKinds || channel >= kChannelCount) { return; }
        const std::uint64_t bit = std::uint64_t{1} << topic;
        if (on) {
            masks[channel] |= bit;
        } else {
            masks[channel] &= ~bit;
        }
    }

    // Little-endian, low byte first, channel order = ClientType order. Fixed
    // explicitly because these bytes are PERSISTED: a value written today must
    // read identically on any platform and on any future build. Same reasoning,
    // and the same wire discipline, as PermSet.
    [[nodiscard]] constexpr std::array<std::uint8_t, kPrefBytes> to_bytes() const noexcept {
        std::array<std::uint8_t, kPrefBytes> out{};
        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            for (std::size_t byte = 0; byte < 8; ++byte) {
                out[(channel * 8) + byte] =
                    static_cast<std::uint8_t>((masks[channel] >> (byte * 8)) & 0xFFU);
            }
        }
        return out;
    }

    [[nodiscard]] static constexpr Preferences from_bytes(
        const std::array<std::uint8_t, kPrefBytes>& bytes) noexcept {
        Preferences out{};
        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            std::uint64_t mask = 0;
            for (std::size_t byte = 0; byte < 8; ++byte) {
                mask |= static_cast<std::uint64_t>(bytes[(channel * 8) + byte]) << (byte * 8);
            }
            out.masks[channel] = mask;
        }
        return out;
    }

    [[nodiscard]] static constexpr Preferences all_enabled() noexcept {
        Preferences out{};
        for (std::uint64_t& mask : out.masks) { mask = ~std::uint64_t{0}; }
        return out;
    }

    [[nodiscard]] constexpr bool operator==(const Preferences&) const noexcept = default;
};

static_assert(sizeof(Preferences) == kPrefBytes);
static_assert(std::is_trivially_copyable_v<Preferences>);

// The ONE authority on "should this client be delivered this topic on this
// channel".
//
// The `user_optional` exemption lives HERE rather than in the stored preferences,
// so a security topic cannot be muted by a client row written by an older build
// or by a hand-edited document. A stored mask is data; this is the rule.
[[nodiscard]] constexpr bool should_deliver(const TopicSpec& spec, ClientType type,
                                            ChannelMask requested,
                                            const Preferences& prefs) noexcept {
    const ChannelMask mask = requested == kDefaultChannels ? spec.default_channels : requested;
    if (!has_channel(mask, type)) { return false; }
    if (!spec.user_optional) { return true; }
    return prefs.enabled(spec.code, type);
}

}  // namespace anvil::notifications
