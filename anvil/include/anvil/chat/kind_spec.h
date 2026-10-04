#pragma once

// Conversation kinds: the seam that makes one chat subsystem a direct
// messenger, a group chat, an announcements list and a one-to-many channel
// (docs/22-chat.md §2, docs/01-seams.md §18).
//
// anvil ships the vocabulary, the checks and the lookups. Which kinds exist,
// how big each may get, who may do what in it, how long anything is kept and
// whether it is encrypted are all the application's. There is no "group of
// 1024", no "admins only" and no "24 hours" anywhere in anvil; each of them is
// a value in somebody's table.
//
// --- why a stored code, and never a string from a request -------------------
//
// A conversation stores its kind as a one-byte code, and the code indexes the
// table. Joining is the disclosure, for the reason a notification topic is a
// code (docs/01-seams.md §7a): a kind a caller could name freely is a kind a
// caller could name as anything. A request that creates a conversation names a
// kind by KEY, which is looked up in the table and refused when absent; nothing
// past that lookup ever sees the request's spelling.
//
// --- what a malformed table would do -----------------------------------------
//
// kinds_are_well_formed refuses every combination in docs/22 §2.3. Each one is
// a conversation that misbehaves at run time with nothing in the table looking
// wrong in review: a "direct" chat somebody can add a third person to, an
// encrypted channel whose key is effectively public, a member who can do what
// an admin cannot, chat media stored in a namespace another handler serves.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "anvil/chat/text.h"
#include "anvil/core/perm_set.h"
#include "anvil/fs/namespace.h"

namespace anvil::chat {

// STORED on every conversation and therefore permanent: append only, never
// reorder, never reuse a retired code. The table is indexed by it.
using KindCode = std::uint8_t;

// Thirty-two, because a kind is a product decision and a product with more than
// a few dozen kinds of conversation is describing content in its kind table.
inline constexpr std::size_t kMaxKinds = 32;

// --- the bounds anvil sets (docs/22 §2.4) ---------------------------------------
//
// A conversation that is not a channel writes per member on the send path (the
// activity bump, §5.3) and caches its member list per process (§5.4); under
// encryption every membership change redistributes a sender key to every
// member's devices. Those costs are anvil's, so the bound is too.
inline constexpr std::uint32_t kMaxMembers = 1024;

// A channel writes nothing per follower per message, so its bound is the
// membership index rather than the send path.
inline constexpr std::uint32_t kMaxFollowers = 1'000'000;

// The longest disappearing timer or edit window anvil will hold: a year. Past
// that a "timer" is retention, which the kind states separately.
inline constexpr std::uint32_t kMaxWindowSeconds = 366U * 24U * 3600U;

// --- shape ----------------------------------------------------------------------

enum class Shape : std::uint8_t {
    // Exactly two members, fixed at creation, unique per pair and per
    // encryption mode. Cannot be joined, left or renamed.
    Direct,
    // Two up to max_members, posting by whoever holds Right::Post.
    Group,
    // Followers, up to max_members. Only owners and admins post; no receipts,
    // no typing, no member list visible to followers, never encrypted.
    Channel,
};

enum class E2ee : std::uint8_t {
    Never,
    // The creator chooses, once, at creation. A conversation's mode never
    // changes afterwards: a flag that can be flipped later is a downgrade
    // switch, and whoever holds the server holds it.
    Optional,
    Required,
};

enum class History : std::uint8_t {
    // A joiner sees from the seq at which they joined.
    FromJoin,
    // A joiner sees everything the conversation still holds.
    Full,
};

enum class Receipts : std::uint8_t { Off, Delivered, Read };

// --- roles and rights -------------------------------------------------------------

// STORED as the member row's role, so append only.
enum class Role : std::uint8_t { Member = 0, Admin = 1, Owner = 2 };

inline constexpr std::size_t kRoleCount = 3;

// anvil's vocabulary, because the handlers are what check it. Which role holds
// which right, per kind, is the application's.
enum class Right : std::uint16_t {
    Post         = 1U << 0U,
    React        = 1U << 1U,
    AddMember    = 1U << 2U,
    RemoveMember = 1U << 3U,
    EditInfo     = 1U << 4U,
    SetTimer     = 1U << 5U,
    ManageAdmins = 1U << 6U,
    CreateInvite = 1U << 7U,
    // Revoke anybody's message, at any time: a moderator's right.
    RevokeAny    = 1U << 8U,
    Pin          = 1U << 9U,
};

using RightMask = std::uint16_t;

inline constexpr RightMask kAllRights = 0x03FFU;

// Each right's name on the wire, in BIT order: kRightNames[i] names 1 << i. The
// descriptor publishes a kind's rights by these, so a client greys out what a
// role cannot do from the same table the handlers check.
inline constexpr std::array<std::string_view, 10> kRightNames{
    {"post", "react", "add_member", "remove_member", "edit_info", "set_timer",
     "manage_admins", "create_invite", "revoke_any", "pin"}};
static_assert(kAllRights == (1U << kRightNames.size()) - 1U,
              "a right without a name is a right the descriptor cannot publish");

[[nodiscard]] constexpr RightMask operator|(Right a, Right b) noexcept {
    return static_cast<RightMask>(static_cast<RightMask>(a) | static_cast<RightMask>(b));
}
[[nodiscard]] constexpr RightMask operator|(RightMask a, Right b) noexcept {
    return static_cast<RightMask>(a | static_cast<RightMask>(b));
}

[[nodiscard]] constexpr bool holds(RightMask mask, Right right) noexcept {
    return (mask & static_cast<RightMask>(right)) != 0;
}

// One mask per role, looked up from the kind at every check rather than copied
// onto a member row. Changing the table changes every existing conversation of
// that kind at the next deploy, which is what a rule should do; a copied mask
// would freeze every conversation at the rights it was created with.
struct RoleRights final {
    RightMask member;
    RightMask admin;
    RightMask owner;
};

// --- the kind -----------------------------------------------------------------------

struct ConversationKindSpec final {
    // Never translated, never parsed from anything but the table lookup.
    std::string_view               key;               // 16
    // The disappearing timers a member may pick, in seconds, strictly
    // ascending. Empty when this kind has none.
    std::span<const std::uint32_t> timers_s;          // 16
    // The account-level authority to CREATE one. Empty means any account that
    // can reach the route. Membership, not this, governs everything after.
    PermSet                        create_requires;   // 16
    std::uint32_t                  max_members;       //  4
    // How long the server holds anything, as a ceiling over every timer. Zero
    // keeps messages until they are revoked or expire.
    std::uint32_t                  retention_days;    //  4
    // Zero disables edits, and delete-for-everyone, respectively.
    std::uint32_t                  edit_window_s;     //  4
    std::uint32_t                  revoke_window_s;   //  4
    // At most kMaxMessageCodePoints; a kind may lower it, never raise it.
    std::uint32_t                  max_text_code_points;  //  4
    RoleRights                     rights;            //  6
    KindCode                       code;              //  1
    Shape                          shape;             //  1
    E2ee                           e2ee;              //  1
    History                        history;           //  1
    Receipts                       receipts;          //  1
    // A mention reaches a member who muted the conversation. Plaintext only:
    // an encrypted mention is inside the ciphertext, where the server cannot
    // read it.
    bool                           mentions_break_mute;  //  1
    // Whether staff holding the application's review permission may read a
    // conversation of this kind they are not in, and members may report one of
    // its messages to them (docs/22 §9.2). Per KIND, so the choice sits in the
    // table where every reviewer of the product can see it, and a kind the
    // product promised was private cannot be read by any permission at all.
    // Never with E2ee::Required: the server holds nothing it could show; an
    // Optional kind's encrypted conversations are refused one by one.
    // Defaulted, so an application that declares nothing reviews nothing.
    bool                           reviewable{false};    //  1
    // Where plaintext attachments go, or nothing for a kind that takes none.
    // Must be a Private namespace that does not deduplicate across owners, and
    // must not take the Sealed class.
    std::optional<fs::Ns>          media_ns;          //  2
    // Where encrypted blobs go (docs/22 §6.4). Required exactly when e2ee is
    // not Never, and must accept exactly fs::kSealedMimes.
    std::optional<fs::Ns>          sealed_ns;         //  2
};

// 88, asserted: the table is a handful of rows in .rodata and is never copied
// per request, so the bound is about a field added carelessly, not about cache
// lines.
static_assert(sizeof(ConversationKindSpec) == 88, "ConversationKindSpec must not grow padding");

// --- lookups ----------------------------------------------------------------------

[[nodiscard]] constexpr RightMask rights_of(const ConversationKindSpec& kind, Role role) noexcept {
    switch (role) {
        case Role::Member: return kind.rights.member;
        case Role::Admin:  return kind.rights.admin;
        case Role::Owner:  return kind.rights.owner;
    }
    return 0;
}

[[nodiscard]] constexpr bool role_may(const ConversationKindSpec& kind, Role role,
                                      Right right) noexcept {
    return holds(rights_of(kind, role), right);
}

// The stored code back to a kind, range-checked. A code outside the table is
// corruption or a newer writer, and either way not a kind this build serves.
// The table is indexed by code (kinds_are_well_formed asserts it), so this is
// one comparison.
[[nodiscard]] constexpr std::optional<KindCode> kind_from_stored(
    std::span<const ConversationKindSpec> kinds, std::int32_t stored) noexcept {
    if (stored < 0 || static_cast<std::size_t>(stored) >= kinds.size()) { return std::nullopt; }
    return static_cast<KindCode>(stored);
}

// A request's kind key back to a code, or nullopt. The only place a request's
// spelling of a kind is ever compared.
[[nodiscard]] constexpr std::optional<KindCode> kind_from_key(
    std::span<const ConversationKindSpec> kinds, std::string_view key) noexcept {
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        if (kinds[i].key == key) { return static_cast<KindCode>(i); }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool timer_allowed(const ConversationKindSpec& kind,
                                           std::uint32_t seconds) noexcept {
    if (seconds == 0) { return true; }   // turning the timer off is always allowed
    for (const std::uint32_t allowed : kind.timers_s) {
        if (allowed == seconds) { return true; }
    }
    return false;
}

// --- table conformance ----------------------------------------------------------

// Lowercase ASCII letters, digits and underscores, 1–32 bytes, starting with a
// letter. A key appears in the descriptor, in request bodies and in log lines,
// and a rule this narrow means none of them has to escape it.
[[nodiscard]] constexpr bool is_wellformed_kind_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > 32) { return false; }
    if (key[0] < 'a' || key[0] > 'z') { return false; }
    for (const char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) { return false; }
    }
    return true;
}

namespace detail {

// A namespace chat media may live in: private, so only a grant serves it, and
// not deduplicated across owners, so a fast upload says nothing about anybody
// else's files (docs/22 §6.1, §6.3).
[[nodiscard]] constexpr bool is_private_media_namespace(fs::Ns ns) noexcept {
    return ns.visibility() == fs::Visibility::Private && ns.dedupe() != fs::Dedupe::Namespace;
}

[[nodiscard]] constexpr bool rights_are_well_formed(const ConversationKindSpec& kind) noexcept {
    const RoleRights& r = kind.rights;
    const auto subset = [](RightMask lower, RightMask upper) {
        return (lower & static_cast<RightMask>(~upper)) == 0;
    };
    if (((r.member | r.admin | r.owner) & static_cast<RightMask>(~kAllRights)) != 0) {
        return false;
    }
    // Monotone: a member who can do what an admin cannot is a table with two
    // names swapped.
    return subset(r.member, r.admin) && subset(r.admin, r.owner);
}

[[nodiscard]] constexpr bool timers_are_well_formed(const ConversationKindSpec& kind) noexcept {
    std::uint32_t previous = 0;
    for (const std::uint32_t seconds : kind.timers_s) {
        // Strictly ascending and non-zero: zero is "off", which is always
        // allowed and is not a timer to list.
        if (seconds <= previous || seconds > kMaxWindowSeconds) { return false; }
        previous = seconds;
    }
    // Retention is a ceiling over every timer. A timer longer than retention
    // promises a lifetime the server will not keep.
    if (kind.retention_days != 0 && previous != 0 &&
        std::uint64_t{previous} > std::uint64_t{kind.retention_days} * 86'400U) {
        return false;
    }
    return true;
}

[[nodiscard]] constexpr bool shape_is_well_formed(const ConversationKindSpec& kind) noexcept {
    const RightMask everyone = kind.rights.member | kind.rights.admin | kind.rights.owner;
    switch (kind.shape) {
        case Shape::Direct: {
            // Two people, fixed at creation. Nobody adds, removes, promotes or
            // invites, or the conversation stops being between those two.
            constexpr RightMask kMembership =
                Right::AddMember | Right::RemoveMember | Right::ManageAdmins |
                Right::CreateInvite;
            return kind.max_members == 2 && (everyone & kMembership) == 0;
        }
        case Shape::Group:
            return kind.max_members >= 2 && kind.max_members <= kMaxMembers;
        case Shape::Channel:
            // The key of an encrypted channel is effectively public, receipts
            // from six figures of followers are a write storm, and a follower
            // who can post is a member of a group.
            return kind.max_members >= 1 && kind.max_members <= kMaxFollowers &&
                   kind.e2ee == E2ee::Never && kind.receipts == Receipts::Off &&
                   !holds(kind.rights.member, Right::Post);
    }
    return false;
}

[[nodiscard]] constexpr bool storage_is_well_formed(const ConversationKindSpec& kind) noexcept {
    if (kind.media_ns.has_value() && !is_private_media_namespace(*kind.media_ns)) {
        return false;
    }
    // Plaintext attachments are sniffed against the closed list; a namespace
    // that takes ciphertext sniffs nothing, so it cannot be where plaintext
    // goes.
    if (kind.media_ns.has_value() && kind.media_ns->sealed()) { return false; }
    if (kind.e2ee == E2ee::Never) { return !kind.sealed_ns.has_value(); }
    // Exactly the sealed class: a namespace that took images would sniff and
    // re-encode ciphertext, which is a decoder pointed at attacker bytes for
    // nothing. Encrypted blobs are unique per upload, so it never
    // deduplicates. fs::namespace_is_well_formed already forces Private and
    // Dedupe::None on any namespace that takes Sealed; the two are restated so
    // this check does not lean on another header's assertion.
    if (!kind.sealed_ns.has_value() || kind.sealed_ns->accepts() != fs::kSealedMimes ||
        kind.sealed_ns->visibility() != fs::Visibility::Private ||
        kind.sealed_ns->dedupe() != fs::Dedupe::None) {
        return false;
    }
    if (kind.media_ns.has_value() && *kind.media_ns == *kind.sealed_ns) { return false; }
    // A joiner cannot decrypt what was sent before they joined, so serving it
    // is bytes they cannot read and metadata they were not owed.
    if (kind.history != History::FromJoin) { return false; }
    // Required means no plaintext conversation of this kind can exist, so a
    // plaintext attachment namespace would be a namespace nothing may use.
    if (kind.e2ee == E2ee::Required && kind.media_ns.has_value()) { return false; }
    return true;
}

}  // namespace detail

// static_assert it over your own table.
[[nodiscard]] constexpr bool kinds_are_well_formed(
    std::span<const ConversationKindSpec> kinds) noexcept {
    if (kinds.empty() || kinds.size() > kMaxKinds) { return false; }
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const ConversationKindSpec& kind = kinds[i];
        // Indexed by code, so the stored byte IS the position: a lookup cannot
        // disagree with the table it looks into.
        if (kind.code != i) { return false; }
        if (!is_wellformed_kind_key(kind.key)) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (kinds[j].key == kind.key) { return false; }
        }
        if (kind.max_text_code_points == 0 || kind.max_text_code_points > kMaxMessageCodePoints) {
            return false;
        }
        if (kind.edit_window_s > kMaxWindowSeconds || kind.revoke_window_s > kMaxWindowSeconds) {
            return false;
        }
        if (!detail::rights_are_well_formed(kind)) { return false; }
        if (!detail::timers_are_well_formed(kind)) { return false; }
        if (!detail::shape_is_well_formed(kind)) { return false; }
        if (!detail::storage_is_well_formed(kind)) { return false; }
        if (kind.reviewable && kind.e2ee == E2ee::Required) { return false; }
    }
    return true;
}

}  // namespace anvil::chat
