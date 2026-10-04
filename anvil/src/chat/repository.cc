// versioned-write-exempt: no chat document is read, modified and written back.
// The conversation's seq and mv move by $inc in the operation that reads them,
// and dsv only ever rises inside one update that reads it on the server;
// receipts move by $max; activity by $max; membership, role, invites and
// reactions are conditional on the state they change in their own filter; and
// title, description, icon, timer and a member's preferences are blind $sets
// whose last writer is meant to win. db/versioned.h guards a staff edit of a
// document somebody else may also be editing, and nothing here is one.

#include "anvil/chat/repository.h"

#include <algorithm>
#include <string>
#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/count.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_delete.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/update.hpp>
#include <mongocxx/pipeline.hpp>

#include "anvil/core/uuid.h"
#include "anvil/crypto/digest.h"
#include "anvil/db/repository.h"

namespace anvil::chat {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace cf = conversation_fields;
namespace mf = member_fields;
namespace gf = message_fields;
namespace rf = reaction_fields;
namespace inf = invite_fields;
namespace bf = block_fields;

[[nodiscard]] bsoncxx::stdx::string_view key(std::string_view name) noexcept {
    return codec::key_of(name);
}

[[nodiscard]] bsoncxx::types::b_int64 i64(std::int64_t value) noexcept {
    return bsoncxx::types::b_int64{value};
}

[[nodiscard]] bsoncxx::types::b_int32 i32(std::int32_t value) noexcept {
    return bsoncxx::types::b_int32{value};
}

// `{field: {$exists: false}}`, the predicate for "still a member" and "never
// revoked". Spelled once.
[[nodiscard]] bsoncxx::document::value absent() {
    return make_document(kvp("$exists", bsoncxx::types::b_bool{false}));
}

// Not expired: either no expiry at all, or one still in the future. A TTL'd
// row is filtered explicitly, and so is a row the sweeper has not reached yet
// (docs/22 §4.7, docs/09-mongodb.md §6).
void append_not_expired_or_timerless(bsoncxx::builder::basic::document& filter, db::TimeMs now) {
    filter.append(kvp("$or", [now](sub_array branches) {
        branches.append(make_document(kvp(key(gf::kExpiresAt), absent())));
        branches.append(
            make_document(kvp(key(gf::kExpiresAt), make_document(kvp("$gt", codec::time_date(now))))));
    }));
}

// `{$add: ["$<field>", 1]}`, an $inc spelled for an update pipeline.
[[nodiscard]] bsoncxx::document::value plus_one(std::string_view field) {
    return make_document(kvp("$add", bsoncxx::builder::basic::make_array(
                                         "$" + std::string{field}, i64(1))));
}

// The device-set version a change at `at` leaves: past both the version and the
// change's own millisecond (ChatRepository::raise_device_sets says why both).
[[nodiscard]] bsoncxx::document::value raised_device_set(db::TimeMs at) {
    return make_document(kvp(
        "$max", bsoncxx::builder::basic::make_array(plus_one(cf::kDeviceSetVersion),
                                                    i64(at.time_since_epoch().count()))));
}

// --- reading optional fields ---------------------------------------------------------
//
// Absent on disk means absent here, never a default that looks like a value.

[[nodiscard]] Result<std::optional<std::int64_t>> read_optional_int64(
    const bsoncxx::document::view& doc, std::string_view field) {
    if (doc.find(key(field)) == doc.end()) { return std::optional<std::int64_t>{}; }
    const Result<std::int64_t> value = codec::read_int64(doc, field);
    if (!value) { return value.error(); }
    return std::optional<std::int64_t>{value.value()};
}

[[nodiscard]] Result<std::string> read_optional_text(const bsoncxx::document::view& doc,
                                                     std::string_view field) {
    if (doc.find(key(field)) == doc.end()) { return std::string{}; }
    const Result<std::string_view> text = codec::read_text(doc, field);
    if (!text) { return text.error(); }
    return std::string{text.value()};
}

[[nodiscard]] Result<bool> read_flag(const bsoncxx::document::view& doc, std::string_view field) {
    if (doc.find(key(field)) == doc.end()) { return false; }
    return codec::read_bool(doc, field);
}

[[nodiscard]] Result<std::optional<db::TimeMs>> read_present_time(
    const bsoncxx::document::view& doc, std::string_view field) {
    if (doc.find(key(field)) == doc.end()) { return std::optional<db::TimeMs>{}; }
    const Result<db::TimeMs> at = codec::read_time(doc, field);
    if (!at) { return at.error(); }
    return std::optional<db::TimeMs>{at.value()};
}

// A uuid that is OMITTED when absent. codec::read_optional_uuid also accepts an
// explicit null, which is harmless here; nothing in this module writes one.
[[nodiscard]] Result<std::optional<Uuid>> read_present_uuid(const bsoncxx::document::view& doc,
                                                            std::string_view field) {
    if (doc.find(key(field)) == doc.end()) { return std::optional<Uuid>{}; }
    return codec::read_optional_uuid(doc, field);
}

// --- conversations ------------------------------------------------------------------

[[nodiscard]] bsoncxx::document::value conversation_document(const ConversationRecord& row,
                                                             bool include_pair) {
    bsoncxx::builder::basic::document doc;
    codec::append_uuid(doc, cf::kId, row.id);
    doc.append(kvp(key(cf::kKind), i32(row.kind)));
    doc.append(kvp(key(cf::kEncrypted), bsoncxx::types::b_bool{row.encrypted}));
    doc.append(kvp(key(cf::kSeq), i64(row.seq)));
    doc.append(kvp(key(cf::kMembershipVersion), i64(row.membership_version)));
    doc.append(kvp(key(cf::kDeviceSetVersion), i64(row.device_set_version)));
    doc.append(kvp(key(cf::kMutations), i64(row.mutations)));
    if (!row.title.empty()) {
        doc.append(kvp(key(cf::kTitle), bsoncxx::types::b_string{key(row.title)}));
    }
    if (!row.description.empty()) {
        doc.append(kvp(key(cf::kDescription), bsoncxx::types::b_string{key(row.description)}));
    }
    if (row.icon.has_value()) { codec::append_uuid(doc, cf::kIcon, *row.icon); }
    doc.append(kvp(key(cf::kTimer), i32(static_cast<std::int32_t>(row.timer_s))));
    codec::append_time(doc, cf::kCreatedAt, row.created_at);
    codec::append_uuid(doc, cf::kCreatedBy, row.created_by);
    // OMITTED off a direct conversation: the unique index is partial on it
    // existing, and every group would otherwise collide on a missing key.
    if (include_pair && row.direct_pair.has_value()) {
        codec::append_digest(doc, cf::kDirectPair, *row.direct_pair);
    }
    if (row.client_id.has_value()) {
        doc.append(kvp(key(cf::kClientId), codec::bytes_bin(*row.client_id)));
    }
    return doc.extract();
}

[[nodiscard]] Result<ConversationRecord> decode_conversation(
    const bsoncxx::document::view& doc, std::span<const ConversationKindSpec> kinds) {
    ConversationRecord out{};
    const Result<Uuid> id = codec::read_uuid(doc, cf::kId);
    if (!id) { return id.error(); }
    out.id = id.value();
    const Result<std::int32_t> kind = codec::read_int32(doc, cf::kKind);
    if (!kind) { return kind.error(); }
    // A code this build does not declare is corruption or a newer writer, and
    // either way not a conversation this build may serve.
    const std::optional<KindCode> code = kind_from_stored(kinds, kind.value());
    if (!code.has_value()) { return fail(ErrorCode::Internal, cf::kKind); }
    out.kind = *code;
    const Result<bool> encrypted = codec::read_bool(doc, cf::kEncrypted);
    if (!encrypted) { return encrypted.error(); }
    out.encrypted = encrypted.value();
    const Result<std::int64_t> seq = codec::read_int64(doc, cf::kSeq);
    if (!seq) { return seq.error(); }
    out.seq = seq.value();
    const Result<std::int64_t> mv = codec::read_int64(doc, cf::kMembershipVersion);
    if (!mv) { return mv.error(); }
    out.membership_version = mv.value();
    const Result<std::int64_t> dsv = codec::read_int64(doc, cf::kDeviceSetVersion);
    if (!dsv) { return dsv.error(); }
    out.device_set_version = dsv.value();
    // Absent on a conversation written before the counter existed, which has
    // had no mutation anybody can catch up on: zero.
    const Result<std::optional<std::int64_t>> mutations = read_optional_int64(doc, cf::kMutations);
    if (!mutations) { return mutations.error(); }
    out.mutations = mutations.value().value_or(0);
    Result<std::string> title = read_optional_text(doc, cf::kTitle);
    if (!title) { return title.error(); }
    out.title = std::move(title).value();
    Result<std::string> description = read_optional_text(doc, cf::kDescription);
    if (!description) { return description.error(); }
    out.description = std::move(description).value();
    const Result<std::optional<Uuid>> icon = read_present_uuid(doc, cf::kIcon);
    if (!icon) { return icon.error(); }
    out.icon = icon.value();
    const Result<std::int32_t> timer = codec::read_int32(doc, cf::kTimer);
    if (!timer) { return timer.error(); }
    if (timer.value() < 0) { return fail(ErrorCode::Internal, cf::kTimer); }
    out.timer_s = static_cast<std::uint32_t>(timer.value());
    const Result<db::TimeMs> at = codec::read_time(doc, cf::kCreatedAt);
    if (!at) { return at.error(); }
    out.created_at = at.value();
    const Result<Uuid> by = codec::read_uuid(doc, cf::kCreatedBy);
    if (!by) { return by.error(); }
    out.created_by = by.value();
    if (doc.find(key(cf::kDirectPair)) != doc.end()) {
        const Result<crypto::Digest256> pair = codec::read_digest(doc, cf::kDirectPair);
        if (!pair) { return pair.error(); }
        out.direct_pair = pair.value();
    }
    return out;
}

[[nodiscard]] mongocxx::options::find_one_and_update allocation_options() {
    mongocxx::options::find_one_and_update options{};
    options.return_document(mongocxx::options::return_document::k_after);
    // What a send needs from the conversation, and nothing else (CLAUDE.md §7).
    options.projection(make_document(
        kvp(key(cf::kSeq), 1), kvp(key(cf::kMembershipVersion), 1),
        kvp(key(cf::kDeviceSetVersion), 1), kvp(key(cf::kMutations), 1), kvp(key(cf::kTimer), 1),
        kvp(key(cf::kKind), 1), kvp(key(cf::kEncrypted), 1)));
    return options;
}

[[nodiscard]] Result<Allocation> decode_allocation(const bsoncxx::document::view& doc,
                                                   std::span<const ConversationKindSpec> kinds) {
    Allocation out{};
    const Result<std::int64_t> seq = codec::read_int64(doc, cf::kSeq);
    if (!seq) { return seq.error(); }
    out.seq = seq.value();
    const Result<std::int64_t> mv = codec::read_int64(doc, cf::kMembershipVersion);
    if (!mv) { return mv.error(); }
    out.membership_version = mv.value();
    const Result<std::int64_t> dsv = codec::read_int64(doc, cf::kDeviceSetVersion);
    if (!dsv) { return dsv.error(); }
    out.device_set_version = dsv.value();
    const Result<std::optional<std::int64_t>> mutations = read_optional_int64(doc, cf::kMutations);
    if (!mutations) { return mutations.error(); }
    out.mutations = mutations.value().value_or(0);
    const Result<std::int32_t> timer = codec::read_int32(doc, cf::kTimer);
    if (!timer) { return timer.error(); }
    if (timer.value() < 0) { return fail(ErrorCode::Internal, cf::kTimer); }
    out.timer_s = static_cast<std::uint32_t>(timer.value());
    const Result<std::int32_t> kind = codec::read_int32(doc, cf::kKind);
    if (!kind) { return kind.error(); }
    const std::optional<KindCode> code = kind_from_stored(kinds, kind.value());
    if (!code.has_value()) { return fail(ErrorCode::Internal, cf::kKind); }
    out.kind = *code;
    const Result<bool> encrypted = codec::read_bool(doc, cf::kEncrypted);
    if (!encrypted) { return encrypted.error(); }
    out.encrypted = encrypted.value();
    return out;
}

// --- members ------------------------------------------------------------------------

[[nodiscard]] bsoncxx::document::value membership_filter(const Uuid& conversation,
                                                         const Uuid& user) {
    return make_document(kvp(key(mf::kConversation), codec::uuid_bin(conversation)),
                         kvp(key(mf::kUser), codec::uuid_bin(user)));
}

[[nodiscard]] bsoncxx::document::value current_membership_filter(const Uuid& conversation,
                                                                 const Uuid& user) {
    return make_document(kvp(key(mf::kConversation), codec::uuid_bin(conversation)),
                         kvp(key(mf::kUser), codec::uuid_bin(user)),
                         kvp(key(mf::kLeftSeq), absent()));
}

[[nodiscard]] bsoncxx::document::value member_document(const MemberRecord& row) {
    bsoncxx::builder::basic::document doc;
    codec::append_uuid(doc, mf::kId, row.id);
    codec::append_uuid(doc, mf::kConversation, row.conversation);
    codec::append_uuid(doc, mf::kUser, row.user);
    codec::append_enum(doc, mf::kRole, row.role);
    doc.append(kvp(key(mf::kJoinedSeq), i64(row.joined_seq)));
    doc.append(kvp(key(mf::kDelivered), i64(row.delivered)));
    doc.append(kvp(key(mf::kRead), i64(row.read)));
    // Always written, so "not private" is an equality the read-by index can
    // serve rather than a missing-or-false the planner cannot bound.
    doc.append(kvp(key(mf::kReadPrivate), bsoncxx::types::b_bool{row.read_private}));
    codec::append_time(doc, mf::kActivity, row.activity);
    // Always written for the same reason: the chat list's equality on `ar`.
    doc.append(kvp(key(mf::kArchived), bsoncxx::types::b_bool{row.archived}));
    doc.append(kvp(key(mf::kHideBefore), i64(row.hide_before)));
    if (row.pinned) { doc.append(kvp(key(mf::kPinned), bsoncxx::types::b_bool{true})); }
    if (row.muted_until.has_value()) { codec::append_time(doc, mf::kMutedUntil, *row.muted_until); }
    return doc.extract();
}

[[nodiscard]] Result<MemberRecord> decode_member(const bsoncxx::document::view& doc) {
    MemberRecord out{};
    const Result<Uuid> id = codec::read_uuid(doc, mf::kId);
    if (!id) { return id.error(); }
    out.id = id.value();
    const Result<Uuid> conversation = codec::read_uuid(doc, mf::kConversation);
    if (!conversation) { return conversation.error(); }
    out.conversation = conversation.value();
    const Result<Uuid> user = codec::read_uuid(doc, mf::kUser);
    if (!user) { return user.error(); }
    out.user = user.value();
    const Result<Role> role = codec::read_enum(doc, mf::kRole, Role::Owner);
    if (!role) { return role.error(); }
    out.role = role.value();
    const Result<std::int64_t> joined = codec::read_int64(doc, mf::kJoinedSeq);
    if (!joined) { return joined.error(); }
    out.joined_seq = joined.value();
    const Result<std::optional<std::int64_t>> left = read_optional_int64(doc, mf::kLeftSeq);
    if (!left) { return left.error(); }
    out.left_seq = left.value();
    const Result<std::optional<std::int64_t>> left_mutations =
        read_optional_int64(doc, mf::kLeftMutations);
    if (!left_mutations) { return left_mutations.error(); }
    out.left_mutations = left_mutations.value();
    const Result<std::int64_t> delivered = codec::read_int64(doc, mf::kDelivered);
    if (!delivered) { return delivered.error(); }
    out.delivered = delivered.value();
    const Result<std::int64_t> read = codec::read_int64(doc, mf::kRead);
    if (!read) { return read.error(); }
    out.read = read.value();
    const Result<bool> read_private = read_flag(doc, mf::kReadPrivate);
    if (!read_private) { return read_private.error(); }
    out.read_private = read_private.value();
    const Result<db::TimeMs> activity = codec::read_time(doc, mf::kActivity);
    if (!activity) { return activity.error(); }
    out.activity = activity.value();
    const Result<std::optional<db::TimeMs>> muted = read_present_time(doc, mf::kMutedUntil);
    if (!muted) { return muted.error(); }
    out.muted_until = muted.value();
    const Result<bool> pinned = read_flag(doc, mf::kPinned);
    if (!pinned) { return pinned.error(); }
    out.pinned = pinned.value();
    const Result<bool> archived = read_flag(doc, mf::kArchived);
    if (!archived) { return archived.error(); }
    out.archived = archived.value();
    const Result<std::int64_t> hide = codec::read_int64(doc, mf::kHideBefore);
    if (!hide) { return hide.error(); }
    out.hide_before = hide.value();
    return out;
}

// --- messages -----------------------------------------------------------------------

void append_mentions(bsoncxx::builder::basic::document& doc,
                     std::span<const MentionSpan> mentions) {
    doc.append(kvp(key(gf::kMentions), [mentions](sub_array rows) {
        for (const MentionSpan& mention : mentions) {
            rows.append([&mention](sub_document row) {
                row.append(kvp(key(gf::kMentionUser), codec::uuid_bin(mention.user)));
                row.append(kvp(key(gf::kMentionOffset),
                               i32(static_cast<std::int32_t>(mention.offset))));
                row.append(kvp(key(gf::kMentionLength),
                               i32(static_cast<std::int32_t>(mention.length))));
            });
        }
    }));
}

[[nodiscard]] bsoncxx::document::value message_document(const MessageRecord& row,
                                                        const Uuid& conversation) {
    bsoncxx::builder::basic::document doc;
    codec::append_uuid(doc, gf::kId, row.id);
    codec::append_uuid(doc, gf::kConversation, conversation);
    doc.append(kvp(key(gf::kSeq), i64(row.seq)));
    codec::append_uuid(doc, gf::kSender, row.sender);
    doc.append(kvp(key(gf::kClientId), codec::bytes_bin(row.client_id)));
    codec::append_enum(doc, gf::kKind, row.kind);
    codec::append_time(doc, gf::kSentAt, row.sent_at);
    // Every optional part is OMITTED rather than written empty: a message is the
    // highest-volume row in the module, and a key with no value is bytes on
    // every one of them.
    if (!row.body.empty()) {
        doc.append(kvp(key(gf::kBody), bsoncxx::types::b_string{key(row.body)}));
    }
    if (!row.attachments.empty()) {
        doc.append(kvp(key(gf::kAttachments), [&row](sub_array rows) {
            for (const AttachmentRecord& attachment : row.attachments) {
                rows.append([&attachment](sub_document entry) {
                    entry.append(kvp(key(gf::kAttMedia), codec::uuid_bin(attachment.media)));
                    entry.append(kvp(key(gf::kAttNamespace), i32(attachment.ns.stored())));
                    entry.append(kvp(key(gf::kAttMime),
                                     i32(static_cast<std::int32_t>(attachment.mime))));
                    entry.append(kvp(key(gf::kAttWidth), i32(attachment.width)));
                    entry.append(kvp(key(gf::kAttHeight), i32(attachment.height)));
                    entry.append(kvp(key(gf::kAttDuration),
                                     i64(static_cast<std::int64_t>(attachment.duration_ms))));
                    if (!attachment.name.empty()) {
                        entry.append(kvp(key(gf::kAttName),
                                         bsoncxx::types::b_string{key(attachment.name)}));
                    }
                });
            }
        }));
    }
    if (!row.mentions.empty()) { append_mentions(doc, row.mentions); }
    if (row.preview.has_value()) {
        const PreviewRecord& preview = *row.preview;
        doc.append(kvp(key(gf::kPreview), [&preview](sub_document sub) {
            sub.append(kvp(key(gf::kPreviewUrl), bsoncxx::types::b_string{key(preview.url)}));
            if (!preview.title.empty()) {
                sub.append(kvp(key(gf::kPreviewTitle), bsoncxx::types::b_string{key(preview.title)}));
            }
            if (!preview.description.empty()) {
                sub.append(kvp(key(gf::kPreviewDescription),
                               bsoncxx::types::b_string{key(preview.description)}));
            }
        }));
    }
    if (row.system.has_value()) {
        const SystemRecord& system = *row.system;
        doc.append(kvp(key(gf::kSystem), [&system](sub_document sub) {
            codec::append_enum(sub, gf::kEvent, system.event);
            if (system.subject.has_value()) {
                codec::append_uuid(sub, gf::kEventSubject, *system.subject);
            }
            codec::append_enum(sub, gf::kEventRole, system.role);
            sub.append(kvp(key(gf::kEventTimer), i32(static_cast<std::int32_t>(system.timer_s))));
        }));
    }
    if (row.ref.has_value()) { doc.append(kvp(key(gf::kRef), i64(*row.ref))); }
    if (row.expires_at.has_value()) { codec::append_time(doc, gf::kExpiresAt, *row.expires_at); }
    if (row.hidden_from_peer) {
        doc.append(kvp(key(gf::kHiddenFromPeer), bsoncxx::types::b_bool{true}));
    }
    if (row.card.has_value()) {
        doc.append(kvp(key(gf::kCardCode), i32(row.card->code)));
        doc.append(kvp(key(gf::kCardBody), bsoncxx::types::b_string{key(row.card->body)}));
    }
    if (!row.ciphertext.empty()) {
        doc.append(kvp(key(gf::kCiphertext),
                       bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                                static_cast<std::uint32_t>(row.ciphertext.size()),
                                                row.ciphertext.data()}));
    }
    if (row.sender_device.has_value()) {
        codec::append_uuid(doc, gf::kSenderDevice, *row.sender_device);
    }
    return doc.extract();
}

[[nodiscard]] Result<std::vector<AttachmentRecord>> decode_attachments(
    const bsoncxx::document::view& doc) {
    std::vector<AttachmentRecord> out;
    auto element = doc.find(key(gf::kAttachments));
    if (element == doc.end()) { return out; }
    if (element->type() != bsoncxx::type::k_array) {
        return fail(ErrorCode::Internal, gf::kAttachments);
    }
    for (const bsoncxx::array::element& entry : element->get_array().value) {
        if (entry.type() != bsoncxx::type::k_document || out.size() >= kMaxAttachments) {
            return fail(ErrorCode::Internal, gf::kAttachments);
        }
        const bsoncxx::document::view row = entry.get_document().value;
        AttachmentRecord attachment{.name = {}, .media = {}, .duration_ms = 0, .width = 0,
                                    .height = 0, .ns = fs::kAllNamespaces[0],
                                    .mime = fs::Mime::Unknown};
        const Result<Uuid> media = codec::read_uuid(row, gf::kAttMedia);
        if (!media) { return media.error(); }
        attachment.media = media.value();
        const Result<std::int32_t> ns = codec::read_int32(row, gf::kAttNamespace);
        if (!ns) { return ns.error(); }
        const std::optional<fs::Ns> stored_ns = fs::Ns::from_stored(ns.value());
        if (!stored_ns.has_value()) { return fail(ErrorCode::Internal, gf::kAttNamespace); }
        attachment.ns = *stored_ns;
        const Result<fs::Mime> mime = codec::read_enum(row, gf::kAttMime, fs::kMaxMime);
        if (!mime) { return mime.error(); }
        attachment.mime = mime.value();
        const Result<std::int32_t> width = codec::read_int32(row, gf::kAttWidth);
        if (!width) { return width.error(); }
        const Result<std::int32_t> height = codec::read_int32(row, gf::kAttHeight);
        if (!height) { return height.error(); }
        if (width.value() < 0 || width.value() > 65535 || height.value() < 0 ||
            height.value() > 65535) {
            return fail(ErrorCode::Internal, gf::kAttWidth);
        }
        attachment.width = static_cast<std::uint16_t>(width.value());
        attachment.height = static_cast<std::uint16_t>(height.value());
        const Result<std::int64_t> duration = codec::read_int64(row, gf::kAttDuration);
        if (!duration) { return duration.error(); }
        if (duration.value() < 0 || duration.value() > std::int64_t{UINT32_MAX}) {
            return fail(ErrorCode::Internal, gf::kAttDuration);
        }
        attachment.duration_ms = static_cast<std::uint32_t>(duration.value());
        Result<std::string> name = read_optional_text(row, gf::kAttName);
        if (!name) { return name.error(); }
        attachment.name = std::move(name).value();
        out.push_back(std::move(attachment));
    }
    return out;
}

[[nodiscard]] Result<std::vector<MentionSpan>> decode_mentions(const bsoncxx::document::view& doc) {
    std::vector<MentionSpan> out;
    auto element = doc.find(key(gf::kMentions));
    if (element == doc.end()) { return out; }
    if (element->type() != bsoncxx::type::k_array) {
        return fail(ErrorCode::Internal, gf::kMentions);
    }
    for (const bsoncxx::array::element& entry : element->get_array().value) {
        if (entry.type() != bsoncxx::type::k_document || out.size() >= kMaxMentions) {
            return fail(ErrorCode::Internal, gf::kMentions);
        }
        const bsoncxx::document::view row = entry.get_document().value;
        const Result<Uuid> user = codec::read_uuid(row, gf::kMentionUser);
        if (!user) { return user.error(); }
        const Result<std::int32_t> offset = codec::read_int32(row, gf::kMentionOffset);
        if (!offset) { return offset.error(); }
        const Result<std::int32_t> length = codec::read_int32(row, gf::kMentionLength);
        if (!length) { return length.error(); }
        if (offset.value() < 0 || length.value() <= 0) {
            return fail(ErrorCode::Internal, gf::kMentions);
        }
        out.push_back(MentionSpan{user.value(), static_cast<std::uint32_t>(offset.value()),
                                  static_cast<std::uint32_t>(length.value())});
    }
    return out;
}

[[nodiscard]] Result<MessageRecord> decode_message(const bsoncxx::document::view& doc) {
    MessageRecord out{};
    const Result<Uuid> id = codec::read_uuid(doc, gf::kId);
    if (!id) { return id.error(); }
    out.id = id.value();
    const Result<Uuid> conversation = codec::read_uuid(doc, gf::kConversation);
    if (!conversation) { return conversation.error(); }
    out.conversation = conversation.value();
    const Result<std::int64_t> seq = codec::read_int64(doc, gf::kSeq);
    if (!seq) { return seq.error(); }
    out.seq = seq.value();
    const Result<Uuid> sender = codec::read_uuid(doc, gf::kSender);
    if (!sender) { return sender.error(); }
    out.sender = sender.value();
    if (const Status cid = codec::read_bytes(doc, gf::kClientId, out.client_id); !cid) {
        return cid.error();
    }
    const Result<MessageKind> kind = codec::read_enum(doc, gf::kKind, kMaxMessageKind);
    if (!kind) { return kind.error(); }
    out.kind = kind.value();
    const Result<db::TimeMs> at = codec::read_time(doc, gf::kSentAt);
    if (!at) { return at.error(); }
    out.sent_at = at.value();
    Result<std::string> body = read_optional_text(doc, gf::kBody);
    if (!body) { return body.error(); }
    out.body = std::move(body).value();
    Result<std::vector<AttachmentRecord>> attachments = decode_attachments(doc);
    if (!attachments) { return attachments.error(); }
    out.attachments = std::move(attachments).value();
    Result<std::vector<MentionSpan>> mentions = decode_mentions(doc);
    if (!mentions) { return mentions.error(); }
    out.mentions = std::move(mentions).value();

    if (auto preview = doc.find(key(gf::kPreview)); preview != doc.end()) {
        if (preview->type() != bsoncxx::type::k_document) {
            return fail(ErrorCode::Internal, gf::kPreview);
        }
        const bsoncxx::document::view sub = preview->get_document().value;
        const Result<std::string_view> url = codec::read_text(sub, gf::kPreviewUrl);
        if (!url) { return url.error(); }
        Result<std::string> title = read_optional_text(sub, gf::kPreviewTitle);
        if (!title) { return title.error(); }
        Result<std::string> description = read_optional_text(sub, gf::kPreviewDescription);
        if (!description) { return description.error(); }
        out.preview = PreviewRecord{std::string{url.value()}, std::move(title).value(),
                                    std::move(description).value()};
    }
    if (auto system = doc.find(key(gf::kSystem)); system != doc.end()) {
        if (system->type() != bsoncxx::type::k_document) {
            return fail(ErrorCode::Internal, gf::kSystem);
        }
        const bsoncxx::document::view sub = system->get_document().value;
        const Result<SystemEvent> event = codec::read_enum(sub, gf::kEvent, kMaxSystemEvent);
        if (!event) { return event.error(); }
        const Result<std::optional<Uuid>> subject = read_present_uuid(sub, gf::kEventSubject);
        if (!subject) { return subject.error(); }
        const Result<Role> role = codec::read_enum(sub, gf::kEventRole, Role::Owner);
        if (!role) { return role.error(); }
        const Result<std::int32_t> timer = codec::read_int32(sub, gf::kEventTimer);
        if (!timer) { return timer.error(); }
        if (timer.value() < 0) { return fail(ErrorCode::Internal, gf::kEventTimer); }
        out.system = SystemRecord{subject.value(), static_cast<std::uint32_t>(timer.value()),
                                  event.value(), role.value()};
    }
    const Result<std::optional<std::int64_t>> ref = read_optional_int64(doc, gf::kRef);
    if (!ref) { return ref.error(); }
    out.ref = ref.value();
    const Result<bool> revoked = read_flag(doc, gf::kRevoked);
    if (!revoked) { return revoked.error(); }
    out.revoked = revoked.value();
    const Result<bool> hidden = read_flag(doc, gf::kHiddenFromPeer);
    if (!hidden) { return hidden.error(); }
    out.hidden_from_peer = hidden.value();
    if (doc.find(key(gf::kCardCode)) != doc.end()) {
        const Result<std::int32_t> code = codec::read_int32(doc, gf::kCardCode);
        if (!code) { return code.error(); }
        if (code.value() < 0 || code.value() > 255) { return fail(ErrorCode::Internal, gf::kCardCode); }
        const Result<std::string_view> card_body = codec::read_text(doc, gf::kCardBody);
        if (!card_body) { return card_body.error(); }
        out.card = CardRecord{std::string{card_body.value()},
                              static_cast<std::uint8_t>(code.value())};
    }
    if (doc.find(key(gf::kEdits)) != doc.end()) {
        const Result<std::int32_t> edits = codec::read_int32(doc, gf::kEdits);
        if (!edits) { return edits.error(); }
        out.edits = edits.value();
    }
    const Result<std::optional<db::TimeMs>> edited = read_present_time(doc, gf::kEditedAt);
    if (!edited) { return edited.error(); }
    out.edited_at = edited.value();
    const Result<std::optional<db::TimeMs>> expires = read_present_time(doc, gf::kExpiresAt);
    if (!expires) { return expires.error(); }
    out.expires_at = expires.value();
    if (auto ciphertext = doc.find(key(gf::kCiphertext)); ciphertext != doc.end()) {
        // Bounded as it was on the way in: a longer one is corruption or a
        // writer that is not this library, and either way not something to
        // serve to every member's device.
        if (ciphertext->type() != bsoncxx::type::k_binary ||
            ciphertext->get_binary().size > kMaxCiphertextBytes) {
            return fail(ErrorCode::Internal, gf::kCiphertext);
        }
        const bsoncxx::types::b_binary bin = ciphertext->get_binary();
        out.ciphertext.assign(bin.bytes, bin.bytes + bin.size);
    }
    const Result<std::optional<Uuid>> device = read_present_uuid(doc, gf::kSenderDevice);
    if (!device) { return device.error(); }
    out.sender_device = device.value();
    const Result<std::optional<std::int64_t>> mutation = read_optional_int64(doc, gf::kMutation);
    if (!mutation) { return mutation.error(); }
    out.mutation = mutation.value().value_or(0);
    return out;
}

// Who sent a message a mutation changed, and whether it is shown past a block,
// read from the projection of the write that changed it.
[[nodiscard]] Result<std::optional<Mutated>> mutated_of(const bsoncxx::document::view& doc) {
    const Result<Uuid> sender = codec::read_uuid(doc, gf::kSender);
    if (!sender) { return sender.error(); }
    const Result<bool> hidden = read_flag(doc, gf::kHiddenFromPeer);
    if (!hidden) { return hidden.error(); }
    return std::optional<Mutated>{Mutated{sender.value(), hidden.value()}};
}

}  // namespace

crypto::Digest256 direct_pair_key(const Uuid& a, const Uuid& b, bool encrypted) {
    const bool a_first = std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    const Uuid& low = a_first ? a : b;
    const Uuid& high = a_first ? b : a;
    // "d" is a domain prefix, so this key can never equal a digest some other
    // part of the system computes over the same bytes.
    std::array<std::uint8_t, 1 + 16 + 16 + 1> input{};
    input[0] = 'd';
    std::copy(low.begin(), low.end(), input.begin() + 1);
    std::copy(high.begin(), high.end(), input.begin() + 17);
    input[33] = encrypted ? 1U : 0U;
    return crypto::sha256(input);
}

ChatRepository::ChatRepository(const db::DatabaseNames& databases,
                               const ChatCollections& collections,
                               std::span<const ConversationKindSpec> kinds)
    : databases_{std::string{databases.for_collection(collections.conversations)},
                 std::string{databases.for_collection(collections.members)},
                 std::string{databases.for_collection(collections.messages)},
                 std::string{databases.for_collection(collections.reactions)},
                 std::string{databases.for_collection(collections.invites)},
                 std::string{databases.for_collection(collections.blocks)},
                 collections.reports.empty()
                     ? std::string{}
                     : std::string{databases.for_collection(collections.reports)}},
      collections_{collections},
      kinds_{kinds} {}

mongocxx::collection ChatRepository::conversations(mongocxx::client& client) const {
    return client[databases_[0]][std::string{collections_.conversations}];
}
mongocxx::collection ChatRepository::members(mongocxx::client& client) const {
    return client[databases_[1]][std::string{collections_.members}];
}
mongocxx::collection ChatRepository::messages(mongocxx::client& client) const {
    return client[databases_[2]][std::string{collections_.messages}];
}
mongocxx::collection ChatRepository::reactions_collection(mongocxx::client& client) const {
    return client[databases_[3]][std::string{collections_.reactions}];
}
mongocxx::collection ChatRepository::invites(mongocxx::client& client) const {
    return client[databases_[4]][std::string{collections_.invites}];
}
mongocxx::collection ChatRepository::blocks(mongocxx::client& client) const {
    return client[databases_[5]][std::string{collections_.blocks}];
}
mongocxx::collection ChatRepository::reports(mongocxx::client& client) const {
    return client[databases_[6]][std::string{collections_.reports}];
}

namespace {

namespace pf = report_fields;

[[nodiscard]] Result<ReportRecord> decode_report(const bsoncxx::document::view& doc) {
    ReportRecord out{};
    const Result<Uuid> id = codec::read_uuid(doc, pf::kId);
    if (!id) { return id.error(); }
    out.id = id.value();
    const Result<Uuid> conversation = codec::read_uuid(doc, pf::kConversation);
    if (!conversation) { return conversation.error(); }
    out.conversation = conversation.value();
    const Result<Uuid> reporter = codec::read_uuid(doc, pf::kReporter);
    if (!reporter) { return reporter.error(); }
    out.reporter = reporter.value();
    const Result<std::int64_t> from = codec::read_int64(doc, pf::kFrom);
    if (!from) { return from.error(); }
    out.from = from.value();
    const Result<std::int64_t> to = codec::read_int64(doc, pf::kTo);
    if (!to) { return to.error(); }
    out.to = to.value();
    Result<std::string> note = read_optional_text(doc, pf::kNote);
    if (!note) { return note.error(); }
    out.note = std::move(note).value();
    const Result<db::TimeMs> at = codec::read_time(doc, pf::kAt);
    if (!at) { return at.error(); }
    out.at = at.value();
    return out;
}

}  // namespace

Result<std::pair<ReportRecord, bool>> ChatRepository::file_report(mongocxx::client& client,
                                                                  const ReportRecord& row) const {
    const auto range = [&]() {
        return make_document(kvp(key(pf::kConversation), codec::uuid_bin(row.conversation)),
                             kvp(key(pf::kReporter), codec::uuid_bin(row.reporter)),
                             kvp(key(pf::kFrom), i64(row.from)), kvp(key(pf::kTo), i64(row.to)));
    };
    const auto existing = [&]() -> Result<std::pair<ReportRecord, bool>> {
        return repo::guarded([&]() -> Result<std::pair<ReportRecord, bool>> {
            const auto found = reports(client).find_one(range().view());
            if (!found) { return fail(ErrorCode::Internal, pf::kId); }
            Result<ReportRecord> decoded = decode_report(found->view());
            if (!decoded) { return decoded.error(); }
            return std::pair<ReportRecord, bool>{std::move(decoded).value(), false};
        });
    };
    const Status inserted = repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document doc;
        codec::append_uuid(doc, pf::kId, row.id);
        codec::append_uuid(doc, pf::kConversation, row.conversation);
        codec::append_uuid(doc, pf::kReporter, row.reporter);
        doc.append(kvp(key(pf::kFrom), i64(row.from)));
        doc.append(kvp(key(pf::kTo), i64(row.to)));
        if (!row.note.empty()) {
            doc.append(kvp(key(pf::kNote), bsoncxx::types::b_string{key(row.note)}));
        }
        codec::append_time(doc, pf::kAt, row.at);
        reports(client).insert_one(doc.view());
        return ok();
    });
    if (!inserted) {
        if (inserted.code() == ErrorCode::Conflict) { return existing(); }
        return inserted.error();
    }
    return std::pair<ReportRecord, bool>{row, true};
}

Result<std::vector<ReportRecord>> ChatRepository::list_reports(mongocxx::client& client,
                                                               const std::optional<Uuid>& after,
                                                               std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<ReportRecord>> {
        bsoncxx::builder::basic::document filter;
        if (after.has_value()) {
            filter.append(kvp(key(pf::kId), make_document(kvp("$lt", codec::uuid_bin(*after)))));
        }
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(pf::kId), -1)));
        options.limit(limit > 0 ? limit : 1);
        std::vector<ReportRecord> out;
        for (const bsoncxx::document::view doc : reports(client).find(filter.view(), options)) {
            Result<ReportRecord> decoded = decode_report(doc);
            if (!decoded) { return decoded.error(); }
            out.push_back(std::move(decoded).value());
        }
        return out;
    });
}

// --- conversations ------------------------------------------------------------------

Status ChatRepository::insert_conversation(mongocxx::client& client,
                                           mongocxx::client_session& session,
                                           const ConversationRecord& row) const {
    return repo::guarded_in_transaction([&]() -> Status {
        conversations(client).insert_one(session, conversation_document(row, true).view());
        return ok();
    });
}

Result<std::pair<ConversationRecord, bool>> ChatRepository::find_or_insert_direct(
    mongocxx::client& client, const ConversationRecord& row) const {
    if (!row.direct_pair.has_value()) { return fail(ErrorCode::Internal, cf::kDirectPair); }
    const auto attempt = [&]() -> Result<std::pair<ConversationRecord, bool>> {
        mongocxx::options::find_one_and_update options{};
        options.upsert(true);
        options.return_document(mongocxx::options::return_document::k_after);
        // The pair key is the equality, so the server writes it into an inserted
        // document itself; naming it in $setOnInsert as well would be the same
        // path twice in one update.
        const auto found = conversations(client).find_one_and_update(
            make_document(kvp(key(cf::kDirectPair), codec::digest_bin(*row.direct_pair))).view(),
            make_document(kvp("$setOnInsert", conversation_document(row, false))).view(),
            options);
        if (!found) { return fail(ErrorCode::Internal); }
        Result<ConversationRecord> decoded = decode_conversation(found->view(), kinds_);
        if (!decoded) { return decoded.error(); }
        const bool created = decoded.value().id == row.id;
        return std::pair<ConversationRecord, bool>{std::move(decoded).value(), created};
    };
    Result<std::pair<ConversationRecord, bool>> first = repo::guarded(attempt);
    // Two upserts racing on one key: the server inserts one and refuses the
    // other with a duplicate key. The loser's answer is the winner's row, which
    // a second attempt now finds rather than inserts.
    if (!first && first.error().code == ErrorCode::Conflict) { return repo::guarded(attempt); }
    return first;
}

Result<std::optional<ConversationRecord>> ChatRepository::find_conversation(
    mongocxx::client& client, const Uuid& id) const {
    return repo::guarded([&]() -> Result<std::optional<ConversationRecord>> {
        const auto found = conversations(client).find_one(
            make_document(kvp(key(cf::kId), codec::uuid_bin(id))).view());
        if (!found) { return std::optional<ConversationRecord>{}; }
        Result<ConversationRecord> decoded = decode_conversation(found->view(), kinds_);
        if (!decoded) { return decoded.error(); }
        return std::optional<ConversationRecord>{std::move(decoded).value()};
    });
}

Result<std::optional<ConversationRecord>> ChatRepository::find_conversation(
    mongocxx::client& client, mongocxx::client_session& session, const Uuid& id) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<ConversationRecord>> {
        const auto found = conversations(client).find_one(
            session, make_document(kvp(key(cf::kId), codec::uuid_bin(id))).view());
        if (!found) { return std::optional<ConversationRecord>{}; }
        Result<ConversationRecord> decoded = decode_conversation(found->view(), kinds_);
        if (!decoded) { return decoded.error(); }
        return std::optional<ConversationRecord>{std::move(decoded).value()};
    });
}

Result<std::optional<ConversationRecord>> ChatRepository::find_created(
    mongocxx::client& client, const Uuid& creator,
    const std::array<std::uint8_t, 16>& client_id) const {
    return repo::guarded([&]() -> Result<std::optional<ConversationRecord>> {
        const auto found = conversations(client).find_one(
            make_document(kvp(key(cf::kCreatedBy), codec::uuid_bin(creator)),
                          kvp(key(cf::kClientId), codec::bytes_bin(client_id)))
                .view());
        if (!found) { return std::optional<ConversationRecord>{}; }
        Result<ConversationRecord> decoded = decode_conversation(found->view(), kinds_);
        if (!decoded) { return decoded.error(); }
        return std::optional<ConversationRecord>{std::move(decoded).value()};
    });
}

Result<std::vector<ConversationRecord>> ChatRepository::find_conversations(
    mongocxx::client& client, std::span<const Uuid> ids) const {
    return repo::guarded([&]() -> Result<std::vector<ConversationRecord>> {
        std::vector<ConversationRecord> out;
        if (ids.empty()) { return out; }
        out.reserve(ids.size());
        bsoncxx::builder::basic::array in;
        for (const Uuid& id : ids) { in.append(codec::uuid_bin(id)); }
        mongocxx::options::find options{};
        options.limit(static_cast<std::int64_t>(ids.size()));
        for (const bsoncxx::document::view doc : conversations(client).find(
                 make_document(kvp(key(cf::kId), make_document(kvp("$in", in.extract())))).view(),
                 options)) {
            Result<ConversationRecord> decoded = decode_conversation(doc, kinds_);
            if (!decoded) { return decoded.error(); }
            out.push_back(std::move(decoded).value());
        }
        return out;
    });
}

Result<std::optional<Allocation>> ChatRepository::allocate(
    mongocxx::client& client, const Uuid& conversation, std::optional<std::int64_t> fence) const {
    return repo::guarded([&]() -> Result<std::optional<Allocation>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, cf::kId, conversation);
        // The fence is in the FILTER, so a stale sender costs no round trip of
        // its own: it simply matches nothing (docs/22 §7.6).
        if (fence.has_value()) { filter.append(kvp(key(cf::kDeviceSetVersion), i64(*fence))); }
        const auto updated = conversations(client).find_one_and_update(
            filter.view(), make_document(kvp("$inc", make_document(kvp(key(cf::kSeq), i64(1))))).view(),
            allocation_options());
        if (!updated) { return std::optional<Allocation>{}; }
        Result<Allocation> decoded = decode_allocation(updated->view(), kinds_);
        if (!decoded) { return decoded.error(); }
        return std::optional<Allocation>{decoded.value()};
    });
}

Result<std::optional<Allocation>> ChatRepository::allocate_for_membership(
    mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<Allocation>> {
        // A pipeline, because the device set's rise reads the field it writes;
        // seq and mv move by one exactly as the $inc they replaced did.
        mongocxx::pipeline update;
        update.add_fields(make_document(kvp(key(cf::kSeq), plus_one(cf::kSeq)),
                                        kvp(key(cf::kMembershipVersion),
                                            plus_one(cf::kMembershipVersion)),
                                        kvp(key(cf::kDeviceSetVersion), raised_device_set(db::now_ms()))));
        const auto updated = conversations(client).find_one_and_update(
            session, make_document(kvp(key(cf::kId), codec::uuid_bin(conversation))).view(),
            update, allocation_options());
        if (!updated) { return std::optional<Allocation>{}; }
        Result<Allocation> decoded = decode_allocation(updated->view(), kinds_);
        if (!decoded) { return decoded.error(); }
        return std::optional<Allocation>{decoded.value()};
    });
}

Result<std::optional<Allocation>> ChatRepository::allocate_mutation(
    mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<Allocation>> {
        const auto updated = conversations(client).find_one_and_update(
            session, make_document(kvp(key(cf::kId), codec::uuid_bin(conversation))).view(),
            make_document(kvp("$inc", make_document(kvp(key(cf::kMutations), i64(1))))).view(),
            allocation_options());
        if (!updated) { return std::optional<Allocation>{}; }
        Result<Allocation> decoded = decode_allocation(updated->view(), kinds_);
        if (!decoded) { return decoded.error(); }
        return std::optional<Allocation>{decoded.value()};
    });
}

Status ChatRepository::raise_device_sets(mongocxx::client& client,
                                         std::span<const Uuid> ids, db::TimeMs at) const {
    return repo::guarded([&]() -> Status {
        if (ids.empty()) { return ok(); }
        bsoncxx::builder::basic::array in;
        for (const Uuid& id : ids) { in.append(codec::uuid_bin(id)); }
        mongocxx::pipeline update;
        update.add_fields(make_document(kvp(key(cf::kDeviceSetVersion), raised_device_set(at))));
        conversations(client).update_many(
            make_document(kvp(key(cf::kId), make_document(kvp("$in", in.extract())))).view(),
            update);
        return ok();
    });
}

Status ChatRepository::set_info(mongocxx::client& client, mongocxx::client_session& session,
                                const Uuid& conversation, std::optional<std::string_view> title,
                                std::optional<std::string_view> description,
                                std::optional<std::optional<Uuid>> icon) const {
    return repo::guarded_in_transaction([&]() -> Status {
        bsoncxx::builder::basic::document set;
        bsoncxx::builder::basic::document unset;
        bool sets = false;
        bool unsets = false;
        const auto text = [&](std::string_view field, std::optional<std::string_view> value) {
            if (!value.has_value()) { return; }
            if (value->empty()) {
                unset.append(kvp(key(field), ""));
                unsets = true;
            } else {
                set.append(kvp(key(field), bsoncxx::types::b_string{key(*value)}));
                sets = true;
            }
        };
        text(cf::kTitle, title);
        text(cf::kDescription, description);
        if (icon.has_value()) {
            if (icon->has_value()) {
                codec::append_uuid(set, cf::kIcon, **icon);
                sets = true;
            } else {
                unset.append(kvp(key(cf::kIcon), ""));
                unsets = true;
            }
        }
        bsoncxx::builder::basic::document update;
        if (sets) { update.append(kvp("$set", set.extract())); }
        if (unsets) { update.append(kvp("$unset", unset.extract())); }
        if (!sets && !unsets) { return ok(); }
        const auto result = conversations(client).update_one(
            session, make_document(kvp(key(cf::kId), codec::uuid_bin(conversation))).view(),
            update.view());
        if (!result.has_value() || result->matched_count() == 0) { return fail(ErrorCode::NotFound); }
        return ok();
    });
}

Status ChatRepository::set_timer(mongocxx::client& client, mongocxx::client_session& session,
                                 const Uuid& conversation, std::uint32_t timer_s) const {
    return repo::guarded_in_transaction([&]() -> Status {
        const auto result = conversations(client).update_one(
            session, make_document(kvp(key(cf::kId), codec::uuid_bin(conversation))).view(),
            make_document(kvp("$set", make_document(kvp(
                                          key(cf::kTimer), i32(static_cast<std::int32_t>(timer_s))))))
                .view());
        if (!result.has_value() || result->matched_count() == 0) { return fail(ErrorCode::NotFound); }
        return ok();
    });
}

// --- members ------------------------------------------------------------------------

Status ChatRepository::join(mongocxx::client& client, mongocxx::client_session& session,
                            const MemberRecord& row) const {
    return repo::guarded_in_transaction([&]() -> Status {
        mongocxx::collection collection = members(client);
        // A PAST membership is revived rather than duplicated: the unique {c, u}
        // index allows one row per person per conversation, and the gap between
        // leaving and rejoining stays invisible because `js` moves forward.
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, mf::kConversation, row.conversation);
        codec::append_uuid(filter, mf::kUser, row.user);
        filter.append(kvp(key(mf::kLeftSeq), make_document(kvp("$exists", bsoncxx::types::b_bool{true}))));
        bsoncxx::builder::basic::document set;
        codec::append_enum(set, mf::kRole, row.role);
        set.append(kvp(key(mf::kJoinedSeq), i64(row.joined_seq)));
        set.append(kvp(key(mf::kDelivered), i64(row.delivered)));
        set.append(kvp(key(mf::kRead), i64(row.read)));
        codec::append_time(set, mf::kActivity, row.activity);
        set.append(kvp(key(mf::kArchived), bsoncxx::types::b_bool{false}));
        const auto revived = collection.update_one(
            session, filter.view(),
            make_document(kvp("$set", set.extract()),
                          kvp("$unset", make_document(kvp(key(mf::kLeftSeq), ""),
                                                      kvp(key(mf::kLeftMutations), ""))))
                .view());
        if (revived.has_value() && revived->matched_count() == 1) { return ok(); }
        // No past row: a first membership. A duplicate key here means a CURRENT
        // membership already exists, which repo::translate reports as Conflict.
        collection.insert_one(session, member_document(row).view());
        return ok();
    });
}

Status ChatRepository::ensure_member(mongocxx::client& client, const MemberRecord& row) const {
    const auto attempt = [&]() -> Status {
        mongocxx::options::update options{};
        options.upsert(true);
        // {c, u} are the equality, so the server writes them itself; the rest
        // only on insert, so a racing second call changes nothing.
        bsoncxx::builder::basic::document on_insert;
        const bsoncxx::document::value full = member_document(row);
        for (const bsoncxx::document::element& element : full.view()) {
            const std::string_view name{element.key().data(), element.key().size()};
            if (name == mf::kConversation || name == mf::kUser) { continue; }
            on_insert.append(kvp(element.key(), element.get_value()));
        }
        members(client).update_one(membership_filter(row.conversation, row.user).view(),
                                   make_document(kvp("$setOnInsert", on_insert.extract())).view(),
                                   options);
        return ok();
    };
    const Status first = repo::guarded(attempt);
    // Two upserts of one absent key race to a duplicate key; the loser's row now
    // exists, which is all this was asked to ensure.
    if (!first && first.code() == ErrorCode::Conflict) { return ok(); }
    return first;
}

Result<std::optional<MemberRecord>> ChatRepository::find_member(mongocxx::client& client,
                                                                const Uuid& conversation,
                                                                const Uuid& user) const {
    return repo::guarded([&]() -> Result<std::optional<MemberRecord>> {
        const auto found = members(client).find_one(membership_filter(conversation, user).view());
        if (!found) { return std::optional<MemberRecord>{}; }
        Result<MemberRecord> decoded = decode_member(found->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<MemberRecord>{decoded.value()};
    });
}

Result<std::optional<MemberRecord>> ChatRepository::find_member(
    mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation,
    const Uuid& user) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<MemberRecord>> {
        const auto found =
            members(client).find_one(session, membership_filter(conversation, user).view());
        if (!found) { return std::optional<MemberRecord>{}; }
        Result<MemberRecord> decoded = decode_member(found->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<MemberRecord>{decoded.value()};
    });
}

Result<std::vector<MemberRecord>> ChatRepository::list_members(mongocxx::client& client,
                                                               const Uuid& conversation,
                                                               const std::optional<Uuid>& after,
                                                               std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<MemberRecord>> {
        const std::int32_t bounded = limit > 0 ? limit : 1;
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, mf::kConversation, conversation);
        if (after.has_value()) {
            filter.append(kvp(key(mf::kUser), make_document(kvp("$gt", codec::uuid_bin(*after)))));
        }
        filter.append(kvp(key(mf::kLeftSeq), absent()));
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(mf::kConversation), 1), kvp(key(mf::kUser), 1)));
        options.limit(bounded);
        std::vector<MemberRecord> out;
        out.reserve(static_cast<std::size_t>(bounded));
        for (const bsoncxx::document::view doc : members(client).find(filter.view(), options)) {
            Result<MemberRecord> decoded = decode_member(doc);
            if (!decoded) { return decoded.error(); }
            out.push_back(decoded.value());
        }
        return out;
    });
}

Result<std::vector<ChatRepository::MemberId>> ChatRepository::member_ids(
    mongocxx::client& client, const Uuid& conversation, std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<MemberId>> {
        const std::int32_t bounded = limit > 0 ? limit : 1;
        mongocxx::options::find options{};
        options.limit(bounded);
        options.projection(make_document(kvp(key(mf::kId), 0), kvp(key(mf::kUser), 1),
                                         kvp(key(mf::kJoinedSeq), 1)));
        std::vector<MemberId> out;
        out.reserve(static_cast<std::size_t>(bounded));
        for (const bsoncxx::document::view doc : members(client).find(
                 make_document(kvp(key(mf::kConversation), codec::uuid_bin(conversation)),
                               kvp(key(mf::kLeftSeq), absent()))
                     .view(),
                 options)) {
            const Result<Uuid> user = codec::read_uuid(doc, mf::kUser);
            if (!user) { return user.error(); }
            const Result<std::int64_t> joined = codec::read_int64(doc, mf::kJoinedSeq);
            if (!joined) { return joined.error(); }
            out.push_back(MemberId{user.value(), joined.value()});
        }
        return out;
    });
}

Result<std::int64_t> ChatRepository::count_members(mongocxx::client& client,
                                                   mongocxx::client_session& session,
                                                   const Uuid& conversation,
                                                   std::int64_t ceiling) const {
    return repo::guarded_in_transaction([&]() -> Result<std::int64_t> {
        mongocxx::options::count options{};
        options.limit(ceiling > 0 ? ceiling : 1);
        return members(client).count_documents(
            session,
            make_document(kvp(key(mf::kConversation), codec::uuid_bin(conversation)),
                          kvp(key(mf::kLeftSeq), absent()))
                .view(),
            options);
    });
}

Result<std::int64_t> ChatRepository::count_current(mongocxx::client& client,
                                                   const Uuid& conversation,
                                                   std::span<const Uuid> users) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        if (users.empty()) { return std::int64_t{0}; }
        bsoncxx::builder::basic::array in;
        for (const Uuid& user : users) { in.append(codec::uuid_bin(user)); }
        mongocxx::options::count options{};
        options.limit(static_cast<std::int64_t>(users.size()));
        return members(client).count_documents(
            make_document(kvp(key(mf::kConversation), codec::uuid_bin(conversation)),
                          kvp(key(mf::kUser), make_document(kvp("$in", in.extract()))),
                          kvp(key(mf::kLeftSeq), absent()))
                .view(),
            options);
    });
}

Result<std::vector<Uuid>> ChatRepository::current_among(mongocxx::client& client,
                                                        const Uuid& conversation,
                                                        std::span<const Uuid> users) const {
    return repo::guarded([&]() -> Result<std::vector<Uuid>> {
        std::vector<Uuid> out;
        if (users.empty()) { return out; }
        bsoncxx::builder::basic::array in;
        for (const Uuid& user : users) { in.append(codec::uuid_bin(user)); }
        mongocxx::options::find options{};
        options.limit(static_cast<std::int64_t>(users.size()));
        options.projection(make_document(kvp(key(mf::kId), 0), kvp(key(mf::kUser), 1)));
        out.reserve(users.size());
        for (const bsoncxx::document::view doc : members(client).find(
                 make_document(kvp(key(mf::kConversation), codec::uuid_bin(conversation)),
                               kvp(key(mf::kUser), make_document(kvp("$in", in.extract()))),
                               kvp(key(mf::kLeftSeq), absent()))
                     .view(),
                 options)) {
            const Result<Uuid> user = codec::read_uuid(doc, mf::kUser);
            if (!user) { return user.error(); }
            out.push_back(user.value());
        }
        return out;
    });
}

Result<bool> ChatRepository::leave(mongocxx::client& client, mongocxx::client_session& session,
                                   const Uuid& conversation, const Uuid& user,
                                   std::int64_t left_seq, std::int64_t left_mutations) const {
    return repo::guarded_in_transaction([&]() -> Result<bool> {
        const auto result = members(client).update_one(
            session, current_membership_filter(conversation, user).view(),
            make_document(kvp("$set", make_document(kvp(key(mf::kLeftSeq), i64(left_seq)),
                                                    kvp(key(mf::kLeftMutations),
                                                        i64(left_mutations)))))
                .view());
        return result.has_value() && result->matched_count() == 1;
    });
}

Result<bool> ChatRepository::set_role(mongocxx::client& client, mongocxx::client_session& session,
                                      const Uuid& conversation, const Uuid& user,
                                      Role role) const {
    return repo::guarded_in_transaction([&]() -> Result<bool> {
        bsoncxx::builder::basic::document set;
        codec::append_enum(set, mf::kRole, role);
        const auto result = members(client).update_one(
            session, current_membership_filter(conversation, user).view(),
            make_document(kvp("$set", set.extract())).view());
        return result.has_value() && result->matched_count() == 1;
    });
}

Result<std::optional<MemberRecord>> ChatRepository::senior(mongocxx::client& client,
                                                           mongocxx::client_session& session,
                                                           const Uuid& conversation,
                                                           Role role) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<MemberRecord>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, mf::kConversation, conversation);
        codec::append_enum(filter, mf::kRole, role);
        filter.append(kvp(key(mf::kLeftSeq), absent()));
        mongocxx::options::find options{};
        // By joined seq, so the choice is deterministic and a retried
        // transaction promotes the same person (docs/22 §3.4).
        options.sort(make_document(kvp(key(mf::kConversation), 1), kvp(key(mf::kRole), 1),
                                   kvp(key(mf::kJoinedSeq), 1)));
        const auto found = members(client).find_one(session, filter.view(), options);
        if (!found) { return std::optional<MemberRecord>{}; }
        Result<MemberRecord> decoded = decode_member(found->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<MemberRecord>{decoded.value()};
    });
}

Result<std::int64_t> ChatRepository::count_role(mongocxx::client& client,
                                                mongocxx::client_session& session,
                                                const Uuid& conversation, Role role) const {
    return repo::guarded_in_transaction([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, mf::kConversation, conversation);
        codec::append_enum(filter, mf::kRole, role);
        filter.append(kvp(key(mf::kLeftSeq), absent()));
        mongocxx::options::count options{};
        options.limit(2);
        return members(client).count_documents(session, filter.view(), options);
    });
}

Result<std::optional<MemberRecord>> ChatRepository::advance(mongocxx::client& client,
                                                            const Uuid& conversation,
                                                            const Uuid& user,
                                                            std::int64_t delivered,
                                                            std::int64_t read) const {
    return repo::guarded([&]() -> Result<std::optional<MemberRecord>> {
        mongocxx::options::find_one_and_update options{};
        options.return_document(mongocxx::options::return_document::k_after);
        const auto updated = members(client).find_one_and_update(
            current_membership_filter(conversation, user).view(),
            make_document(kvp("$max", make_document(kvp(key(mf::kDelivered), i64(delivered)),
                                                    kvp(key(mf::kRead), i64(read)))))
                .view(),
            options);
        if (!updated) { return std::optional<MemberRecord>{}; }
        Result<MemberRecord> decoded = decode_member(updated->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<MemberRecord>{decoded.value()};
    });
}

Result<bool> ChatRepository::set_preferences(mongocxx::client& client, const Uuid& conversation,
                                             const Uuid& user,
                                             const Preferences& preferences) const {
    return repo::guarded([&]() -> Result<bool> {
        bsoncxx::builder::basic::document set;
        bsoncxx::builder::basic::document unset;
        bool sets = false;
        bool unsets = false;
        if (preferences.muted_until.has_value()) {
            if (preferences.muted_until->has_value()) {
                codec::append_time(set, mf::kMutedUntil, **preferences.muted_until);
                sets = true;
            } else {
                unset.append(kvp(key(mf::kMutedUntil), ""));
                unsets = true;
            }
        }
        if (preferences.pinned.has_value()) {
            // OMITTED when unpinned, so the pin index holds pinned rows only.
            if (*preferences.pinned) {
                set.append(kvp(key(mf::kPinned), bsoncxx::types::b_bool{true}));
                sets = true;
            } else {
                unset.append(kvp(key(mf::kPinned), ""));
                unsets = true;
            }
        }
        if (preferences.archived.has_value()) {
            set.append(kvp(key(mf::kArchived), bsoncxx::types::b_bool{*preferences.archived}));
            sets = true;
        }
        if (preferences.read_private.has_value()) {
            set.append(kvp(key(mf::kReadPrivate), bsoncxx::types::b_bool{*preferences.read_private}));
            sets = true;
        }
        bsoncxx::builder::basic::document update;
        if (sets) { update.append(kvp("$set", set.extract())); }
        if (unsets) { update.append(kvp("$unset", unset.extract())); }
        // "Clear chat" only ever moves forward: an older value from a stale tab
        // must not un-hide what a newer one hid.
        if (preferences.hide_before.has_value()) {
            update.append(kvp("$max", make_document(kvp(key(mf::kHideBefore),
                                                        i64(*preferences.hide_before)))));
        }
        if (!sets && !unsets && !preferences.hide_before.has_value()) { return true; }
        const auto result = members(client).update_one(
            membership_filter(conversation, user).view(), update.view());
        return result.has_value() && result->matched_count() == 1;
    });
}

Status ChatRepository::bump_activity(mongocxx::client& client, const Uuid& conversation,
                                     db::TimeMs at) const {
    return repo::guarded([&]() -> Status {
        members(client).update_many(
            make_document(kvp(key(mf::kConversation), codec::uuid_bin(conversation)),
                          kvp(key(mf::kLeftSeq), absent()))
                .view(),
            make_document(kvp("$max", make_document(kvp(key(mf::kActivity), codec::time_date(at)))))
                .view());
        return ok();
    });
}

Result<std::vector<MemberRecord>> ChatRepository::list_memberships(
    mongocxx::client& client, const Uuid& user, bool archived,
    const std::optional<std::pair<db::TimeMs, Uuid>>& after, std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<MemberRecord>> {
        const std::int32_t bounded = limit > 0 ? limit : 1;
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, mf::kUser, user);
        filter.append(kvp(key(mf::kArchived), bsoncxx::types::b_bool{archived}));
        if (after.has_value()) {
            const db::TimeMs at = after->first;
            const Uuid id = after->second;
            // The compound cursor, descending: two conversations bumped in the
            // same millisecond are common in a busy account, and an instant-only
            // cursor would serve one twice and skip the other.
            filter.append(kvp("$or", [at, id](sub_array branches) {
                branches.append(make_document(
                    kvp(key(mf::kActivity), make_document(kvp("$lt", codec::time_date(at))))));
                branches.append(make_document(
                    kvp(key(mf::kActivity), codec::time_date(at)),
                    kvp(key(mf::kId), make_document(kvp("$lt", codec::uuid_bin(id))))));
            }));
        }
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(mf::kUser), 1), kvp(key(mf::kArchived), 1),
                                   kvp(key(mf::kActivity), -1), kvp(key(mf::kId), -1)));
        options.limit(bounded);
        std::vector<MemberRecord> out;
        out.reserve(static_cast<std::size_t>(bounded));
        for (const bsoncxx::document::view doc : members(client).find(filter.view(), options)) {
            Result<MemberRecord> decoded = decode_member(doc);
            if (!decoded) { return decoded.error(); }
            out.push_back(decoded.value());
        }
        return out;
    });
}

Result<std::vector<Uuid>> ChatRepository::memberships_of(mongocxx::client& client,
                                                        const Uuid& user,
                                                        const std::optional<Uuid>& after,
                                                        std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<Uuid>> {
        const std::int32_t bounded = limit > 0 ? limit : 1;
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, mf::kUser, user);
        if (after.has_value()) {
            filter.append(
                kvp(key(mf::kConversation), make_document(kvp("$gt", codec::uuid_bin(*after)))));
        }
        filter.append(kvp(key(mf::kLeftSeq), absent()));
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(mf::kUser), 1), kvp(key(mf::kConversation), 1)));
        options.projection(make_document(kvp(key(mf::kId), 0), kvp(key(mf::kConversation), 1)));
        options.limit(bounded);
        std::vector<Uuid> out;
        out.reserve(static_cast<std::size_t>(bounded));
        for (const bsoncxx::document::view doc : members(client).find(filter.view(), options)) {
            const Result<Uuid> conversation = codec::read_uuid(doc, mf::kConversation);
            if (!conversation) { return conversation.error(); }
            out.push_back(conversation.value());
        }
        return out;
    });
}

Result<std::vector<MemberRecord>> ChatRepository::pinned(mongocxx::client& client,
                                                         const Uuid& user,
                                                         std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<MemberRecord>> {
        mongocxx::options::find options{};
        options.limit(limit > 0 ? limit : 1);
        std::vector<MemberRecord> out;
        for (const bsoncxx::document::view doc : members(client).find(
                 make_document(kvp(key(mf::kUser), codec::uuid_bin(user)),
                               kvp(key(mf::kPinned), bsoncxx::types::b_bool{true}))
                     .view(),
                 options)) {
            Result<MemberRecord> decoded = decode_member(doc);
            if (!decoded) { return decoded.error(); }
            out.push_back(decoded.value());
        }
        return out;
    });
}

Result<std::vector<Uuid>> ChatRepository::delivered_to(mongocxx::client& client,
                                                       const Uuid& conversation, std::int64_t seq,
                                                       std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<Uuid>> {
        mongocxx::options::find options{};
        options.limit(limit > 0 ? limit : 1);
        options.projection(make_document(kvp(key(mf::kUser), 1)));
        std::vector<Uuid> out;
        for (const bsoncxx::document::view doc : members(client).find(
                 make_document(kvp(key(mf::kConversation), codec::uuid_bin(conversation)),
                               kvp(key(mf::kDelivered), make_document(kvp("$gte", i64(seq)))),
                               kvp(key(mf::kLeftSeq), absent()))
                     .view(),
                 options)) {
            const Result<Uuid> user = codec::read_uuid(doc, mf::kUser);
            if (!user) { return user.error(); }
            out.push_back(user.value());
        }
        return out;
    });
}

Result<std::vector<Uuid>> ChatRepository::read_by(mongocxx::client& client,
                                                  const Uuid& conversation, std::int64_t seq,
                                                  std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<Uuid>> {
        mongocxx::options::find options{};
        options.limit(limit > 0 ? limit : 1);
        options.projection(make_document(kvp(key(mf::kUser), 1)));
        std::vector<Uuid> out;
        for (const bsoncxx::document::view doc : members(client).find(
                 make_document(kvp(key(mf::kConversation), codec::uuid_bin(conversation)),
                               kvp(key(mf::kRead), make_document(kvp("$gte", i64(seq)))),
                               kvp(key(mf::kReadPrivate), bsoncxx::types::b_bool{false}),
                               kvp(key(mf::kLeftSeq), absent()))
                     .view(),
                 options)) {
            const Result<Uuid> user = codec::read_uuid(doc, mf::kUser);
            if (!user) { return user.error(); }
            out.push_back(user.value());
        }
        return out;
    });
}

// --- messages -----------------------------------------------------------------------

Status ChatRepository::insert_message(mongocxx::client& client, mongocxx::client_session& session,
                                      const MessageRecord& row, const Uuid& conversation) const {
    return repo::guarded_in_transaction([&]() -> Status {
        messages(client).insert_one(session, message_document(row, conversation).view());
        return ok();
    });
}

Result<std::optional<MessageRecord>> ChatRepository::find_by_client_id(
    mongocxx::client& client, const Uuid& conversation, const Uuid& sender,
    const std::array<std::uint8_t, 16>& client_id) const {
    return repo::guarded([&]() -> Result<std::optional<MessageRecord>> {
        const auto found = messages(client).find_one(
            make_document(kvp(key(gf::kConversation), codec::uuid_bin(conversation)),
                          kvp(key(gf::kSender), codec::uuid_bin(sender)),
                          kvp(key(gf::kClientId), codec::bytes_bin(client_id)))
                .view());
        if (!found) { return std::optional<MessageRecord>{}; }
        Result<MessageRecord> decoded = decode_message(found->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<MessageRecord>{std::move(decoded).value()};
    });
}

Result<std::optional<MessageRecord>> ChatRepository::find_message(mongocxx::client& client,
                                                                  const Uuid& conversation,
                                                                  std::int64_t seq,
                                                                  db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::optional<MessageRecord>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, gf::kConversation, conversation);
        filter.append(kvp(key(gf::kSeq), i64(seq)));
        append_not_expired_or_timerless(filter, now);
        const auto found = messages(client).find_one(filter.view());
        if (!found) { return std::optional<MessageRecord>{}; }
        Result<MessageRecord> decoded = decode_message(found->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<MessageRecord>{std::move(decoded).value()};
    });
}

Result<std::optional<MessageRecord>> ChatRepository::find_message(
    mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation,
    std::int64_t seq, db::TimeMs now) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<MessageRecord>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, gf::kConversation, conversation);
        filter.append(kvp(key(gf::kSeq), i64(seq)));
        append_not_expired_or_timerless(filter, now);
        const auto found = messages(client).find_one(session, filter.view());
        if (!found) { return std::optional<MessageRecord>{}; }
        Result<MessageRecord> decoded = decode_message(found->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<MessageRecord>{std::move(decoded).value()};
    });
}

Result<HistoryPage> ChatRepository::history(mongocxx::client& client, const Uuid& conversation,
                                            std::int64_t low, std::int64_t high,
                                            std::int32_t limit, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<HistoryPage> {
        HistoryPage page{};
        const std::int32_t bounded = limit > 0 ? limit : 1;
        if (high <= low) { return page; }
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, gf::kConversation, conversation);
        filter.append(kvp(key(gf::kSeq), make_document(kvp("$gte", i64(low)), kvp("$lt", i64(high)))));
        append_not_expired_or_timerless(filter, now);
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(gf::kConversation), 1), kvp(key(gf::kSeq), -1)));
        // One more than the page, so "is there anything older" is answered by
        // the page itself rather than by a count.
        options.limit(bounded + 1);
        for (const bsoncxx::document::view doc : messages(client).find(filter.view(), options)) {
            Result<MessageRecord> decoded = decode_message(doc);
            if (!decoded) { return decoded.error(); }
            page.messages.push_back(std::move(decoded).value());
        }
        if (page.messages.size() > static_cast<std::size_t>(bounded)) {
            page.messages.pop_back();
            page.older = page.messages.back().seq;
        }
        std::reverse(page.messages.begin(), page.messages.end());
        return page;
    });
}

Result<std::vector<MessageRecord>> ChatRepository::after(mongocxx::client& client,
                                                         const Uuid& conversation,
                                                         std::int64_t after_seq,
                                                         std::int64_t high, std::int32_t limit,
                                                         db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::vector<MessageRecord>> {
        std::vector<MessageRecord> out;
        if (high <= after_seq + 1) { return out; }
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, gf::kConversation, conversation);
        filter.append(
            kvp(key(gf::kSeq), make_document(kvp("$gt", i64(after_seq)), kvp("$lt", i64(high)))));
        append_not_expired_or_timerless(filter, now);
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(gf::kConversation), 1), kvp(key(gf::kSeq), 1)));
        options.limit(limit > 0 ? limit : 1);
        for (const bsoncxx::document::view doc : messages(client).find(filter.view(), options)) {
            Result<MessageRecord> decoded = decode_message(doc);
            if (!decoded) { return decoded.error(); }
            out.push_back(std::move(decoded).value());
        }
        return out;
    });
}

Result<std::vector<NudgeMessage>> ChatRepository::recent_for_nudge(mongocxx::client& client,
                                                                  const Uuid& conversation,
                                                                  std::int64_t high,
                                                                  std::int32_t limit,
                                                                  db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::vector<NudgeMessage>> {
        std::vector<NudgeMessage> out;
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, gf::kConversation, conversation);
        filter.append(kvp(key(gf::kSeq), make_document(kvp("$lt", i64(high)))));
        append_not_expired_or_timerless(filter, now);
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(gf::kConversation), 1), kvp(key(gf::kSeq), -1)));
        options.limit(limit > 0 ? limit : 1);
        // The text is cut on the server, in code points: a message can hold
        // thousands of them and a nudge shows a hundred and twenty.
        options.projection(make_document(
            kvp(key(gf::kId), 0), kvp(key(gf::kSeq), 1), kvp(key(gf::kSender), 1),
            kvp(key(gf::kKind), 1), kvp(key(gf::kSentAt), 1), kvp(key(gf::kMentions), 1),
            kvp(key(gf::kRevoked), 1), kvp(key(gf::kHiddenFromPeer), 1),
            kvp(key(gf::kBody),
                make_document(kvp("$substrCP", [](sub_array args) {
                    args.append(make_document(kvp("$ifNull", [](sub_array either) {
                        either.append("$" + std::string{gf::kBody});
                        either.append("");
                    })));
                    args.append(i32(0));
                    args.append(i32(kNudgeTextCodePoints));
                })))));
        out.reserve(static_cast<std::size_t>(limit > 0 ? limit : 1));
        for (const bsoncxx::document::view doc : messages(client).find(filter.view(), options)) {
            NudgeMessage row{};
            const Result<std::int64_t> seq = codec::read_int64(doc, gf::kSeq);
            if (!seq) { return seq.error(); }
            row.seq = seq.value();
            const Result<Uuid> sender = codec::read_uuid(doc, gf::kSender);
            if (!sender) { return sender.error(); }
            row.sender = sender.value();
            const Result<MessageKind> kind = codec::read_enum(doc, gf::kKind, kMaxMessageKind);
            if (!kind) { return kind.error(); }
            row.kind = kind.value();
            const Result<db::TimeMs> at = codec::read_time(doc, gf::kSentAt);
            if (!at) { return at.error(); }
            row.sent_at = at.value();
            Result<std::string> text = read_optional_text(doc, gf::kBody);
            if (!text) { return text.error(); }
            row.text = std::move(text).value();
            const Result<std::vector<MentionSpan>> mentions = decode_mentions(doc);
            if (!mentions) { return mentions.error(); }
            row.mentioned.reserve(mentions.value().size());
            for (const MentionSpan& mention : mentions.value()) {
                row.mentioned.push_back(mention.user);
            }
            const Result<bool> revoked = read_flag(doc, gf::kRevoked);
            if (!revoked) { return revoked.error(); }
            row.revoked = revoked.value();
            const Result<bool> hidden = read_flag(doc, gf::kHiddenFromPeer);
            if (!hidden) { return hidden.error(); }
            row.hidden_from_peer = hidden.value();
            out.push_back(std::move(row));
        }
        return out;
    });
}

Result<std::optional<Mutated>> ChatRepository::edit(
    mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation,
    std::int64_t seq, const Uuid& sender, std::string_view body,
    std::span<const MentionSpan> mentions, db::TimeMs not_before, db::TimeMs at,
    std::int64_t mutation) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<Mutated>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, gf::kConversation, conversation);
        filter.append(kvp(key(gf::kSeq), i64(seq)));
        codec::append_uuid(filter, gf::kSender, sender);
        codec::append_enum(filter, gf::kKind, MessageKind::Text);
        filter.append(kvp(key(gf::kRevoked), absent()));
        filter.append(kvp(key(gf::kSentAt), make_document(kvp("$gte", codec::time_date(not_before)))));

        bsoncxx::builder::basic::document set;
        set.append(kvp(key(gf::kBody), bsoncxx::types::b_string{key(body)}));
        codec::append_time(set, gf::kEditedAt, at);
        set.append(kvp(key(gf::kMutation), i64(mutation)));
        if (!mentions.empty()) { append_mentions(set, mentions); }
        bsoncxx::builder::basic::document update;
        update.append(kvp("$set", set.extract()));
        update.append(kvp("$inc", make_document(kvp(key(gf::kEdits), i32(1)))));
        // An edit that drops every mention must drop the old ones, or a person
        // the author took out stays notified forever.
        if (mentions.empty()) {
            update.append(kvp("$unset", make_document(kvp(key(gf::kMentions), ""))));
        }
        mongocxx::options::find_one_and_update options{};
        options.projection(make_document(kvp(key(gf::kSender), 1), kvp(key(gf::kHiddenFromPeer), 1)));
        const auto before =
            messages(client).find_one_and_update(session, filter.view(), update.view(), options);
        if (!before) { return std::optional<Mutated>{}; }
        return mutated_of(before->view());
    });
}

Result<std::optional<Revoked>> ChatRepository::revoke(mongocxx::client& client,
                                                      mongocxx::client_session& session,
                                                      const Uuid& conversation, std::int64_t seq,
                                                      const std::optional<Uuid>& sender,
                                                      std::optional<db::TimeMs> not_before,
                                                      std::int64_t mutation) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<Revoked>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, gf::kConversation, conversation);
        filter.append(kvp(key(gf::kSeq), i64(seq)));
        // A system message is the conversation's record of itself and is never
        // taken back.
        filter.append(kvp(key(gf::kKind),
                          make_document(kvp("$ne", i32(static_cast<std::int32_t>(MessageKind::System))))));
        filter.append(kvp(key(gf::kRevoked), absent()));
        if (sender.has_value()) { codec::append_uuid(filter, gf::kSender, *sender); }
        if (not_before.has_value()) {
            filter.append(
                kvp(key(gf::kSentAt), make_document(kvp("$gte", codec::time_date(*not_before)))));
        }
        mongocxx::options::find_one_and_update options{};
        // BEFORE the update, for the attachments: the caller releases each one's
        // reference in this same transaction.
        options.return_document(mongocxx::options::return_document::k_before);
        options.projection(make_document(kvp(key(gf::kAttachments), 1), kvp(key(gf::kSender), 1),
                                         kvp(key(gf::kHiddenFromPeer), 1)));
        const auto before = messages(client).find_one_and_update(
            session, filter.view(),
            make_document(kvp("$set", make_document(kvp(key(gf::kRevoked), bsoncxx::types::b_bool{true}),
                                                    kvp(key(gf::kMutation), i64(mutation)))),
                          kvp("$unset", make_document(kvp(key(gf::kBody), ""),
                                                      kvp(key(gf::kAttachments), ""),
                                                      kvp(key(gf::kMentions), ""),
                                                      kvp(key(gf::kPreview), ""),
                                                      kvp(key(gf::kCardCode), ""),
                                                      kvp(key(gf::kCardBody), ""),
                                                      kvp(key(gf::kCiphertext), ""))))
                .view(),
            options);
        if (!before) { return std::optional<Revoked>{}; }
        Result<std::vector<AttachmentRecord>> attachments = decode_attachments(before->view());
        if (!attachments) { return attachments.error(); }
        const Result<std::optional<Mutated>> who = mutated_of(before->view());
        if (!who) { return who.error(); }
        return std::optional<Revoked>{Revoked{std::move(attachments).value(), *who.value()}};
    });
}

Result<std::optional<MessageRecord>> ChatRepository::claim_expired(
    mongocxx::client& client, mongocxx::client_session& session, db::TimeMs now) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<MessageRecord>> {
        mongocxx::options::find_one_and_delete options{};
        options.sort(make_document(kvp(key(gf::kExpiresAt), 1), kvp(key(gf::kId), 1)));
        // `$exists` beside the range repeats the partial index's own predicate:
        // the planner uses a partial index only when it can prove the query is
        // inside it.
        const auto removed = messages(client).find_one_and_delete(
            session,
            make_document(kvp(key(gf::kExpiresAt),
                              make_document(kvp("$exists", bsoncxx::types::b_bool{true}),
                                            kvp("$lte", codec::time_date(now)))))
                .view(),
            options);
        if (!removed) { return std::optional<MessageRecord>{}; }
        Result<MessageRecord> decoded = decode_message(removed->view());
        if (!decoded) { return decoded.error(); }
        return std::optional<MessageRecord>{std::move(decoded).value()};
    });
}

// --- reactions ----------------------------------------------------------------------

Result<bool> ChatRepository::set_reaction(mongocxx::client& client,
                                          mongocxx::client_session& session,
                                          const Uuid& conversation, std::int64_t seq,
                                          const Uuid& user, std::string_view reaction) const {
    return repo::guarded_in_transaction([&]() -> Result<bool> {
        mongocxx::options::update options{};
        options.upsert(true);
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, rf::kConversation, conversation);
        filter.append(kvp(key(rf::kSeq), i64(seq)));
        codec::append_uuid(filter, rf::kUser, user);
        bsoncxx::builder::basic::document on_insert;
        codec::append_uuid(on_insert, rf::kId, uuid::generate_v4());
        const auto result = reactions_collection(client).update_one(
            session, filter.view(),
            make_document(kvp("$set", make_document(kvp(key(rf::kReaction),
                                                        bsoncxx::types::b_string{key(reaction)}))),
                          kvp("$setOnInsert", on_insert.extract()))
                .view(),
            options);
        // The same reaction again changes nothing, and moves no counter.
        return result.has_value() &&
               (result->upserted_count() > 0 || result->modified_count() > 0);
    });
}

Result<bool> ChatRepository::clear_reaction(mongocxx::client& client,
                                            mongocxx::client_session& session,
                                            const Uuid& conversation, std::int64_t seq,
                                            const Uuid& user) const {
    return repo::guarded_in_transaction([&]() -> Result<bool> {
        const auto result = reactions_collection(client).delete_one(
            session,
            make_document(kvp(key(rf::kConversation), codec::uuid_bin(conversation)),
                          kvp(key(rf::kSeq), i64(seq)), kvp(key(rf::kUser), codec::uuid_bin(user)))
                .view());
        return result.has_value() && result->deleted_count() > 0;
    });
}

Status ChatRepository::stamp_mutation(mongocxx::client& client, mongocxx::client_session& session,
                                      const Uuid& conversation, std::int64_t seq,
                                      std::int64_t mutation) const {
    return repo::guarded_in_transaction([&]() -> Status {
        messages(client).update_one(
            session,
            make_document(kvp(key(gf::kConversation), codec::uuid_bin(conversation)),
                          kvp(key(gf::kSeq), i64(seq)))
                .view(),
            make_document(kvp("$set", make_document(kvp(key(gf::kMutation), i64(mutation))))).view());
        return ok();
    });
}

Result<std::vector<MessageRecord>> ChatRepository::changed(
    mongocxx::client& client, const Uuid& conversation, std::int64_t after_mutation,
    std::optional<std::int64_t> through_mutation, std::int64_t low, std::int64_t high,
    std::int32_t limit, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::vector<MessageRecord>> {
        std::vector<MessageRecord> out;
        if (high <= low) { return out; }
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, gf::kConversation, conversation);
        // `$exists` beside the range repeats the partial index's predicate, so
        // the planner can prove the query is inside it.
        bsoncxx::builder::basic::document range;
        range.append(kvp("$exists", bsoncxx::types::b_bool{true}));
        range.append(kvp("$gt", i64(after_mutation)));
        if (through_mutation.has_value()) { range.append(kvp("$lte", i64(*through_mutation))); }
        filter.append(kvp(key(gf::kMutation), range.extract()));
        filter.append(kvp(key(gf::kSeq), make_document(kvp("$gte", i64(low)), kvp("$lt", i64(high)))));
        append_not_expired_or_timerless(filter, now);
        mongocxx::options::find options{};
        options.sort(make_document(kvp(key(gf::kConversation), 1), kvp(key(gf::kMutation), 1)));
        options.limit(limit > 0 ? limit : 1);
        for (const bsoncxx::document::view doc : messages(client).find(filter.view(), options)) {
            Result<MessageRecord> decoded = decode_message(doc);
            if (!decoded) { return decoded.error(); }
            out.push_back(std::move(decoded).value());
        }
        return out;
    });
}

Result<std::vector<ChatRepository::ReactionTally>> ChatRepository::reactions(
    mongocxx::client& client, const Uuid& conversation, std::span<const std::int64_t> seqs,
    const Uuid& viewer, std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<ReactionTally>> {
        std::vector<ReactionTally> out;
        if (seqs.empty()) { return out; }
        bsoncxx::builder::basic::array in;
        for (const std::int64_t seq : seqs) { in.append(i64(seq)); }
        mongocxx::pipeline pipeline;
        // The match is the {c, s, u} index's prefix, so the group reads only
        // the page's reactions.
        pipeline.match(make_document(kvp(key(rf::kConversation), codec::uuid_bin(conversation)),
                                     kvp(key(rf::kSeq), make_document(kvp("$in", in.extract())))));
        pipeline.group(make_document(
            kvp("_id", make_document(kvp("s", "$s"), kvp("e", "$e"))),
            kvp("n", make_document(kvp("$sum", i64(1)))),
            kvp("mine", make_document(kvp(
                            "$max", make_document(kvp(
                                        "$eq", bsoncxx::builder::basic::make_array(
                                                   "$u", codec::uuid_bin(viewer)))))))));
        pipeline.sort(make_document(kvp("_id.s", 1), kvp("n", -1), kvp("_id.e", 1)));
        pipeline.limit(limit > 0 ? limit : 1);
        for (const bsoncxx::document::view doc : reactions_collection(client).aggregate(pipeline)) {
            const auto id = doc["_id"];
            if (!id || id.type() != bsoncxx::type::k_document) {
                return fail(ErrorCode::Internal, rf::kReaction);
            }
            const bsoncxx::document::view group = id.get_document().value;
            const Result<std::int64_t> seq = codec::read_int64(group, "s");
            if (!seq) { return seq.error(); }
            const Result<std::string_view> reaction = codec::read_text(group, "e");
            if (!reaction) { return reaction.error(); }
            const Result<std::int64_t> count = codec::read_int64(doc, "n");
            if (!count) { return count.error(); }
            const Result<bool> mine = codec::read_bool(doc, "mine");
            if (!mine) { return mine.error(); }
            out.push_back(ReactionTally{std::string{reaction.value()}, seq.value(), count.value(),
                                        mine.value()});
        }
        return out;
    });
}

Status ChatRepository::clear_reactions(mongocxx::client& client,
                                       mongocxx::client_session& session,
                                       const Uuid& conversation, std::int64_t seq) const {
    return repo::guarded_in_transaction([&]() -> Status {
        reactions_collection(client).delete_many(
            session, make_document(kvp(key(rf::kConversation), codec::uuid_bin(conversation)),
                                   kvp(key(rf::kSeq), i64(seq)))
                         .view());
        return ok();
    });
}

// --- invites ------------------------------------------------------------------------

Status ChatRepository::insert_invite(mongocxx::client& client, const crypto::Digest256& digest,
                                     const Uuid& conversation, std::int32_t cap,
                                     db::TimeMs expires_at, const Uuid& created_by) const {
    return repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document doc;
        codec::append_digest(doc, inf::kId, digest);
        codec::append_uuid(doc, inf::kConversation, conversation);
        doc.append(kvp(key(inf::kUses), i32(0)));
        doc.append(kvp(key(inf::kCap), i32(cap)));
        codec::append_time(doc, inf::kExpiresAt, expires_at);
        codec::append_uuid(doc, inf::kCreatedBy, created_by);
        invites(client).insert_one(doc.view());
        return ok();
    });
}

Result<std::optional<Uuid>> ChatRepository::redeem_invite(mongocxx::client& client,
                                                          mongocxx::client_session& session,
                                                          const crypto::Digest256& digest,
                                                          db::TimeMs now) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<Uuid>> {
        // Hash, expiry and remaining uses are all in the FILTER, so two joins
        // racing for the last use are one success and one nothing.
        bsoncxx::builder::basic::document filter;
        codec::append_digest(filter, inf::kId, digest);
        filter.append(kvp(key(inf::kExpiresAt), make_document(kvp("$gt", codec::time_date(now)))));
        filter.append(kvp("$expr", make_document(kvp(
                                       "$lt", bsoncxx::builder::basic::make_array("$n", "$cap")))));
        mongocxx::options::find_one_and_update options{};
        options.projection(make_document(kvp(key(inf::kConversation), 1)));
        const auto updated = invites(client).find_one_and_update(
            session, filter.view(),
            make_document(kvp("$inc", make_document(kvp(key(inf::kUses), i32(1))))).view(), options);
        if (!updated) { return std::optional<Uuid>{}; }
        const Result<Uuid> conversation = codec::read_uuid(updated->view(), inf::kConversation);
        if (!conversation) { return conversation.error(); }
        return std::optional<Uuid>{conversation.value()};
    });
}

Result<bool> ChatRepository::revoke_invite(mongocxx::client& client,
                                           const crypto::Digest256& digest,
                                           const Uuid& conversation) const {
    return repo::guarded([&]() -> Result<bool> {
        const auto result = invites(client).delete_one(
            make_document(kvp(key(inf::kId), codec::digest_bin(digest)),
                          kvp(key(inf::kConversation), codec::uuid_bin(conversation)))
                .view());
        return result.has_value() && result->deleted_count() == 1;
    });
}

// --- blocks -------------------------------------------------------------------------

Status ChatRepository::block(mongocxx::client& client, const Uuid& blocker,
                             const Uuid& blocked) const {
    return repo::guarded([&]() -> Status {
        mongocxx::options::update options{};
        options.upsert(true);
        bsoncxx::builder::basic::document on_insert;
        codec::append_uuid(on_insert, bf::kId, uuid::generate_v4());
        blocks(client).update_one(
            make_document(kvp(key(bf::kBlocker), codec::uuid_bin(blocker)),
                          kvp(key(bf::kBlocked), codec::uuid_bin(blocked)))
                .view(),
            make_document(kvp("$setOnInsert", on_insert.extract())).view(), options);
        return ok();
    });
}

Status ChatRepository::unblock(mongocxx::client& client, const Uuid& blocker,
                               const Uuid& blocked) const {
    return repo::guarded([&]() -> Status {
        blocks(client).delete_one(make_document(kvp(key(bf::kBlocker), codec::uuid_bin(blocker)),
                                                kvp(key(bf::kBlocked), codec::uuid_bin(blocked)))
                                      .view());
        return ok();
    });
}

Result<bool> ChatRepository::has_blocked(mongocxx::client& client, const Uuid& blocker,
                                         const Uuid& blocked) const {
    return repo::guarded([&]() -> Result<bool> {
        mongocxx::options::count options{};
        options.limit(1);
        return blocks(client).count_documents(
                   make_document(kvp(key(bf::kBlocker), codec::uuid_bin(blocker)),
                                 kvp(key(bf::kBlocked), codec::uuid_bin(blocked)))
                       .view(),
                   options) > 0;
    });
}

Result<bool> ChatRepository::blocked_between(mongocxx::client& client, const Uuid& a,
                                             const Uuid& b) const {
    return repo::guarded([&]() -> Result<bool> {
        mongocxx::options::count options{};
        options.limit(1);
        const std::int64_t found = blocks(client).count_documents(
            make_document(kvp("$or", [&a, &b](sub_array branches) {
                branches.append(make_document(kvp(key(bf::kBlocker), codec::uuid_bin(a)),
                                              kvp(key(bf::kBlocked), codec::uuid_bin(b))));
                branches.append(make_document(kvp(key(bf::kBlocker), codec::uuid_bin(b)),
                                              kvp(key(bf::kBlocked), codec::uuid_bin(a))));
            })).view(),
            options);
        return found > 0;
    });
}

}  // namespace anvil::chat
