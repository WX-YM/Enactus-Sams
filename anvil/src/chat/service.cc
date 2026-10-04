#include "anvil/chat/service.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <cstdlib>
#include <limits>
#include <utility>

#include <trantor/utils/Logger.h>

#include "anvil/audit/service.h"
#include "anvil/chat/device_queue.h"
#include "anvil/chat/devices.h"
#include "anvil/chat/live.h"
#include "anvil/chat/push.h"
#include "anvil/chat/text.h"
#include "anvil/core/uuid.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"
#include "anvil/crypto/x25519.h"
#include "anvil/db/repository.h"

namespace anvil::chat {
namespace {

// Thrown inside a transaction to abort it with a Failure. A write refused inside
// a transaction has already aborted it on the server, and a callback that
// returned normally would make the driver retry committing an aborted
// transaction for two minutes; throwing is the only correct way out.
struct AbortTransaction final : std::exception {
    explicit AbortTransaction(Failure f) noexcept : failure{f} {}
    [[nodiscard]] const char* what() const noexcept override { return "chat write aborted"; }
    Failure failure;
};

template <typename T>
T must(Result<T> result) {
    if (!result) { throw AbortTransaction{result.error()}; }
    return std::move(result).value();
}

void must(const Status& status) {
    if (!status) { throw AbortTransaction{status.error()}; }
}

template <typename Fn>
[[nodiscard]] Status transact(mongocxx::client& client, std::string_view what, Fn&& body) {
    try {
        auto session = client.start_session();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) { body(*txn); });
    } catch (const AbortTransaction& aborted) {
        return aborted.failure;
    } catch (const mongocxx::exception& e) {
        LOG_ERROR << "chat " << std::string{what} << " transaction failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable);
    }
    return ok();
}

[[nodiscard]] Failure invalid(std::string_view field, input::Reason reason) noexcept {
    return Failure{ErrorCode::ValidationFailed, field, static_cast<std::uint16_t>(reason)};
}

// A system message: the conversation's record of a change to itself. Its client
// id is random rather than derived, so nothing a client sends can be made to
// collide with one and abort the change it records.
[[nodiscard]] MessageRecord system_message(std::int64_t seq, const Uuid& actor, SystemEvent event,
                                           std::optional<Uuid> subject, Role role,
                                           std::uint32_t timer_s, db::TimeMs now) {
    MessageRecord row{};
    row.id = uuid::generate_v7();
    row.seq = seq;
    row.sender = actor;
    row.client_id = crypto::random_array<16>();
    row.kind = MessageKind::System;
    row.sent_at = now;
    row.system = SystemRecord{subject, timer_s, event, role};
    return row;
}

// A joiner's watermarks start AT the message announcing them: nobody has to
// "read" their own arrival, and an older history a Full kind shows them is
// history, not news.
[[nodiscard]] MemberRecord member_row(const Uuid& conversation, const Uuid& user, Role role,
                                      std::int64_t joined, std::int64_t read, db::TimeMs now) {
    MemberRecord row{};
    row.id = uuid::generate_v4();
    row.conversation = conversation;
    row.user = user;
    row.role = role;
    row.joined_seq = joined;
    row.delivered = read;
    row.read = read;
    row.activity = now;
    return row;
}

// Where a joiner's visible range starts (docs/22 §3.3): everything, or from the
// message that announces them.
[[nodiscard]] std::int64_t joined_at(const ConversationKindSpec& kind,
                                     std::int64_t announce_seq) noexcept {
    return kind.history == History::Full ? 0 : announce_seq;
}

[[nodiscard]] bool encrypted_for(const ConversationKindSpec& kind, bool requested) noexcept {
    switch (kind.e2ee) {
        case E2ee::Never: return false;
        case E2ee::Optional: return requested;
        case E2ee::Required: return true;
    }
    return false;
}

// When a message sent now stops being readable: the conversation's timer, or
// the kind's retention, whichever comes first, or never. Stamped on the row at
// send, so a timer changed later does not reach back into what was already
// said (docs/22 §4.7).
[[nodiscard]] std::optional<db::TimeMs> expiry_for(const ConversationKindSpec& kind,
                                                   std::uint32_t timer_s, db::TimeMs now) {
    std::optional<db::TimeMs> at;
    if (timer_s != 0) { at = now + std::chrono::seconds{timer_s}; }
    if (kind.retention_days != 0) {
        const db::TimeMs retained = now + std::chrono::hours{24} * kind.retention_days;
        if (!at.has_value() || retained < *at) { at = retained; }
    }
    return at;
}

// A conversation as somebody who left it is shown it: the log stops at the last
// message they were entitled to (docs/22 §3.3, §7.1), so the head does too.
// How far the conversation went on after them is not theirs to read, in the
// head any more than in the history.
void as_seen_by(ConversationRecord& conversation, const MemberRecord& member) noexcept {
    if (member.left_seq.has_value()) {
        conversation.seq = std::min(conversation.seq, *member.left_seq - 1);
    }
    // The same for its edits: how many there were after you left is not yours.
    if (member.left_mutations.has_value()) {
        conversation.mutations = std::min(conversation.mutations, *member.left_mutations);
    }
}

[[nodiscard]] bool is_nil_id(const std::array<std::uint8_t, 16>& id) noexcept {
    return std::all_of(id.begin(), id.end(), [](std::uint8_t b) { return b == 0; });
}

}  // namespace

ChatService::ChatService(ChatServiceDeps deps)
    : repository_{deps.repository},
      media_{deps.media},
      kinds_{deps.kinds},
      cards_{deps.cards},
      hooks_{std::move(deps.hooks)},
      invite_pepper_{},
      grants_{deps.grants},
      live_{deps.live},
      push_{deps.push},
      devices_{deps.devices},
      queue_{deps.queue},
      prekeys_{deps.prekeys},
      review_{deps.review} {
    if ((review_.audit == nullptr) != !review_.read_action.has_value()) {
        throw std::invalid_argument{"ChatService: a review needs its audit and its action"};
    }
    // Part of the device machinery is a deployment that accepts an encrypted
    // send and has nowhere to put its per-device ciphertexts, or registers
    // devices nobody can claim a key for.
    if ((devices_ == nullptr) != (queue_ == nullptr) ||
        (devices_ == nullptr) != (prekeys_ == nullptr)) {
        throw std::invalid_argument{"ChatService: devices, queue and prekeys come together"};
    }
    if (deps.invite_pepper.size() != invite_pepper_.size()) {
        throw std::invalid_argument{"ChatService: the invite pepper must be 32 bytes"};
    }
    std::copy(deps.invite_pepper.begin(), deps.invite_pepper.end(), invite_pepper_.data());
    // At construction rather than in cards_are_well_formed, which runs in a
    // constant expression where a function pointer's nullness is not folded.
    for (const CardSpec& card : cards_) {
        if (card.bind == nullptr) {
            throw std::invalid_argument{"ChatService: a card kind has no binder"};
        }
    }
}

void ChatService::report(const MembershipEvent& event) const noexcept {
    if (!hooks_.on_membership) { return; }
    // An application's callback throwing must not turn a committed change into
    // an error response for a write that happened.
    try {
        hooks_.on_membership(event);
    } catch (const std::exception& e) {
        LOG_ERROR << "chat membership hook threw: " << e.what();
    } catch (...) {
        LOG_ERROR << "chat membership hook threw";
    }
}

void ChatService::report_device(const DeviceEvent& event) const noexcept {
    if (!hooks_.on_device) { return; }
    try {
        hooks_.on_device(event);
    } catch (const std::exception& e) {
        LOG_ERROR << "chat device hook threw: " << e.what();
    } catch (...) {
        LOG_ERROR << "chat device hook threw";
    }
}

Status ChatService::mentioned_are_members(mongocxx::client& client, const Uuid& conversation,
                                          std::span<const MentionSpan> mentions) const {
    if (mentions.empty()) { return ok(); }
    std::vector<Uuid> named;
    named.reserve(mentions.size());
    for (const MentionSpan& mention : mentions) {
        if (std::find(named.begin(), named.end(), mention.user) == named.end()) {
            named.push_back(mention.user);
        }
    }
    const Result<std::int64_t> present = repository_.count_current(client, conversation, named);
    if (!present) { return present.error(); }
    if (present.value() != static_cast<std::int64_t>(named.size())) {
        return invalid(kMentionsField, input::Reason::NotAllowed);
    }
    return ok();
}

Result<CardRecord> ChatService::bind_card(const SendMessage::Card& card) const {
    const std::optional<CardCode> code = card_from_key(cards_, card.kind);
    if (!code.has_value() || card.body == nullptr) {
        return invalid(kCardField, input::Reason::NotAllowed);
    }
    Result<std::string> canonical = cards_[*code].bind(*card.body);
    if (!canonical) { return canonical.error(); }
    std::string body = std::move(canonical).value();
    // Re-checked rather than believed: it is served back inside every page that
    // holds it, and a malformed one would break each of them.
    if (body.size() > kMaxCardBytes) { return invalid(kCardField, input::Reason::TooLong); }
    input::BodyArena arena;
    const input::JsonDocument parsed = input::parse_json(body, arena);
    if (!parsed.ok() || !parsed.root().is_object()) {
        LOG_ERROR << "chat card binder produced something that is not a JSON object";
        return fail(ErrorCode::Internal, kCardField);
    }
    return CardRecord{std::move(body), *code};
}

Result<std::vector<AttachmentRecord>> ChatService::resolve_attachments(
    mongocxx::client& client, const Actor& actor, const ConversationKindSpec& kind,
    std::span<const OutgoingAttachment> attachments, bool sealed) const {
    std::vector<AttachmentRecord> out;
    if (attachments.empty()) { return out; }
    const std::optional<fs::Ns> target = sealed ? kind.sealed_ns : kind.media_ns;
    if (!target.has_value()) { return invalid(kAttachmentsField, input::Reason::NotAllowed); }
    const fs::Ns ns = *target;
    out.reserve(attachments.size());
    const db::TimeMs now = db::now_ms();
    const std::int64_t now_unix =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    for (const OutgoingAttachment& wanted : attachments) {
        const bool by_handle = !wanted.handle.empty();
        if (by_handle == wanted.forward.has_value()) {
            return invalid(kAttachmentsField, input::Reason::BadFormat);
        }
        AttachmentRecord record{.name = {}, .media = {}, .duration_ms = 0, .width = 0,
                                .height = 0, .ns = ns, .mime = fs::Mime::Unknown};
        if (by_handle) {
            // Only the uploader's own handle opens, so an object id learned
            // anywhere else cannot become this sender's attachment.
            const std::optional<media::UploadClaim> claim =
                media::open_upload_handle(grants_, wanted.handle, actor.user, now_unix);
            if (!claim.has_value() || claim->ns != ns) {
                return invalid(kAttachmentsField, input::Reason::NotAllowed);
            }
            const Result<std::optional<media::MediaRecord>> stored =
                media_.find(client, claim->ns, claim->id);
            if (!stored) { return stored.error(); }
            if (!stored.value().has_value()) {
                return invalid(kAttachmentsField, input::Reason::NotAllowed);
            }
            const fs::MimeClass mime_class = fs::mime_class(stored.value()->mime);
            const bool fits = sealed ? mime_class == fs::MimeClass::Sealed
                                     : mime_class == fs::MimeClass::Image ||
                                           mime_class == fs::MimeClass::File;
            if (!fits) { return invalid(kAttachmentsField, input::Reason::NotAllowed); }
            // What a client says about its own file is content, and for a
            // sealed blob content is inside the ciphertext (§7.7). A name
            // stored beside it would be the one thing about the file the
            // server could read.
            if (sealed && (!wanted.name.empty() || wanted.width != 0 || wanted.height != 0 ||
                           wanted.duration_ms != 0)) {
                return invalid(kAttachmentsField, input::Reason::NotAllowed);
            }
            if (!wanted.name.empty()) {
                if (const input::Reason reason =
                        validate_line(wanted.name, kMaxAttachmentNameCodePoints);
                    reason != input::Reason::Ok) {
                    return invalid(kAttachmentsField, reason);
                }
            }
            record.media = claim->id;
            record.mime = stored.value()->mime;
            record.name = std::string{wanted.name};
            record.duration_ms = wanted.duration_ms;
            record.width = wanted.width;
            record.height = wanted.height;
        } else {
            // A forward names a message, never an object, and only one the
            // sender can see; the object is then referenced a second time
            // rather than uploaded again.
            const Result<std::optional<MemberRecord>> there =
                repository_.find_member(client, wanted.forward->conversation, actor.user);
            if (!there) { return there.error(); }
            if (!there.value().has_value() || !there.value()->can_see(wanted.forward->seq)) {
                return invalid(kAttachmentsField, input::Reason::NotAllowed);
            }
            const Result<std::optional<MessageRecord>> source = repository_.find_message(
                client, wanted.forward->conversation, wanted.forward->seq, now);
            if (!source) { return source.error(); }
            if (!source.value().has_value() || !source.value()->shown_to(actor.user) ||
                wanted.forward->index >= source.value()->attachments.size()) {
                return invalid(kAttachmentsField, input::Reason::NotAllowed);
            }
            record = source.value()->attachments[wanted.forward->index];
            // A forward across kinds with different namespaces would make one
            // object two namespaces' — refused rather than copied.
            if (record.ns != ns) { return invalid(kAttachmentsField, input::Reason::NotAllowed); }
        }
        for (const AttachmentRecord& earlier : out) {
            if (earlier.media == record.media) {
                return invalid(kAttachmentsField, input::Reason::NotAllowed);
            }
        }
        out.push_back(std::move(record));
    }
    return out;
}

Status ChatService::reachable(mongocxx::client& client, const Uuid& actor,
                              const Uuid& target) const {
    if (actor == target || is_nil(target)) {
        return invalid(kMembersField, input::Reason::NotAllowed);
    }
    const Result<bool> blocked = repository_.blocked_between(client, actor, target);
    if (!blocked) { return blocked.error(); }
    // Refused alike whether a block or the policy said no, and without naming
    // which person: either answer would tell the actor who blocked them.
    if (blocked.value() || !hooks_.may_reach || !hooks_.may_reach(client, actor, target)) {
        return invalid(kMembersField, input::Reason::NotAllowed);
    }
    return ok();
}

Result<ConversationState> ChatService::current(mongocxx::client& client, const Actor& actor,
                                               const Uuid& conversation) const {
    const Result<std::optional<MemberRecord>> member =
        repository_.find_member(client, conversation, actor.user);
    if (!member) { return member.error(); }
    if (!member.value().has_value() || !member.value()->current()) {
        return fail(ErrorCode::NotFound);
    }
    const Result<std::optional<ConversationRecord>> found =
        repository_.find_conversation(client, conversation);
    if (!found) { return found.error(); }
    if (!found.value().has_value()) { return fail(ErrorCode::NotFound); }
    return ConversationState{*found.value(), *member.value()};
}

// --- conversations ------------------------------------------------------------------

Result<CreatedConversation> ChatService::create(mongocxx::client& client, const Actor& actor,
                                                const CreateConversation& request) const {
    const std::optional<KindCode> code = kind_from_key(kinds_, request.kind);
    if (!code.has_value()) { return invalid(kKindField, input::Reason::NotAllowed); }
    const ConversationKindSpec& kind = kinds_[*code];
    // A direct conversation is found, not created (§3.2).
    if (kind.shape == Shape::Direct) { return invalid(kKindField, input::Reason::NotAllowed); }
    if (!actor.permissions.contains_all(kind.create_requires)) { return fail(ErrorCode::Forbidden); }

    if (const input::Reason reason = validate_line(request.title, kMaxTitleCodePoints);
        reason != input::Reason::Ok) {
        return invalid(kTitleField, reason);
    }
    if (!request.description.empty()) {
        if (const input::Reason reason =
                validate_prose(request.description, kMaxDescriptionCodePoints);
            reason != input::Reason::Ok) {
            return invalid(kDescriptionField, reason);
        }
    }

    // A retry reads the first attempt rather than making a second group, as a
    // send's does (§4.2); asked after the checks above, so a retry is refused
    // exactly as the first attempt would have been.
    const bool keyed = !is_nil_id(request.client_id);
    const auto earlier = [&]() -> Result<std::optional<CreatedConversation>> {
        const Result<std::optional<ConversationRecord>> found =
            repository_.find_created(client, actor.user, request.client_id);
        if (!found) { return found.error(); }
        if (!found.value().has_value()) { return std::optional<CreatedConversation>{}; }
        Result<ConversationState> seen = state(client, actor, found.value()->id);
        if (!seen) { return seen.error(); }
        return std::optional<CreatedConversation>{CreatedConversation{
            std::move(seen.value().conversation), seen.value().membership, false}};
    };
    if (keyed) {
        Result<std::optional<CreatedConversation>> retry = earlier();
        if (!retry) { return retry.error(); }
        if (retry.value().has_value()) { return std::move(*retry.value()); }
    }

    std::vector<Uuid> people;
    if (request.members.size() > kMaxMembersPerRequest) {
        return invalid(kMembersField, input::Reason::TooLong);
    }
    people.reserve(request.members.size());
    for (const Uuid& person : request.members) {
        if (person == actor.user) { continue; }
        if (std::find(people.begin(), people.end(), person) != people.end()) { continue; }
        people.push_back(person);
    }
    if (people.size() + 1 > kind.max_members) { return invalid(kMembersField, input::Reason::TooLong); }
    for (const Uuid& person : people) {
        if (const Status reach = reachable(client, actor.user, person); !reach) {
            return reach.error();
        }
    }

    const db::TimeMs now = db::now_ms();
    ConversationRecord row{};
    row.id = uuid::generate_v4();
    row.kind = *code;
    row.encrypted = encrypted_for(kind, request.encrypted);
    row.title = std::string{request.title};
    row.description = std::string{request.description};
    row.created_at = now;
    row.created_by = actor.user;
    if (keyed) { row.client_id = request.client_id; }

    MemberRecord owner{};
    const Status written = transact(client, "create", [&](mongocxx::client_session& txn) {
        must(repository_.insert_conversation(client, txn, row));
        const std::optional<Allocation> alloc =
            must(repository_.allocate_for_membership(client, txn, row.id));
        if (!alloc.has_value()) { throw AbortTransaction{fail(ErrorCode::Internal)}; }
        must(repository_.insert_message(
            client, txn,
            system_message(alloc->seq, actor.user, SystemEvent::Created, std::nullopt, Role::Owner,
                           0, now),
            row.id));
        const std::int64_t joined = joined_at(kind, alloc->seq);
        owner = member_row(row.id, actor.user, Role::Owner, joined, alloc->seq, now);
        must(repository_.join(client, txn, owner));
        for (const Uuid& person : people) {
            must(repository_.join(client, txn,
                                  member_row(row.id, person, Role::Member, joined,
                                             alloc->seq, now)));
        }
        row.seq = alloc->seq;
        row.membership_version = alloc->membership_version;
        row.device_set_version = alloc->device_set_version;
    });
    if (!written) {
        // Two attempts with one key raced past the read above; the unique index
        // let one in, and this one's answer is the winner's.
        if (keyed && written.code() == ErrorCode::Conflict) {
            Result<std::optional<CreatedConversation>> winner = earlier();
            if (!winner) { return winner.error(); }
            if (winner.value().has_value()) { return std::move(*winner.value()); }
        }
        return written.error();
    }
    report(MembershipEvent{row.id, actor.user, actor.user, row.seq, SystemEvent::Created});
    return CreatedConversation{row, owner, true};
}

Result<ConversationState> ChatService::open_direct(mongocxx::client& client, const Actor& actor,
                                                   std::string_view kind_key, const Uuid& other,
                                                   bool encrypted) const {
    const std::optional<KindCode> code = kind_from_key(kinds_, kind_key);
    if (!code.has_value() || kinds_[*code].shape != Shape::Direct) {
        return invalid(kKindField, input::Reason::NotAllowed);
    }
    const ConversationKindSpec& kind = kinds_[*code];
    if (!actor.permissions.contains_all(kind.create_requires)) { return fail(ErrorCode::Forbidden); }
    if (const Status reach = reachable(client, actor.user, other); !reach) { return reach.error(); }

    const db::TimeMs now = db::now_ms();
    ConversationRecord row{};
    row.id = uuid::generate_v4();
    row.kind = *code;
    row.encrypted = encrypted_for(kind, encrypted);
    row.created_at = now;
    row.created_by = actor.user;
    row.direct_pair = direct_pair_key(actor.user, other, row.encrypted);

    const Result<std::pair<ConversationRecord, bool>> found =
        repository_.find_or_insert_direct(client, row);
    if (!found) { return found.error(); }
    ConversationRecord conversation = found.value().first;

    // Both rows, every time, idempotently. The opener who lost the upsert race,
    // or an opener whose first attempt died between the upsert and here, is what
    // finishes a half-made conversation: there is no step only the creator can do.
    // `js` is zero because both people are in it from its first message.
    for (const Uuid& person : {actor.user, other}) {
        if (const Status ensured =
                repository_.ensure_member(client, member_row(conversation.id, person, Role::Member,
                                                             0, 0, now));
            !ensured) {
            return ensured.error();
        }
    }
    if (found.value().second) {
        const Status announced = transact(client, "open", [&](mongocxx::client_session& txn) {
            const std::optional<Allocation> alloc =
                must(repository_.allocate_for_membership(client, txn, conversation.id));
            if (!alloc.has_value()) { throw AbortTransaction{fail(ErrorCode::Internal)}; }
            must(repository_.insert_message(
                client, txn,
                system_message(alloc->seq, actor.user, SystemEvent::Created, other, Role::Member,
                               0, now),
                conversation.id));
            conversation.seq = alloc->seq;
            // The announcement raised the fence; answering the version from
            // before it would hand the opener a set already stale.
            conversation.membership_version = alloc->membership_version;
            conversation.device_set_version = alloc->device_set_version;
        });
        if (!announced) { return announced.error(); }
        report(MembershipEvent{conversation.id, actor.user, other, conversation.seq,
                               SystemEvent::Created});
    }
    const Result<std::optional<MemberRecord>> mine =
        repository_.find_member(client, conversation.id, actor.user);
    if (!mine) { return mine.error(); }
    if (!mine.value().has_value()) { return fail(ErrorCode::Internal); }
    return ConversationState{conversation, *mine.value()};
}

Result<ConversationState> ChatService::state(mongocxx::client& client, const Actor& actor,
                                             const Uuid& conversation) const {
    const Result<std::optional<MemberRecord>> member =
        repository_.find_member(client, conversation, actor.user);
    if (!member) { return member.error(); }
    if (!member.value().has_value()) { return fail(ErrorCode::NotFound); }
    const Result<std::optional<ConversationRecord>> found =
        repository_.find_conversation(client, conversation);
    if (!found) { return found.error(); }
    if (!found.value().has_value()) { return fail(ErrorCode::NotFound); }
    ConversationRecord seen = *found.value();
    as_seen_by(seen, *member.value());
    return ConversationState{std::move(seen), *member.value()};
}

Status ChatService::update_info(mongocxx::client& client, const Actor& actor,
                                const Uuid& conversation, std::optional<std::string_view> title,
                                std::optional<std::string_view> description) const {
    if (title.has_value()) {
        if (const input::Reason reason = validate_line(*title, kMaxTitleCodePoints);
            reason != input::Reason::Ok) {
            return invalid(kTitleField, reason);
        }
    }
    if (description.has_value() && !description->empty()) {
        if (const input::Reason reason = validate_prose(*description, kMaxDescriptionCodePoints);
            reason != input::Reason::Ok) {
            return invalid(kDescriptionField, reason);
        }
    }
    if (!title.has_value() && !description.has_value()) { return ok(); }

    std::int64_t seq = 0;
    const db::TimeMs now = db::now_ms();
    const Status written = transact(client, "info", [&](mongocxx::client_session& txn) {
        const std::optional<MemberRecord> me =
            must(repository_.find_member(client, txn, conversation, actor.user));
        if (!me.has_value() || !me->current()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const std::optional<ConversationRecord> row =
            must(repository_.find_conversation(client, txn, conversation));
        if (!row.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const ConversationKindSpec& kind = kinds_[row->kind];
        if (kind.shape == Shape::Direct || !role_may(kind, me->role, Right::EditInfo)) {
            throw AbortTransaction{fail(ErrorCode::Forbidden)};
        }
        must(repository_.set_info(client, txn, conversation, title, description, std::nullopt));
        const std::optional<Allocation> alloc =
            must(repository_.allocate_for_membership(client, txn, conversation));
        if (!alloc.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        must(repository_.insert_message(
            client, txn,
            system_message(alloc->seq, actor.user, SystemEvent::InfoChanged, std::nullopt,
                           me->role, 0, now),
            conversation));
        seq = alloc->seq;
    });
    if (!written) { return written; }
    report(MembershipEvent{conversation, actor.user, actor.user, seq, SystemEvent::InfoChanged});
    return ok();
}

// --- membership ---------------------------------------------------------------------

Result<std::vector<MemberRecord>> ChatService::members(mongocxx::client& client,
                                                       const Actor& actor,
                                                       const Uuid& conversation,
                                                       const std::optional<Uuid>& after,
                                                       std::int32_t limit) const {
    // A CURRENT member: who is in a conversation now is not the business of
    // somebody who left it, and in an encrypted one it is exactly the set the
    // remaining members' sender keys were rotated away from (§7.1).
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    const ConversationKindSpec& kind = kinds_[me.value().conversation.kind];
    // A follower cannot see who else follows (docs/22 §2.1); an admin can.
    if (kind.shape == Shape::Channel && me.value().membership.role == Role::Member) {
        return fail(ErrorCode::Forbidden);
    }
    return repository_.list_members(client, conversation, after, std::clamp(limit, 1, kMaxMemberPage));
}

Status ChatService::add_members(mongocxx::client& client, const Actor& actor,
                                const Uuid& conversation, std::span<const Uuid> people) const {
    if (people.empty()) { return ok(); }
    if (people.size() > kMaxMembersPerRequest) { return invalid(kMembersField, input::Reason::TooLong); }
    // The caller's membership and right FIRST, outside the transaction as well
    // as inside it. Asking whether each person is reachable before knowing the
    // caller belongs here would answer a stranger NotAllowed where somebody
    // blocked them and NotFound where nobody did: a block oracle over any id.
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    if (!role_may(kinds_[me.value().conversation.kind], me.value().membership.role,
                  Right::AddMember)) {
        return fail(ErrorCode::Forbidden);
    }
    for (const Uuid& person : people) {
        if (const Status reach = reachable(client, actor.user, person); !reach) { return reach; }
    }

    std::vector<MembershipEvent> events;
    const db::TimeMs now = db::now_ms();
    const Status written = transact(client, "add", [&](mongocxx::client_session& txn) {
        events.clear();
        // Read again inside: the check above is a snapshot a concurrent removal
        // could make stale, and this one is serialised with it.
        const std::optional<MemberRecord> fresh =
            must(repository_.find_member(client, txn, conversation, actor.user));
        if (!fresh.has_value() || !fresh->current()) {
            throw AbortTransaction{fail(ErrorCode::NotFound)};
        }
        const std::optional<ConversationRecord> row =
            must(repository_.find_conversation(client, txn, conversation));
        if (!row.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const ConversationKindSpec& kind = kinds_[row->kind];
        if (!role_may(kind, fresh->role, Right::AddMember)) {
            throw AbortTransaction{fail(ErrorCode::Forbidden)};
        }
        // Exact: every membership transaction writes the conversation document,
        // so a concurrent add conflicts and is retried against this one's count.
        const std::int64_t present =
            must(repository_.count_members(client, txn, conversation, kind.max_members + 1));
        if (present + static_cast<std::int64_t>(people.size()) > kind.max_members) {
            throw AbortTransaction{invalid(kMembersField, input::Reason::TooLong)};
        }
        for (const Uuid& person : people) {
            const std::optional<Allocation> alloc =
                must(repository_.allocate_for_membership(client, txn, conversation));
            if (!alloc.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
            const std::int64_t joined = joined_at(kind, alloc->seq);
            const Status joined_status = repository_.join(
                client, txn,
                member_row(conversation, person, Role::Member, joined,
                           alloc->seq, now));
            // Already a member: refused as the whole request, without naming them.
            if (!joined_status) {
                throw AbortTransaction{joined_status.code() == ErrorCode::Conflict
                                           ? invalid(kMembersField, input::Reason::NotAllowed)
                                           : joined_status.error()};
            }
            must(repository_.insert_message(
                client, txn,
                system_message(alloc->seq, actor.user, SystemEvent::MemberAdded, person,
                               Role::Member, 0, now),
                conversation));
            events.push_back(
                MembershipEvent{conversation, actor.user, person, alloc->seq, SystemEvent::MemberAdded});
        }
    });
    if (!written) { return written; }
    for (const MembershipEvent& event : events) { report(event); }
    return ok();
}

Status ChatService::remove_member(mongocxx::client& client, const Actor& actor,
                                  const Uuid& conversation, const Uuid& target) const {
    const bool leaving = target == actor.user;
    std::vector<MembershipEvent> events;
    const db::TimeMs now = db::now_ms();
    const Status written = transact(client, "remove", [&](mongocxx::client_session& txn) {
        events.clear();
        const std::optional<MemberRecord> me =
            must(repository_.find_member(client, txn, conversation, actor.user));
        if (!me.has_value() || !me->current()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const std::optional<ConversationRecord> row =
            must(repository_.find_conversation(client, txn, conversation));
        if (!row.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const ConversationKindSpec& kind = kinds_[row->kind];
        // Two people, fixed at creation: nobody leaves a direct conversation or
        // is removed from one (docs/22 §2.1).
        if (kind.shape == Shape::Direct) { throw AbortTransaction{fail(ErrorCode::Forbidden)}; }

        std::optional<MemberRecord> subject = me;
        if (!leaving) {
            if (!role_may(kind, me->role, Right::RemoveMember)) {
                throw AbortTransaction{fail(ErrorCode::Forbidden)};
            }
            subject = must(repository_.find_member(client, txn, conversation, target));
            if (!subject.has_value() || !subject->current()) {
                throw AbortTransaction{fail(ErrorCode::NotFound)};
            }
            // Nobody removes someone of a higher role than their own.
            if (static_cast<int>(subject->role) > static_cast<int>(me->role)) {
                throw AbortTransaction{fail(ErrorCode::Forbidden)};
            }
        }

        const std::optional<Allocation> alloc =
            must(repository_.allocate_for_membership(client, txn, conversation));
        if (!alloc.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        // `ls` is the seq of the message announcing the departure, which the
        // person who left therefore does not see: their range ends before it.
        if (!must(repository_.leave(client, txn, conversation, target, alloc->seq,
                                    alloc->mutations))) {
            throw AbortTransaction{fail(ErrorCode::NotFound)};
        }
        const SystemEvent event = leaving ? SystemEvent::MemberLeft : SystemEvent::MemberRemoved;
        must(repository_.insert_message(
            client, txn,
            system_message(alloc->seq, actor.user, event, target, subject->role, 0, now),
            conversation));
        events.push_back(MembershipEvent{conversation, actor.user, target, alloc->seq, event});

        // The last owner gone: hand the conversation on, in this same
        // transaction, or it is a conversation nobody can ever administer again.
        if (subject->role == Role::Owner &&
            must(repository_.count_role(client, txn, conversation, Role::Owner)) == 0) {
            std::optional<MemberRecord> heir =
                must(repository_.senior(client, txn, conversation, Role::Admin));
            if (!heir.has_value()) {
                heir = must(repository_.senior(client, txn, conversation, Role::Member));
            }
            if (heir.has_value()) {
                if (!must(repository_.set_role(client, txn, conversation, heir->user, Role::Owner))) {
                    throw AbortTransaction{fail(ErrorCode::Internal)};
                }
                const std::optional<Allocation> next =
                    must(repository_.allocate_for_membership(client, txn, conversation));
                if (!next.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
                must(repository_.insert_message(
                    client, txn,
                    system_message(next->seq, actor.user, SystemEvent::OwnerSucceeded, heir->user,
                                   Role::Owner, 0, now),
                    conversation));
                events.push_back(MembershipEvent{conversation, actor.user, heir->user, next->seq,
                                                 SystemEvent::OwnerSucceeded});
            }
        }
    });
    if (!written) { return written; }
    for (const MembershipEvent& event : events) { report(event); }
    return ok();
}

Status ChatService::set_role(mongocxx::client& client, const Actor& actor,
                             const Uuid& conversation, const Uuid& target, Role role) const {
    if (target == actor.user) { return invalid(kRoleField, input::Reason::NotAllowed); }
    std::int64_t seq = 0;
    const db::TimeMs now = db::now_ms();
    const Status written = transact(client, "role", [&](mongocxx::client_session& txn) {
        const std::optional<MemberRecord> me =
            must(repository_.find_member(client, txn, conversation, actor.user));
        if (!me.has_value() || !me->current()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const std::optional<ConversationRecord> row =
            must(repository_.find_conversation(client, txn, conversation));
        if (!row.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const ConversationKindSpec& kind = kinds_[row->kind];
        if (!role_may(kind, me->role, Right::ManageAdmins)) {
            throw AbortTransaction{fail(ErrorCode::Forbidden)};
        }
        // Owner is only ever handed on by an owner, and an admin with
        // ManageAdmins cannot demote an owner.
        const std::optional<MemberRecord> subject =
            must(repository_.find_member(client, txn, conversation, target));
        if (!subject.has_value() || !subject->current()) {
            throw AbortTransaction{fail(ErrorCode::NotFound)};
        }
        if ((role == Role::Owner || subject->role == Role::Owner) && me->role != Role::Owner) {
            throw AbortTransaction{fail(ErrorCode::Forbidden)};
        }
        if (!must(repository_.set_role(client, txn, conversation, target, role))) {
            throw AbortTransaction{fail(ErrorCode::NotFound)};
        }
        const std::optional<Allocation> alloc =
            must(repository_.allocate_for_membership(client, txn, conversation));
        if (!alloc.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        must(repository_.insert_message(
            client, txn,
            system_message(alloc->seq, actor.user, SystemEvent::RoleChanged, target, role, 0, now),
            conversation));
        seq = alloc->seq;
    });
    if (!written) { return written; }
    report(MembershipEvent{conversation, actor.user, target, seq, SystemEvent::RoleChanged});
    return ok();
}

Result<ConversationState> ChatService::follow(mongocxx::client& client, const Actor& actor,
                                              const Uuid& conversation) const {
    const Result<std::optional<ConversationRecord>> found =
        repository_.find_conversation(client, conversation);
    if (!found) { return found.error(); }
    // Not a channel is answered exactly as no conversation: following is how a
    // channel is reached, and a group must not be joinable by guessing its id.
    if (!found.value().has_value() || kinds_[found.value()->kind].shape != Shape::Channel) {
        return fail(ErrorCode::NotFound);
    }
    const ConversationKindSpec& kind = kinds_[found.value()->kind];
    const db::TimeMs now = db::now_ms();
    MemberRecord joined{};
    // Following writes no system message: a channel with a million followers
    // must not carry a million "X followed" entries in its log.
    //
    // Nor is the follower bound counted here. Counting a six-figure membership
    // on every follow costs more than the bound protects, and nothing a channel
    // does per message is proportional to its followers (docs/22 §2.1, §5.3):
    // max_members on a channel states the scale the deployment was sized for.
    const Status written = transact(client, "follow", [&](mongocxx::client_session& txn) {
        const std::optional<ConversationRecord> row =
            must(repository_.find_conversation(client, txn, conversation));
        if (!row.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        joined = member_row(conversation, actor.user, Role::Member, joined_at(kind, row->seq + 1),
                            row->seq, now);
        const Status status = repository_.join(client, txn, joined);
        if (!status) {
            throw AbortTransaction{status.code() == ErrorCode::Conflict ? fail(ErrorCode::Conflict)
                                                                        : status.error()};
        }
    });
    if (!written) {
        // Following twice is following.
        if (written.code() == ErrorCode::Conflict) { return state(client, actor, conversation); }
        return written.error();
    }
    return ConversationState{*found.value(), joined};
}

// --- messages -------------------------------------------------------------------------

Result<bool> ChatService::hidden_past_block(mongocxx::client& client, const Actor& actor,
                                            const Uuid& conversation,
                                            const ConversationKindSpec& kind) const {
    // A direct conversation past a block (docs/22 §3.6). The sender who blocked
    // the other person knows they did, and is refused; a sender the other
    // person blocked is not told — the message is stored, shown to its sender,
    // and never to the person who blocked them. Their unread count can include
    // it, which an approximate count tolerates (§5.2); refusing would be the
    // disclosure.
    bool hidden = false;
    if (kind.shape != Shape::Direct) { return hidden; }
    const Result<std::vector<MemberRecord>> pair =
        repository_.list_members(client, conversation, std::nullopt, 2);
    if (!pair) { return pair.error(); }
    for (const MemberRecord& member : pair.value()) {
        if (member.user == actor.user) { continue; }
        const Result<bool> mine = repository_.has_blocked(client, actor.user, member.user);
        if (!mine) { return mine.error(); }
        if (mine.value()) { return fail(ErrorCode::Forbidden); }
        const Result<bool> theirs = repository_.has_blocked(client, member.user, actor.user);
        if (!theirs) { return theirs.error(); }
        hidden = theirs.value();
    }
    return hidden;
}

void ChatService::after_send(mongocxx::client& client, const ConversationKindSpec& kind,
                             const Uuid& conversation, KindCode code, const MessageRecord& row,
                             const Allocation& allocation, db::TimeMs now) const {
    const bool hidden = row.hidden_from_peer;
    // The application is told at the commit, before the wake and the bump, so
    // what it records is when the message was stored and not two round trips
    // later. The hook must not block, so the wake waits on nothing.
    if (!hidden && hooks_.on_message) {
        try {
            hooks_.on_message(MessageEvent{conversation, row.sender, row.seq, code, row.kind});
        } catch (...) {
            LOG_ERROR << "chat message hook threw";
        }
    }

    // The wake first: it is what a waiting reader sees, and the activity bump
    // below can be an update_many over a thousand rows. A hidden message wakes
    // its sender's own devices and nobody else's (ChatLive::message_sent).
    if (live_ != nullptr) { live_->message_sent(client, *this, kind, conversation, row, allocation); }
    if (push_ != nullptr && !hidden) { push_->after_commit(kind, conversation, row.seq, now); }

    // A hidden message must not move the blocker's chat list either.
    if (hidden) { return; }
    if (!hooks_.claim_activity_bump || hooks_.claim_activity_bump(conversation)) {
        // A failed bump is not a failed send: the message is stored, and the
        // chat list catches up on the next bump.
        if (const Status bumped = repository_.bump_activity(client, conversation, now); !bumped) {
            LOG_WARN << "chat activity bump failed";
        }
    }
}

Result<SentMessage> ChatService::send(mongocxx::client& client, const Actor& actor,
                                      const Uuid& conversation,
                                      const SendMessage& message) const {
    // Everything that needs no I/O first, so a malformed send costs a parse.
    if (is_nil_id(message.client_id)) { return invalid(kClientIdField, input::Reason::Required); }
    if (message.attachments.size() > kMaxAttachments) {
        return invalid(kAttachmentsField, input::Reason::TooLong);
    }
    // A caption is optional on a message that carries something; a message
    // with neither is nothing.
    const bool captioned =
        !message.body.empty() || (message.attachments.empty() && !message.card.has_value());
    if (captioned) {
        if (const input::Reason reason = validate_message_text(message.body, kMaxMessageCodePoints);
            reason != input::Reason::Ok) {
            return invalid(kBodyField, reason);
        }
    }
    if (const input::Reason reason = validate_mentions(message.body, message.mentions);
        reason != input::Reason::Ok) {
        return invalid(kMentionsField, reason);
    }
    if (message.preview.has_value()) {
        if (const std::optional<input::FieldError> refused = validate_link_preview(*message.preview)) {
            return Failure{ErrorCode::ValidationFailed, refused->field,
                           static_cast<std::uint16_t>(refused->reason)};
        }
    }

    // Step 1, before a seq is spent: a non-member never burns one, and a
    // refusal arriving after the $inc would take longer than one before it —
    // a timing oracle on whether the conversation exists.
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    const ConversationKindSpec& kind = kinds_[me.value().conversation.kind];
    if (!role_may(kind, me.value().membership.role, Right::Post)) {
        return fail(ErrorCode::Forbidden);
    }
    const Result<bool> past_block = hidden_past_block(client, actor, conversation, kind);
    if (!past_block) { return past_block.error(); }
    const bool hidden = past_block.value();

    // Plaintext into an encrypted conversation would put on the server exactly
    // what the conversation exists to keep off it.
    if (me.value().conversation.encrypted) { return invalid(kBodyField, input::Reason::NotAllowed); }
    if (captioned) {
        if (const input::Reason reason =
                validate_message_text(message.body, kind.max_text_code_points);
            reason != input::Reason::Ok) {
            return invalid(kBodyField, reason);
        }
    }
    Result<std::vector<AttachmentRecord>> attachments =
        resolve_attachments(client, actor, kind, message.attachments, false);
    if (!attachments) { return attachments.error(); }
    std::optional<CardRecord> card;
    if (message.card.has_value()) {
        Result<CardRecord> bound = bind_card(*message.card);
        if (!bound) { return bound.error(); }
        card = std::move(bound).value();
    }
    if (const Status named = mentioned_are_members(client, conversation, message.mentions);
        !named) {
        return named.error();
    }

    if (message.reply_to.has_value()) {
        if (!me.value().membership.can_see(*message.reply_to)) {
            return invalid(kReplyField, input::Reason::NotAllowed);
        }
        const Result<std::optional<MessageRecord>> target =
            repository_.find_message(client, conversation, *message.reply_to, db::now_ms());
        if (!target) { return target.error(); }
        if (!target.value().has_value() || !target.value()->shown_to(actor.user)) {
            return invalid(kReplyField, input::Reason::NotAllowed);
        }
    }

    // Step 2: a retry reads the first attempt rather than spending a seq.
    const auto earlier = [&]() -> Result<std::optional<SentMessage>> {
        const Result<std::optional<MessageRecord>> found =
            repository_.find_by_client_id(client, conversation, actor.user, message.client_id);
        if (!found) { return found.error(); }
        if (!found.value().has_value()) { return std::optional<SentMessage>{}; }
        return std::optional<SentMessage>{
            SentMessage{found.value()->sent_at, found.value()->seq, false}};
    };
    {
        const Result<std::optional<SentMessage>> retry = earlier();
        if (!retry) { return retry.error(); }
        if (retry.value().has_value()) { return *retry.value(); }
    }

    // Step 3: the seq, outside any transaction (docs/22 §4.1).
    const Result<std::optional<Allocation>> alloc =
        repository_.allocate(client, conversation, std::nullopt);
    if (!alloc) { return alloc.error(); }
    if (!alloc.value().has_value()) { return fail(ErrorCode::NotFound); }

    const db::TimeMs now = db::now_ms();
    MessageRecord row{};
    row.id = uuid::generate_v7();
    row.seq = alloc.value()->seq;
    row.sender = actor.user;
    row.client_id = message.client_id;
    row.kind = MessageKind::Text;
    row.sent_at = now;
    row.body = std::string{message.body};
    row.mentions.assign(message.mentions.begin(), message.mentions.end());
    if (message.preview.has_value()) {
        row.preview = PreviewRecord{std::string{message.preview->url},
                                    std::string{message.preview->title},
                                    std::string{message.preview->description}};
    }
    row.expires_at = expiry_for(kind, alloc.value()->timer_s, now);
    row.ref = message.reply_to;
    row.hidden_from_peer = hidden;
    row.attachments = std::move(attachments).value();
    if (card.has_value()) {
        row.kind = MessageKind::Card;
        row.card = std::move(card);
    }

    // The push job is asked for before the commit, so a process killed after
    // it has already asked (chat/push.h). A message hidden past a block pushes
    // nobody: its one reader is its sender.
    if (push_ != nullptr && !hidden) { push_->before_commit(kind, conversation, now); }

    // Step 4.
    // The row and every attachment's reference commit together, or neither
    // does (docs/07-filesystem.md §7). An object collected between resolving
    // and here is NotFound, and the send fails rather than pointing at nothing.
    const Status stored = transact(client, "send", [&](mongocxx::client_session& txn) {
        must(repository_.insert_message(client, txn, row, conversation));
        for (const AttachmentRecord& attachment : row.attachments) {
            must(media_.attach(client, txn, attachment.ns, attachment.media));
        }
    });
    if (!stored) {
        // Two attempts with one client id raced past step 2. The unique index
        // let one in; this one's seq is burnt and its answer is the winner's.
        if (stored.code() == ErrorCode::Conflict) {
            const Result<std::optional<SentMessage>> winner = earlier();
            if (!winner) { return winner.error(); }
            if (winner.value().has_value()) { return *winner.value(); }
        }
        return stored.error();
    }

    after_send(client, kind, conversation, me.value().conversation.kind, row, *alloc.value(), now);
    return SentMessage{now, row.seq, true};
}

Result<SentMessage> ChatService::send_encrypted(mongocxx::client& client, const Actor& actor,
                                                const Uuid& conversation,
                                                const EncryptedMessage& message) const {
    if (devices_ == nullptr || queue_ == nullptr) { return fail(ErrorCode::ServiceUnavailable); }
    // Everything that needs no I/O first, so a malformed send costs a parse.
    if (is_nil_id(message.client_id)) { return invalid(kClientIdField, input::Reason::Required); }
    if (is_nil(message.device)) { return invalid(kDeviceField, input::Reason::Required); }
    if (message.ciphertext.empty() && message.devices.empty()) {
        return invalid(kCiphertextField, input::Reason::Required);
    }
    if (message.ciphertext.size() > kMaxCiphertextBytes) {
        return invalid(kCiphertextField, input::Reason::TooLong);
    }
    if (message.devices.size() > kMaxDeviceCiphertexts) {
        return invalid(kDevicesField, input::Reason::TooLong);
    }
    if (message.attachments.size() > kMaxAttachments) {
        return invalid(kAttachmentsField, input::Reason::TooLong);
    }
    if (message.page && (message.devices.empty() || !message.ciphertext.empty())) {
        return invalid(kPageField, input::Reason::NotAllowed);
    }
    for (std::size_t i = 0; i < message.devices.size(); ++i) {
        const DeviceCiphertext& entry = message.devices[i];
        if (entry.ciphertext.empty()) { return invalid(kDevicesField, input::Reason::Required); }
        if (entry.ciphertext.size() > kMaxCiphertextBytes) {
            return invalid(kDevicesField, input::Reason::TooLong);
        }
        // Its own device holds the plaintext already, and a second entry for
        // one device is two ciphertexts the device cannot tell apart.
        if (entry.device == message.device) { return invalid(kDevicesField, input::Reason::NotAllowed); }
        for (std::size_t j = 0; j < i; ++j) {
            if (message.devices[j].device == entry.device) {
                return invalid(kDevicesField, input::Reason::NotAllowed);
            }
        }
    }

    // Step 1, before a seq is spent, as for a plaintext send.
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    const ConversationKindSpec& kind = kinds_[me.value().conversation.kind];
    // Ciphertext in a plaintext conversation would be bytes no member's client
    // is looking for, in a log every member reads as text.
    if (!me.value().conversation.encrypted) {
        return invalid(kCiphertextField, input::Reason::NotAllowed);
    }
    if (!role_may(kind, me.value().membership.role, Right::Post)) {
        return fail(ErrorCode::Forbidden);
    }
    if (message.page && kind.shape != Shape::Group) {
        return invalid(kPageField, input::Reason::NotAllowed);
    }
    const Result<bool> past_block = hidden_past_block(client, actor, conversation, kind);
    if (!past_block) { return past_block.error(); }
    const bool hidden = past_block.value();

    // The sending device is one of the actor's own and current. A session can
    // name any device of its account (prekeys.h says why that is the line),
    // but never one of somebody else's or one that was unlinked.
    if (const Status mine = own_device(client, actor, message.device); !mine) {
        return mine.error();
    }

    // The per-device map against the devices of the CURRENT members, which is
    // read only when there is a map: a group message under a sender key costs
    // no member read at all, and the fence alone guards it.
    std::vector<Uuid> owners_of_entries(message.devices.size());
    if (!message.devices.empty()) {
        const Result<std::vector<ChatRepository::MemberId>> members =
            repository_.member_ids(client, conversation, static_cast<std::int32_t>(kMaxMembers));
        if (!members) { return members.error(); }
        std::vector<Uuid> users;
        users.reserve(members.value().size());
        for (const ChatRepository::MemberId& member : members.value()) { users.push_back(member.user); }
        const Result<std::vector<DeviceOwner>> current_devices = devices_->device_owners(
            client, users, static_cast<std::int32_t>(kMaxMembers));
        if (!current_devices) { return current_devices.error(); }
        std::size_t expected = 0;
        for (const DeviceOwner& owner : current_devices.value()) {
            if (owner.device != message.device) { ++expected; }
        }
        for (std::size_t i = 0; i < message.devices.size(); ++i) {
            const auto found = std::find_if(
                current_devices.value().begin(), current_devices.value().end(),
                [&](const DeviceOwner& owner) { return owner.device == message.devices[i].device; });
            // An extra device is refused outright, never ignored: it is a bug,
            // or an attempt to deliver to a device that is not in the
            // conversation (§7.6).
            if (found == current_devices.value().end()) {
                return invalid(kDevicesField, input::Reason::NotAllowed);
            }
            owners_of_entries[i] = found->user;
        }
        // A missing one is the stale case: the sender's set predates a device
        // or a member, and the client re-encrypts for the set the 409 hands it.
        if (!message.page && message.devices.size() != expected) {
            return Failure{ErrorCode::Conflict, kDevicesField, 0};
        }
    }

    Result<std::vector<AttachmentRecord>> attachments =
        resolve_attachments(client, actor, kind, message.attachments, true);
    if (!attachments) { return attachments.error(); }

    // Step 2: a retry reads the first attempt rather than spending a seq, and
    // is answered with it even if the device set has moved since: that
    // message is stored, and re-encrypting it would be a second one.
    const auto earlier = [&]() -> Result<std::optional<SentMessage>> {
        const Result<std::optional<MessageRecord>> found =
            repository_.find_by_client_id(client, conversation, actor.user, message.client_id);
        if (!found) { return found.error(); }
        if (!found.value().has_value()) { return std::optional<SentMessage>{}; }
        return std::optional<SentMessage>{
            SentMessage{found.value()->sent_at, found.value()->seq, false}};
    };
    {
        const Result<std::optional<SentMessage>> retry = earlier();
        if (!retry) { return retry.error(); }
        if (retry.value().has_value()) { return *retry.value(); }
    }

    // Step 3, with the fence in its filter (§7.6): a sender whose version is
    // not the conversation's matches nothing and spends no seq. The
    // conversation was just read, so matching nothing is the fence.
    const Result<std::optional<Allocation>> alloc =
        repository_.allocate(client, conversation, message.device_set_version);
    if (!alloc) { return alloc.error(); }
    if (!alloc.value().has_value()) { return Failure{ErrorCode::Conflict, kDevicesField, 0}; }

    const db::TimeMs now = db::now_ms();
    MessageRecord row{};
    row.id = uuid::generate_v7();
    row.seq = alloc.value()->seq;
    row.sender = actor.user;
    row.sender_device = message.device;
    row.client_id = message.client_id;
    row.kind = MessageKind::Encrypted;
    row.sent_at = now;
    row.ciphertext.assign(message.ciphertext.begin(), message.ciphertext.end());
    row.expires_at = expiry_for(kind, alloc.value()->timer_s, now);
    row.hidden_from_peer = hidden;
    row.attachments = std::move(attachments).value();

    // A per-device ciphertext lives no longer than its message: a disappearing
    // message must not outlast its timer in somebody's queue.
    db::TimeMs queue_expiry = now + queue_->retention();
    if (row.expires_at.has_value() && *row.expires_at < queue_expiry) {
        queue_expiry = *row.expires_at;
    }
    std::vector<QueuedCiphertext> queued;
    queued.reserve(message.devices.size());
    for (std::size_t i = 0; i < message.devices.size(); ++i) {
        // Past a block, the person who blocked the sender is never delivered
        // to (§3.6); the sender's own other devices still are.
        if (hidden && owners_of_entries[i] != actor.user) { continue; }
        QueuedCiphertext entry{};
        entry.ciphertext.assign(message.devices[i].ciphertext.begin(),
                                message.devices[i].ciphertext.end());
        entry.expires_at = queue_expiry;
        entry.seq = row.seq;
        entry.id = uuid::generate_v7();
        entry.device = message.devices[i].device;
        entry.user = owners_of_entries[i];
        entry.conversation = conversation;
        entry.sender = actor.user;
        entry.sender_device = message.device;
        queued.push_back(std::move(entry));
    }

    // Asked for before the commit, as for a plaintext send (chat/push.h); the
    // job pushes {c, seq} and nothing else for an encrypted conversation.
    if (push_ != nullptr && !hidden) { push_->before_commit(kind, conversation, now); }

    // Step 4: the row, its attachments' references and its queue rows commit
    // together, or none of them does.
    const Status stored = transact(client, "send", [&](mongocxx::client_session& txn) {
        must(repository_.insert_message(client, txn, row, conversation));
        for (const AttachmentRecord& attachment : row.attachments) {
            must(media_.attach(client, txn, attachment.ns, attachment.media));
        }
        must(queue_->enqueue(client, txn, queued));
    });
    if (!stored) {
        if (stored.code() == ErrorCode::Conflict) {
            const Result<std::optional<SentMessage>> winner = earlier();
            if (!winner) { return winner.error(); }
            if (winner.value().has_value()) { return *winner.value(); }
        }
        return stored.error();
    }
    // A device that sends is in use; coalesced to a write an hour.
    (void)devices_->touch(client, actor.user, message.device, now);
    after_send(client, kind, conversation, me.value().conversation.kind, row, *alloc.value(), now);
    return SentMessage{now, row.seq, true};
}

Result<ConversationDevices> ChatService::conversation_devices(mongocxx::client& client,
                                                              const Actor& actor,
                                                              const Uuid& conversation,
                                                              const std::optional<Uuid>& after,
                                                              std::int32_t limit) const {
    if (devices_ == nullptr) { return fail(ErrorCode::ServiceUnavailable); }
    // The conversation, and so its version, is read before the lists.
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    if (!me.value().conversation.encrypted) { return fail(ErrorCode::Forbidden); }
    const std::int32_t bounded = std::clamp(limit, 1, kMaxDevicePage);
    const Result<std::vector<MemberRecord>> members =
        repository_.list_members(client, conversation, after, bounded);
    if (!members) { return members.error(); }
    std::vector<Uuid> users;
    users.reserve(members.value().size());
    for (const MemberRecord& member : members.value()) { users.push_back(member.user); }
    Result<std::vector<AccountDevices>> listed = devices_->devices_of(client, users, bounded);
    if (!listed) { return listed.error(); }

    ConversationDevices out{};
    out.device_set_version = me.value().conversation.device_set_version;
    out.members.reserve(users.size());
    for (const Uuid& user : users) {
        const auto found = std::find_if(listed.value().begin(), listed.value().end(),
                                        [&](const AccountDevices& a) { return a.user == user; });
        if (found == listed.value().end()) {
            out.members.push_back(AccountDevices{.devices = {}, .device_set_version = 0, .user = user});
        } else {
            out.members.push_back(std::move(*found));
        }
    }
    if (users.size() == static_cast<std::size_t>(bounded)) { out.next = users.back(); }
    return out;
}

Result<PresenceView> ChatService::presence(mongocxx::client& client, const Actor& actor,
                                          const Uuid& subject) const {
    if (live_ == nullptr) { return fail(ErrorCode::NotFound); }
    return live_->presence().view(client, actor.user, subject);
}

Result<std::vector<PresenceView>> ChatService::presence(mongocxx::client& client,
                                                       const Actor& actor,
                                                       std::span<const Uuid> subjects) const {
    if (live_ == nullptr) { return fail(ErrorCode::NotFound); }
    return live_->presence().view_many(client, actor.user, subjects);
}

namespace {

// The caller's readable range as [low, high).
[[nodiscard]] std::pair<std::int64_t, std::int64_t> readable(const MemberRecord& member) noexcept {
    const std::int64_t low = std::max(member.joined_seq, member.hide_before + 1);
    const std::int64_t high = member.left_seq.value_or(std::numeric_limits<std::int64_t>::max());
    return {low, high};
}

}  // namespace

Result<HistoryPage> ChatService::history(mongocxx::client& client, const Actor& actor,
                                         const Uuid& conversation,
                                         std::optional<std::int64_t> before,
                                         std::int32_t limit) const {
    const Result<std::optional<MemberRecord>> member =
        repository_.find_member(client, conversation, actor.user);
    if (!member) { return member.error(); }
    if (!member.value().has_value()) { return fail(ErrorCode::NotFound); }
    auto [low, high] = readable(*member.value());
    if (before.has_value()) { high = std::min(high, *before); }
    HistoryPage page{};
    if (high <= low) { return page; }
    Result<HistoryPage> found = repository_.history(client, conversation, low, high,
                                                    std::clamp(limit, 1, kMaxHistoryPage),
                                                    db::now_ms());
    if (!found) { return found.error(); }
    HistoryPage out = std::move(found).value();
    // Filtered after the page is cut, so `older` is still the right cursor: a
    // page is shorter than asked for rather than refilled, which is a bounded
    // read where refilling against a long run of hidden messages is not.
    std::erase_if(out.messages,
                  [&](const MessageRecord& m) { return !m.shown_to(actor.user); });
    // The page's `older` is only a cursor while something older is visible.
    if (out.older.has_value() && *out.older <= low) { out.older.reset(); }
    return out;
}

Result<std::vector<MessageRecord>> ChatService::catch_up(mongocxx::client& client,
                                                         const Actor& actor,
                                                         const Uuid& conversation,
                                                         std::int64_t after,
                                                         std::int32_t limit) const {
    const Result<std::optional<MemberRecord>> member =
        repository_.find_member(client, conversation, actor.user);
    if (!member) { return member.error(); }
    if (!member.value().has_value()) { return fail(ErrorCode::NotFound); }
    const auto [low, high] = readable(*member.value());
    Result<std::vector<MessageRecord>> found =
        repository_.after(client, conversation, std::max(after, low - 1), high,
                          std::clamp(limit, 1, kMaxHistoryPage), db::now_ms());
    if (!found) { return found.error(); }
    std::vector<MessageRecord> out = std::move(found).value();
    std::erase_if(out, [&](const MessageRecord& m) { return !m.shown_to(actor.user); });
    return out;
}

Result<std::vector<MessageRecord>> ChatService::changes(mongocxx::client& client,
                                                        const Actor& actor,
                                                        const Uuid& conversation,
                                                        std::int64_t after,
                                                        std::int32_t limit) const {
    const Result<std::optional<MemberRecord>> member =
        repository_.find_member(client, conversation, actor.user);
    if (!member) { return member.error(); }
    if (!member.value().has_value()) { return fail(ErrorCode::NotFound); }
    const auto [low, high] = readable(*member.value());
    // A past member catches up to where they left, as they read to there.
    Result<std::vector<MessageRecord>> found = repository_.changed(
        client, conversation, std::max<std::int64_t>(after, 0), member.value()->left_mutations,
        low, high, std::clamp(limit, 1, kMaxHistoryPage), db::now_ms());
    if (!found) { return found.error(); }
    std::vector<MessageRecord> out = std::move(found).value();
    std::erase_if(out, [&](const MessageRecord& m) { return !m.shown_to(actor.user); });
    return out;
}

Status ChatService::edit(mongocxx::client& client, const Actor& actor,
                         const Uuid& conversation, std::int64_t seq, std::string_view body,
                         std::span<const MentionSpan> mentions) const {
    if (const input::Reason reason = validate_message_text(body, kMaxMessageCodePoints);
        reason != input::Reason::Ok) {
        return invalid(kBodyField, reason);
    }
    if (const input::Reason reason = validate_mentions(body, mentions);
        reason != input::Reason::Ok) {
        return invalid(kMentionsField, reason);
    }
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    const ConversationKindSpec& kind = kinds_[me.value().conversation.kind];
    if (kind.edit_window_s == 0) { return fail(ErrorCode::Forbidden); }
    if (const input::Reason reason = validate_message_text(body, kind.max_text_code_points);
        reason != input::Reason::Ok) {
        return invalid(kBodyField, reason);
    }
    if (const Status named = mentioned_are_members(client, conversation, mentions); !named) {
        return named;
    }
    const db::TimeMs now = db::now_ms();
    std::optional<Allocation> numbered;
    std::optional<Mutated> edited;
    const Status written = transact(client, "edit", [&](mongocxx::client_session& txn) {
        // The number first: the conversation document is what two mutations
        // conflict on, and the shorter the transaction has held its snapshot
        // when it writes it, the less often a concurrent send makes it retry.
        numbered = must(repository_.allocate_mutation(client, txn, conversation));
        if (!numbered.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        edited = must(repository_.edit(client, txn, conversation, seq, actor.user, body, mentions,
                                       now - std::chrono::seconds{kind.edit_window_s}, now,
                                       numbered->mutations));
        // Not theirs, too old, revoked, gone: the number is given back with
        // the rest of the transaction.
        if (!edited.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
    });
    if (!written) { return written; }
    announce_mutation(client, kind, conversation, *numbered, *edited);
    return ok();
}

Status ChatService::revoke(mongocxx::client& client, const Actor& actor,
                           const Uuid& conversation, std::int64_t seq) const {
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    const ConversationKindSpec& kind = kinds_[me.value().conversation.kind];
    // A message the caller cannot see is not one they can take back, however
    // high their role: a moderator who joined later still did not see it.
    if (!me.value().membership.can_see(seq)) { return fail(ErrorCode::NotFound); }

    const bool moderator = role_may(kind, me.value().membership.role, Right::RevokeAny);
    if (!moderator && kind.revoke_window_s == 0) { return fail(ErrorCode::Forbidden); }
    const db::TimeMs now = db::now_ms();
    const std::optional<Uuid> sender = moderator ? std::nullopt : std::optional<Uuid>{actor.user};
    const std::optional<db::TimeMs> not_before =
        moderator ? std::nullopt
                  : std::optional<db::TimeMs>{now - std::chrono::seconds{kind.revoke_window_s}};

    std::optional<Allocation> numbered;
    std::optional<Revoked> revoked;
    const Status written = transact(client, "revoke", [&](mongocxx::client_session& txn) {
        numbered = must(repository_.allocate_mutation(client, txn, conversation));
        if (!numbered.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        revoked = must(repository_.revoke(client, txn, conversation, seq, sender, not_before,
                                          numbered->mutations));
        // Nothing to take back gives the number back with the transaction.
        if (!revoked.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        for (const AttachmentRecord& attachment : revoked->attachments) {
            const Status released = media_.release(client, txn, attachment.ns, attachment.media);
            // A row already gone holds no count to move. Anything else aborts:
            // a count that commits apart from the message it describes is a
            // leak no sweep can tell from a live reference.
            if (!released && released.code() != ErrorCode::NotFound) {
                throw AbortTransaction{released.error()};
            }
        }
        must(repository_.clear_reactions(client, txn, conversation, seq));
        // Delete for everyone takes the per-device ciphertexts with the common
        // one: a device that had not fetched its copy yet must not get it now.
        if (queue_ != nullptr) { must(queue_->discard_message(client, txn, conversation, seq)); }
    });
    if (!written) { return written; }
    announce_mutation(client, kind, conversation, *numbered, revoked->message);
    return ok();
}

// --- receipts and the chat list -----------------------------------------------------

Result<MemberRecord> ChatService::receipts(mongocxx::client& client, const Actor& actor,
                                           const Uuid& conversation, std::int64_t delivered,
                                           std::int64_t read) const {
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    const std::int64_t head = me.value().conversation.seq;
    const std::int64_t clamped_read = std::clamp<std::int64_t>(read, 0, head);
    const std::int64_t clamped_delivered =
        std::max(std::clamp<std::int64_t>(delivered, 0, head), clamped_read);
    const Result<std::optional<MemberRecord>> advanced =
        repository_.advance(client, conversation, actor.user, clamped_delivered, clamped_read);
    if (!advanced) { return advanced.error(); }
    if (!advanced.value().has_value()) { return fail(ErrorCode::NotFound); }
    return *advanced.value();
}

Result<Readers> ChatService::readers(mongocxx::client& client, const Actor& actor,
                                     const Uuid& conversation, std::int64_t seq) const {
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    const ConversationKindSpec& kind = kinds_[me.value().conversation.kind];
    if (kind.receipts == Receipts::Off) { return fail(ErrorCode::Forbidden); }
    if (!me.value().membership.can_see(seq)) { return fail(ErrorCode::NotFound); }
    // A read route rather than a Receipt frame per watermark move: a frame
    // would be one publish per member per receipt batch, which in a group of
    // a thousand where every member posts is a million publishes per message
    // read. A sender asks once, for their newest message, when they look.
    const auto bound =
        static_cast<std::int32_t>(std::min<std::uint32_t>(kind.max_members, kMaxMembers));
    Readers out{};
    Result<std::vector<Uuid>> delivered = repository_.delivered_to(client, conversation, seq, bound);
    if (!delivered) { return delivered.error(); }
    out.delivered_to = std::move(delivered).value();
    if (kind.receipts == Receipts::Read) {
        Result<std::vector<Uuid>> read = repository_.read_by(client, conversation, seq, bound);
        if (!read) { return read.error(); }
        out.read_by = std::move(read).value();
    }
    return out;
}

namespace {

[[nodiscard]] std::int32_t unread_of(const ConversationRecord& conversation,
                                     const MemberRecord& member) noexcept {
    // A past member's count stops where they left.
    const std::int64_t head =
        member.left_seq.has_value() ? std::min(conversation.seq, *member.left_seq - 1)
                                    : conversation.seq;
    const std::int64_t seen = std::max(member.read, member.hide_before);
    const std::int64_t unread = std::clamp<std::int64_t>(head - seen, 0, kUnreadCap);
    return static_cast<std::int32_t>(unread);
}

}  // namespace

Result<ChatList> ChatService::chat_list(mongocxx::client& client, const Actor& actor,
                                        bool archived,
                                        const std::optional<std::pair<db::TimeMs, Uuid>>& after,
                                        std::int32_t limit) const {
    const std::int32_t bounded = std::clamp(limit, 1, 100);
    const Result<std::vector<MemberRecord>> page =
        repository_.list_memberships(client, actor.user, archived, after, bounded);
    if (!page) { return page.error(); }
    std::vector<MemberRecord> pinned_rows;
    if (!after.has_value() && !archived) {
        Result<std::vector<MemberRecord>> pins = repository_.pinned(client, actor.user, kMaxPins);
        if (!pins) { return pins.error(); }
        pinned_rows = std::move(pins).value();
    }

    // One $in for every conversation the page and the pins name.
    std::vector<Uuid> ids;
    ids.reserve(page.value().size() + pinned_rows.size());
    for (const MemberRecord& row : page.value()) { ids.push_back(row.conversation); }
    for (const MemberRecord& row : pinned_rows) { ids.push_back(row.conversation); }
    const Result<std::vector<ConversationRecord>> conversations =
        repository_.find_conversations(client, ids);
    if (!conversations) { return conversations.error(); }

    const auto item = [&](const MemberRecord& member) -> std::optional<ChatListItem> {
        for (const ConversationRecord& conversation : conversations.value()) {
            if (conversation.id == member.conversation) {
                ConversationRecord seen = conversation;
                as_seen_by(seen, member);
                return ChatListItem{std::move(seen), member, unread_of(conversation, member)};
            }
        }
        return std::nullopt;
    };
    ChatList out{};
    for (const MemberRecord& row : pinned_rows) {
        if (std::optional<ChatListItem> found = item(row)) { out.pinned.push_back(std::move(*found)); }
    }
    for (const MemberRecord& row : page.value()) {
        if (std::optional<ChatListItem> found = item(row)) { out.items.push_back(std::move(*found)); }
    }
    if (page.value().size() == static_cast<std::size_t>(bounded)) {
        out.next = std::pair{page.value().back().activity, page.value().back().id};
    }
    return out;
}

Result<MemberRecord> ChatService::preferences(mongocxx::client& client, const Actor& actor,
                                              const Uuid& conversation,
                                              const MemberPreferences& preferences) const {
    const int ways = (preferences.muted_until.has_value() ? 1 : 0) +
                     (preferences.mute_for.has_value() ? 1 : 0) +
                     (preferences.mute_indefinitely ? 1 : 0);
    if (ways > 1) { return invalid(kMuteForField, input::Reason::NotAllowed); }
    if (preferences.mute_for.has_value() &&
        (preferences.mute_for->count() < 0 || *preferences.mute_for > kMaxMuteDuration)) {
        return invalid(kMuteForField, input::Reason::OutOfRange);
    }
    // A past member keeps their row, and may still mute, archive or clear it.
    const Result<ConversationState> me = state(client, actor, conversation);
    if (!me) { return me.error(); }
    if (preferences.pinned.value_or(false) && !me.value().membership.pinned) {
        const Result<std::vector<MemberRecord>> pins =
            repository_.pinned(client, actor.user, kMaxPins);
        if (!pins) { return pins.error(); }
        if (static_cast<std::int32_t>(pins.value().size()) >= kMaxPins) {
            return invalid(kPinnedField, input::Reason::TooLong);
        }
    }
    ChatRepository::Preferences write{};
    write.muted_until = preferences.muted_until;
    if (preferences.mute_for.has_value()) {
        // The server's clock, the one every push decision reads it against.
        write.muted_until = preferences.mute_for->count() == 0
                                ? std::optional<db::TimeMs>{}
                                : std::optional<db::TimeMs>{db::now_ms() + *preferences.mute_for};
    }
    if (preferences.mute_indefinitely) {
        write.muted_until = std::optional<db::TimeMs>{kMutedIndefinitely};
    }
    write.pinned = preferences.pinned;
    write.archived = preferences.archived;
    write.read_private = preferences.read_private;
    if (preferences.clear) { write.hide_before = me.value().conversation.seq; }
    const Result<bool> written =
        repository_.set_preferences(client, conversation, actor.user, write);
    if (!written) { return written.error(); }
    if (!written.value()) { return fail(ErrorCode::NotFound); }
    // Read back, so a client learns the instant a duration became without
    // computing it from its own clock.
    const Result<std::optional<MemberRecord>> after =
        repository_.find_member(client, conversation, actor.user);
    if (!after) { return after.error(); }
    if (!after.value().has_value()) { return fail(ErrorCode::NotFound); }
    return *after.value();
}

// --- reactions ----------------------------------------------------------------------

Status ChatService::react(mongocxx::client& client, const Actor& actor,
                          const Uuid& conversation, std::int64_t seq,
                          std::optional<std::string_view> reaction) const {
    if (reaction.has_value()) {
        if (const input::Reason reason = validate_reaction(*reaction); reason != input::Reason::Ok) {
            return invalid(kReactionField, reason);
        }
    }
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    const ConversationKindSpec& kind = kinds_[me.value().conversation.kind];
    if (!role_may(kind, me.value().membership.role, Right::React)) { return fail(ErrorCode::Forbidden); }
    if (!me.value().membership.can_see(seq)) { return fail(ErrorCode::NotFound); }
    const Result<std::optional<MessageRecord>> target =
        repository_.find_message(client, conversation, seq, db::now_ms());
    if (!target) { return target.error(); }
    // Taking a reaction back needs the message for the same reason setting one
    // does: who may be told it changed is decided by whose it is.
    if (!target.value().has_value() || !target.value()->shown_to(actor.user)) {
        return fail(ErrorCode::NotFound);
    }
    if (reaction.has_value() &&
        (target.value()->revoked || target.value()->kind != MessageKind::Text)) {
        return fail(ErrorCode::NotFound);
    }
    std::optional<Allocation> numbered;
    const Status written = transact(client, "react", [&](mongocxx::client_session& txn) {
        numbered.reset();
        const bool changed =
            reaction.has_value()
                ? must(repository_.set_reaction(client, txn, conversation, seq, actor.user,
                                                *reaction))
                : must(repository_.clear_reaction(client, txn, conversation, seq, actor.user));
        // The same reaction twice, or taking back one that is not there, is
        // nothing a client has to catch up on.
        if (!changed) { return; }
        numbered = must(repository_.allocate_mutation(client, txn, conversation));
        if (!numbered.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        must(repository_.stamp_mutation(client, txn, conversation, seq, numbered->mutations));
    });
    if (!written) { return written; }
    if (numbered.has_value()) {
        announce_mutation(client, kind, conversation, *numbered,
                          Mutated{target.value()->sender, target.value()->hidden_from_peer});
    }
    return ok();
}

void ChatService::announce_mutation(mongocxx::client& client, const ConversationKindSpec& kind,
                                    const Uuid& conversation, const Allocation& numbered,
                                    const Mutated& message) const {
    if (live_ == nullptr) { return; }
    // A message past a block is its sender's alone (§3.6), and so is news of
    // its change.
    const std::optional<Uuid> only =
        message.hidden_from_peer ? std::optional<Uuid>{message.sender} : std::nullopt;
    live_->message_mutated(client, kind, conversation, numbered, only);
}

Result<std::vector<ChatRepository::ReactionTally>> ChatService::reactions(
    mongocxx::client& client, const Actor& actor, const Uuid& conversation,
    std::span<const std::int64_t> seqs) const {
    const Result<std::optional<MemberRecord>> member =
        repository_.find_member(client, conversation, actor.user);
    if (!member) { return member.error(); }
    if (!member.value().has_value()) { return fail(ErrorCode::NotFound); }
    std::vector<std::int64_t> visible;
    visible.reserve(std::min<std::size_t>(seqs.size(), kMaxHistoryPage));
    for (const std::int64_t seq : seqs) {
        if (visible.size() >= static_cast<std::size_t>(kMaxHistoryPage)) { break; }
        if (member.value()->can_see(seq)) { visible.push_back(seq); }
    }
    // A tally per distinct reaction per message: a page of a hundred messages
    // with a few kinds of reaction each, bounded generously.
    return repository_.reactions(client, conversation, visible, actor.user, kMaxHistoryPage * 16);
}

// --- disappearing messages ----------------------------------------------------------

Status ChatService::set_timer(mongocxx::client& client, const Actor& actor,
                              const Uuid& conversation, std::uint32_t seconds) const {
    std::int64_t seq = 0;
    const db::TimeMs now = db::now_ms();
    const Status written = transact(client, "timer", [&](mongocxx::client_session& txn) {
        const std::optional<MemberRecord> me =
            must(repository_.find_member(client, txn, conversation, actor.user));
        if (!me.has_value() || !me->current()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const std::optional<ConversationRecord> row =
            must(repository_.find_conversation(client, txn, conversation));
        if (!row.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const ConversationKindSpec& kind = kinds_[row->kind];
        if (!role_may(kind, me->role, Right::SetTimer)) {
            throw AbortTransaction{fail(ErrorCode::Forbidden)};
        }
        if (!timer_allowed(kind, seconds)) {
            throw AbortTransaction{invalid(kTimerField, input::Reason::NotAllowed)};
        }
        must(repository_.set_timer(client, txn, conversation, seconds));
        const std::optional<Allocation> alloc =
            must(repository_.allocate_for_membership(client, txn, conversation));
        if (!alloc.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        must(repository_.insert_message(
            client, txn,
            system_message(alloc->seq, actor.user, SystemEvent::TimerChanged, std::nullopt,
                           me->role, seconds, now),
            conversation));
        seq = alloc->seq;
    });
    if (!written) { return written; }
    report(MembershipEvent{conversation, actor.user, actor.user, seq, SystemEvent::TimerChanged});
    return ok();
}

Result<std::int32_t> ChatService::sweep_expired(mongocxx::client& client,
                                                std::int32_t batch) const {
    std::int32_t removed = 0;
    // One pass, bounded, no loop past the batch: a backlog is cleared by the
    // next run, and a run that kept going would be the backlog deciding how
    // long a pool thread is held.
    for (std::int32_t i = 0; i < batch; ++i) {
        bool claimed = false;
        const Status swept = transact(client, "sweep", [&](mongocxx::client_session& txn) {
            const std::optional<MessageRecord> expired =
                must(repository_.claim_expired(client, txn, db::now_ms()));
            claimed = expired.has_value();
            if (!claimed) { return; }
            for (const AttachmentRecord& attachment : expired->attachments) {
                const Status released = media_.release(client, txn, attachment.ns, attachment.media);
                if (!released && released.code() != ErrorCode::NotFound) {
                    throw AbortTransaction{released.error()};
                }
            }
            must(repository_.clear_reactions(client, txn, expired->conversation, expired->seq));
            if (queue_ != nullptr) {
                must(queue_->discard_message(client, txn, expired->conversation, expired->seq));
            }
        });
        if (!swept) { return swept.error(); }
        if (!claimed) { break; }
        ++removed;
    }
    return removed;
}

// --- device changes -------------------------------------------------------------------

namespace {

// The client id of the DevicesChanged message one change writes into one
// conversation: the same for every run of the same change, so a re-run or a
// racing run finds the first one's row through the {c, u, cid} index and writes
// no second. Keyed by the pepper, because the sender is the account itself and
// a client id the account could compute is one it could spend first, on a send
// of its own, and so suppress the notice of its own device change.
[[nodiscard]] std::array<std::uint8_t, 16> device_change_id(std::span<const std::uint8_t> pepper,
                                                            const Uuid& user,
                                                            db::TimeMs since) {
    std::string input{"anvil-chat-dsv"};
    input.append(reinterpret_cast<const char*>(user.data()), user.size());
    const auto ms = static_cast<std::uint64_t>(since.time_since_epoch().count());
    for (int shift = 56; shift >= 0; shift -= 8) {
        input.push_back(static_cast<char>(static_cast<std::uint8_t>(ms >> shift)));
    }
    const crypto::Digest256 digest = crypto::sha256_with_pepper(input, pepper);
    std::array<std::uint8_t, 16> out{};
    std::copy_n(digest.begin(), out.size(), out.begin());
    return out;
}

}  // namespace

Status ChatService::propagate_devices(mongocxx::client& client, const Uuid& user) const {
    if (devices_ == nullptr) { return fail(ErrorCode::ServiceUnavailable); }
    const Result<std::optional<IdentityRecord>> identity = devices_->identity(client, user);
    if (!identity) { return identity.error(); }
    if (!identity.value().has_value() || !identity.value()->pending_since.has_value()) {
        return ok();
    }
    const db::TimeMs since = *identity.value()->pending_since;
    const std::array<std::uint8_t, 16> notice = device_change_id(invite_pepper_.span(), user, since);

    std::optional<Uuid> after;
    for (;;) {
        const Result<std::vector<Uuid>> page =
            repository_.memberships_of(client, user, after, kDeviceChangePage);
        if (!page) { return page.error(); }
        if (page.value().empty()) { break; }
        if (const Status raised = repository_.raise_device_sets(client, page.value(), since);
            !raised) {
            return raised;
        }
        const Result<std::vector<ConversationRecord>> rows =
            repository_.find_conversations(client, page.value());
        if (!rows) { return rows.error(); }
        for (const ConversationRecord& row : rows.value()) {
            // Groups learn of a device from the fence and the device lists their
            // 409 carries; a direct conversation also shows it, because there
            // it is the other person's security code that changed (§7.1).
            if (!row.encrypted || kinds_[row.kind].shape != Shape::Direct) { continue; }
            const Result<std::optional<MessageRecord>> written =
                repository_.find_by_client_id(client, row.id, user, notice);
            if (!written) { return written.error(); }
            if (written.value().has_value()) { continue; }
            const Result<std::optional<Allocation>> alloc =
                repository_.allocate(client, row.id, std::nullopt);
            if (!alloc) { return alloc.error(); }
            if (!alloc.value().has_value()) { continue; }
            MessageRecord message = system_message(alloc.value()->seq, user,
                                                   SystemEvent::DevicesChanged, user,
                                                   Role::Member, 0, db::now_ms());
            message.client_id = notice;
            const Status stored = transact(client, "devices", [&](mongocxx::client_session& txn) {
                must(repository_.insert_message(client, txn, message, row.id));
            });
            // A racing run of the same change got there first, and its message
            // is the one; this run's seq is burnt, as a racing send's would be.
            if (!stored && stored.code() != ErrorCode::Conflict) { return stored; }
        }
        if (page.value().size() < static_cast<std::size_t>(kDeviceChangePage)) { break; }
        after = page.value().back();
    }
    const Result<bool> cleared = devices_->clear_pending(client, user, since);
    if (!cleared) { return cleared.error(); }
    return ok();
}

Result<std::int32_t> ChatService::sweep_device_changes(mongocxx::client& client, db::TimeMs now,
                                                       std::int32_t batch) const {
    if (devices_ == nullptr) { return fail(ErrorCode::ServiceUnavailable); }
    const Result<std::vector<PendingChange>> pending =
        devices_->pending_changes(client, now - kDeviceChangeGrace, batch > 0 ? batch : 1);
    if (!pending) { return pending.error(); }
    std::int32_t finished = 0;
    for (const PendingChange& change : pending.value()) {
        if (const Status done = propagate_devices(client, change.user); !done) {
            return done.error();
        }
        ++finished;
    }
    return finished;
}

// --- devices, keys and the queue ------------------------------------------------------

Status ChatService::require_devices() const {
    if (devices_ == nullptr || queue_ == nullptr || prekeys_ == nullptr) {
        return fail(ErrorCode::ServiceUnavailable);
    }
    return ok();
}

Status ChatService::own_device(mongocxx::client& client, const Actor& actor,
                               const Uuid& device) const {
    const std::array<Uuid, 1> self{actor.user};
    const Result<std::vector<DeviceOwner>> mine = devices_->device_owners(client, self, 1);
    if (!mine) { return mine.error(); }
    if (std::none_of(mine.value().begin(), mine.value().end(),
                     [&](const DeviceOwner& owner) { return owner.device == device; })) {
        return Failure{ErrorCode::Forbidden, kDeviceField, 0};
    }
    return ok();
}

Result<std::optional<IdentityRecord>> ChatService::my_devices(mongocxx::client& client,
                                                              const Actor& actor) const {
    if (const Status ready = require_devices(); !ready) { return ready.error(); }
    return devices_->identity(client, actor.user);
}

Status ChatService::register_device(mongocxx::client& client, const Actor& actor,
                                    NewDevice device) const {
    if (const Status ready = require_devices(); !ready) { return ready; }
    // Asked of the application, because when the account last proved who it
    // is with a password or a passkey is the accounts module's to know. Unset
    // is never: a session alone does not admit a first device (§7.3).
    std::optional<db::TimeMs> authenticated_at;
    if (hooks_.authenticated_at) {
        authenticated_at = hooks_.authenticated_at(client, actor.user, actor.session);
    }
    if (!authenticated_at.has_value()) {
        return Failure{ErrorCode::CapabilityRequired, device_inputs::kAuthenticatedAt, 0};
    }
    device.session = actor.session;
    const db::TimeMs now = db::now_ms();
    if (const Status made = devices_->register_first_device(client, actor.user, device,
                                                             *authenticated_at, now);
        !made) {
        return made;
    }
    report_device(DeviceEvent{actor.user, device.id, actor.session, DeviceChange::Registered});
    return propagate_devices(client, actor.user);
}

Status ChatService::link_device(mongocxx::client& client, const Actor& actor,
                                const Uuid& approver, NewDevice device,
                                std::uint64_t timestamp_s,
                                const crypto::Ed25519Signature& signature) const {
    if (const Status ready = require_devices(); !ready) { return ready; }
    device.session = actor.session;
    if (const Status made = devices_->link_device(client, actor.user, approver, device,
                                                   timestamp_s, signature, db::now_ms());
        !made) {
        return made;
    }
    report_device(DeviceEvent{actor.user, device.id, actor.session, DeviceChange::Linked});
    return propagate_devices(client, actor.user);
}

Result<std::string> ChatService::request_link(mongocxx::client& client, const Actor& actor,
                                             const Uuid& device,
                                             const crypto::X25519PublicKey& agreement,
                                             const crypto::Ed25519PublicKey& signing) const {
    if (const Status ready = require_devices(); !ready) { return ready.error(); }
    if (is_nil(device)) { return invalid(device_inputs::kDeviceId, input::Reason::Required); }
    if (!crypto::x25519_public_key_is_valid(agreement)) {
        return invalid(device_inputs::kAgreementKey, input::Reason::BadFormat);
    }
    if (!crypto::ed25519_public_key_is_valid(signing)) {
        return invalid(device_inputs::kSigningKey, input::Reason::BadFormat);
    }
    std::string token = crypto::random_token();
    const LinkRequest request{.approval = std::nullopt,
                              .expires_at = db::now_ms() + kLinkRequestLifetime,
                              .agreement = agreement,
                              .signing = signing,
                              .device = device,
                              .user = actor.user,
                              .session = actor.session};
    if (const Status stored = devices_->put_link_request(
            client, crypto::sha256_with_pepper(token, invite_pepper_.span()), request);
        !stored) {
        return stored.error();
    }
    return token;
}

namespace {

// A token is 43 base64url characters; anything else is not one, and costs a
// length compare rather than a hash and a round trip.
inline constexpr std::size_t kTokenChars = 43;

}  // namespace

Result<LinkRequest> ChatService::read_link_request(mongocxx::client& client, const Actor& actor,
                                                   std::string_view token) const {
    if (const Status ready = require_devices(); !ready) { return ready.error(); }
    if (token.size() != kTokenChars) { return fail(ErrorCode::NotFound); }
    const Result<std::optional<LinkRequest>> found = devices_->find_link_request(
        client, crypto::sha256_with_pepper(token, invite_pepper_.span()), actor.user,
        db::now_ms());
    if (!found) { return found.error(); }
    if (!found.value().has_value()) { return fail(ErrorCode::NotFound); }
    return *found.value();
}

Status ChatService::approve_link_request(mongocxx::client& client, const Actor& actor,
                                         std::string_view token, const Uuid& approver,
                                         std::uint64_t timestamp_s,
                                         const crypto::Ed25519Signature& signature) const {
    const Result<LinkRequest> request = read_link_request(client, actor, token);
    if (!request) { return request.error(); }
    const db::TimeMs now = db::now_ms();
    const std::int64_t now_s =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    if (timestamp_s > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        std::abs(now_s - static_cast<std::int64_t>(timestamp_s)) > kLinkSkew.count()) {
        return invalid(device_inputs::kTimestamp, input::Reason::OutOfRange);
    }
    const Result<std::optional<IdentityRecord>> identity = devices_->identity(client, actor.user);
    if (!identity) { return identity.error(); }
    const DeviceRecord* signer = nullptr;
    if (identity.value().has_value()) {
        for (const DeviceRecord& device : identity.value()->devices) {
            if (device.published.id == approver) { signer = &device; }
        }
    }
    if (signer == nullptr) { return Failure{ErrorCode::Forbidden, device_inputs::kApprover, 0}; }
    if (!crypto::ed25519_verify(signer->published.keys.signing,
                                link_message(actor.user, request.value().device,
                                             request.value().agreement, request.value().signing,
                                             timestamp_s),
                                signature)) {
        return Failure{ErrorCode::Forbidden, device_inputs::kLinkSignature, 0};
    }
    const Result<bool> written = devices_->approve_link_request(
        client, crypto::sha256_with_pepper(token, invite_pepper_.span()), actor.user,
        LinkProof{signature, timestamp_s, approver}, now);
    if (!written) { return written.error(); }
    if (!written.value()) { return Failure{ErrorCode::Conflict, device_inputs::kApprover, 0}; }
    return ok();
}

Result<std::optional<LinkProof>> ChatService::collect_link_approval(mongocxx::client& client,
                                                                    const Actor& actor,
                                                                    std::string_view token) const {
    if (const Status ready = require_devices(); !ready) { return ready.error(); }
    if (token.size() != kTokenChars) { return fail(ErrorCode::NotFound); }
    const crypto::Digest256 digest = crypto::sha256_with_pepper(token, invite_pepper_.span());
    const db::TimeMs now = db::now_ms();
    const Result<std::optional<LinkRequest>> taken =
        devices_->collect_link_approval(client, digest, actor.user, actor.session, now);
    if (!taken) { return taken.error(); }
    if (taken.value().has_value()) { return taken.value()->approval; }
    // Not approved yet is an answer only for the session that asked; to any
    // other it is as if there were nothing there.
    const Result<std::optional<LinkRequest>> pending =
        devices_->find_link_request(client, digest, actor.user, now);
    if (!pending) { return pending.error(); }
    if (!pending.value().has_value() || pending.value()->session != actor.session ||
        pending.value()->approval.has_value()) {
        return fail(ErrorCode::NotFound);
    }
    return std::optional<LinkProof>{};
}

Status ChatService::rotate_prekeys(mongocxx::client& client, const Actor& actor,
                                   const Uuid& device,
                                   const std::optional<SignedPrekey>& signed_prekey,
                                   const std::optional<SignedPrekey>& last_resort) const {
    if (const Status ready = require_devices(); !ready) { return ready; }
    const Status rotated =
        devices_->rotate_prekeys(client, actor.user, device, signed_prekey, last_resort);
    if (rotated) { (void)devices_->touch(client, actor.user, device, db::now_ms()); }
    return rotated;
}

Status ChatService::unlink_device(mongocxx::client& client, const Actor& actor,
                                  const Uuid& device) const {
    if (const Status ready = require_devices(); !ready) { return ready; }
    const Result<bool> unlinked = devices_->unlink_device(client, actor.user, device, db::now_ms());
    if (!unlinked) { return unlinked.error(); }
    // Idempotent: a device already gone is answered as gone, and its leftovers
    // are discarded again in case the request that unlinked it died first.
    return finish_unlink(client, actor.user, device, actor.session, unlinked.value());
}

Status ChatService::session_ended(mongocxx::client& client, const Uuid& user,
                                  const Uuid& session) const {
    if (const Status ready = require_devices(); !ready) { return ready; }
    const Result<std::optional<Uuid>> unlinked =
        devices_->unlink_session(client, user, session, db::now_ms());
    if (!unlinked) { return unlinked.error(); }
    if (!unlinked.value().has_value()) { return ok(); }
    return finish_unlink(client, user, *unlinked.value(), session, true);
}

Status ChatService::finish_unlink(mongocxx::client& client, const Uuid& user, const Uuid& device,
                                  const Uuid& session, bool changed) const {
    // The keys and the queue of a device that is no longer current are
    // unreachable already: a claim and a read both require a current device.
    // Deleting them is tidiness, and may run late.
    if (const Status keys = prekeys_->discard(client, user, device); !keys) { return keys; }
    if (const Status rows = queue_->discard_device(client, user, device); !rows) { return rows; }
    if (changed) {
        report_device(DeviceEvent{user, device, session, DeviceChange::Unlinked});
    }
    return propagate_devices(client, user);
}

Result<std::int32_t> ChatService::sweep_idle_devices(mongocxx::client& client, db::TimeMs now,
                                                     std::uint32_t idle_days,
                                                     std::int32_t batch) const {
    if (const Status ready = require_devices(); !ready) { return ready.error(); }
    const Result<std::vector<UnlinkedDevice>> idle =
        devices_->unlink_idle(client, now, idle_days, batch);
    if (!idle) { return idle.error(); }
    for (const UnlinkedDevice& gone : idle.value()) {
        if (const Status done = finish_unlink(client, gone.user, gone.device, Uuid{}, true);
            !done) {
            return done.error();
        }
    }
    return static_cast<std::int32_t>(idle.value().size());
}

Status ChatService::upload_prekeys(mongocxx::client& client, const Actor& actor,
                                   const Uuid& device,
                                   std::span<const OneTimePrekey> keys) const {
    if (const Status ready = require_devices(); !ready) { return ready; }
    const Status stored = prekeys_->upload(client, actor.user, device, keys);
    if (stored) { (void)devices_->touch(client, actor.user, device, db::now_ms()); }
    return stored;
}

Result<std::vector<AccountClaim>> ChatService::claim_bundles(
    mongocxx::client& client, const Actor& actor, const Uuid& conversation,
    std::span<const Uuid> targets, const std::function<Status(const Uuid&)>& admit) const {
    if (const Status ready = require_devices(); !ready) { return ready.error(); }
    if (targets.empty()) { return invalid(device_inputs::kUsers, input::Reason::Required); }
    std::vector<Uuid> wanted;
    wanted.reserve(std::min(targets.size(), kMaxClaimBatch));
    for (const Uuid& target : targets) {
        if (std::find(wanted.begin(), wanted.end(), target) != wanted.end()) { continue; }
        if (wanted.size() == kMaxClaimBatch) {
            return invalid(device_inputs::kUsers, input::Reason::TooLong);
        }
        wanted.push_back(target);
    }
    // The caller a current member of one ENCRYPTED conversation: a bundle is
    // claimed to encrypt to somebody you share one with, and to nobody else.
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    if (!me.value().conversation.encrypted) { return fail(ErrorCode::Forbidden); }
    // Every target's membership in one read, BEFORE any budget is spent: a
    // target somebody does not share the conversation with costs that target
    // nothing, or anybody could lock everybody out of claiming against an
    // account by asking about it.
    const Result<std::vector<Uuid>> present =
        repository_.current_among(client, conversation, wanted);
    if (!present) { return present.error(); }
    std::vector<AccountClaim> out;
    out.reserve(wanted.size());
    for (const Uuid& target : wanted) {
        AccountClaim claim{.bundles = {}, .refused = std::nullopt, .user = target};
        if (std::find(present.value().begin(), present.value().end(), target) ==
            present.value().end()) {
            claim.refused = ErrorCode::NotFound;
        } else if (admit) {
            if (const Status admitted = admit(target); !admitted) {
                claim.refused = admitted.code();
            }
        }
        if (!claim.refused.has_value()) {
            Result<std::vector<ClaimedBundle>> bundles = prekeys_->claim(client, target);
            if (!bundles) { return bundles.error(); }
            claim.bundles = std::move(bundles).value();
        }
        out.push_back(std::move(claim));
    }
    return out;
}

Result<std::vector<QueuedCiphertext>> ChatService::device_queue(mongocxx::client& client,
                                                                const Actor& actor,
                                                                const Uuid& device,
                                                                const std::optional<Uuid>& after,
                                                                std::int32_t limit) const {
    if (const Status ready = require_devices(); !ready) { return ready.error(); }
    if (const Status mine = own_device(client, actor, device); !mine) { return mine.error(); }
    const db::TimeMs now = db::now_ms();
    // A device that fetches is a device in use: without this the idle sweeper
    // would unlink one that reads every day and never uploads.
    (void)devices_->touch(client, actor.user, device, now);
    return queue_->read(client, actor.user, device, after, limit, now);
}

Result<std::int64_t> ChatService::acknowledge_queue(mongocxx::client& client, const Actor& actor,
                                                    const Uuid& device,
                                                    const Uuid& through) const {
    if (const Status ready = require_devices(); !ready) { return ready.error(); }
    if (const Status mine = own_device(client, actor, device); !mine) { return mine.error(); }
    return queue_->acknowledge(client, actor.user, device, through);
}

// --- staff review and reports -----------------------------------------------------------

Result<ConversationRecord> ChatService::open_for_review(mongocxx::client& client,
                                                        const Actor& reviewer,
                                                        const std::array<std::uint8_t, 16>& ip,
                                                        const Uuid& conversation) const {
    if (review_.audit == nullptr) { return fail(ErrorCode::ServiceUnavailable); }
    const Result<std::optional<ConversationRecord>> found =
        repository_.find_conversation(client, conversation);
    if (!found) { return found.error(); }
    if (!found.value().has_value()) { return fail(ErrorCode::NotFound); }
    // An encrypted conversation is ciphertext the server cannot read, and a
    // kind the application did not declare reviewable is one it promised its
    // people nobody reads.
    if (found.value()->encrypted || !kinds_[found.value()->kind].reviewable) {
        return Failure{ErrorCode::Forbidden, "conversation", 0};
    }
    // Recorded BEFORE anything is read for the reviewer, and synchronously: a
    // read nothing recorded is the one this log exists to make impossible.
    const audit::AuditEntry entry{.actor = reviewer.user,
                                  .subject = conversation,
                                  .from_state = std::nullopt,
                                  .to_state = std::nullopt,
                                  .ip = ip,
                                  .action = *review_.read_action,
                                  .code = ErrorCode::Ok,
                                  .succeeded = true};
    if (const Status written = review_.audit->write(client, entry); !written) {
        LOG_ERROR << "chat review refused: its audit row could not be written";
        return fail(ErrorCode::ServiceUnavailable);
    }
    return *found.value();
}

Result<Review> ChatService::review(mongocxx::client& client, const Actor& reviewer,
                                   const std::array<std::uint8_t, 16>& ip,
                                   const Uuid& conversation, const std::optional<Uuid>& after,
                                   std::int32_t limit) const {
    Result<ConversationRecord> opened = open_for_review(client, reviewer, ip, conversation);
    if (!opened) { return opened.error(); }
    const std::int32_t bounded = std::clamp(limit, 1, kMaxMemberPage);
    Result<std::vector<MemberRecord>> members =
        repository_.list_members(client, conversation, after, bounded);
    if (!members) { return members.error(); }
    Review out{std::move(opened).value(), std::move(members).value(), std::nullopt};
    if (out.members.size() == static_cast<std::size_t>(bounded)) { out.next = out.members.back().user; }
    return out;
}

Result<HistoryPage> ChatService::review_history(mongocxx::client& client, const Actor& reviewer,
                                                const std::array<std::uint8_t, 16>& ip,
                                                const Uuid& conversation,
                                                std::optional<std::int64_t> before,
                                                std::int32_t limit) const {
    Result<ConversationRecord> opened = open_for_review(client, reviewer, ip, conversation);
    if (!opened) { return opened.error(); }
    const std::int64_t high = before.value_or(opened.value().seq + 1);
    return repository_.history(client, conversation, 0, high,
                               std::clamp(limit, 1, kMaxHistoryPage), db::now_ms());
}

Result<FiledReport> ChatService::report(mongocxx::client& client, const Actor& actor,
                                        const Uuid& conversation, std::int64_t from,
                                        std::int64_t to, std::string_view note) const {
    if (from < 0 || to < from || to - from >= kMaxReportRange) {
        return invalid("to", input::Reason::OutOfRange);
    }
    if (!note.empty()) {
        if (const input::Reason reason = validate_prose(note, kMaxReportNoteCodePoints);
            reason != input::Reason::Ok) {
            return invalid("note", reason);
        }
    }
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    if (me.value().conversation.encrypted ||
        !kinds_[me.value().conversation.kind].reviewable) {
        return Failure{ErrorCode::Forbidden, "conversation", 0};
    }
    // A report names what its reporter was shown, and nothing before they
    // joined or after a clear.
    if (!me.value().membership.can_see(from) || !me.value().membership.can_see(to) ||
        to > me.value().conversation.seq) {
        return invalid("from", input::Reason::NotAllowed);
    }
    const ReportRecord row{.note = std::string{note},
                           .at = db::now_ms(),
                           .from = from,
                           .to = to,
                           .id = uuid::generate_v7(),
                           .conversation = conversation,
                           .reporter = actor.user};
    Result<std::pair<ReportRecord, bool>> filed = repository_.file_report(client, row);
    if (!filed) { return filed.error(); }
    FiledReport out{std::move(filed.value().first), filed.value().second};
    if (out.created && hooks_.on_report) {
        try {
            hooks_.on_report(ReportEvent{from, to, out.report.id, conversation, actor.user});
        } catch (...) {
            LOG_ERROR << "chat report hook threw";
        }
    }
    return out;
}

Result<std::vector<ChatRepository::ReactionTally>> ChatService::review_reactions(
    mongocxx::client& client, const Uuid& conversation, std::span<const std::int64_t> seqs) const {
    // A nil viewer is nobody's reaction: `mine` reads false on every tally.
    return repository_.reactions(client, conversation, seqs.first(std::min<std::size_t>(seqs.size(), kMaxHistoryPage)),
                                 Uuid{}, kMaxHistoryPage * 16);
}

Result<std::vector<ReportRecord>> ChatService::reports(mongocxx::client& client,
                                                       const std::optional<Uuid>& after,
                                                       std::int32_t limit) const {
    return repository_.list_reports(client, after, std::clamp(limit, 1, kMaxReportPage));
}

// --- invites and blocks --------------------------------------------------------------

Result<std::string> ChatService::create_invite(mongocxx::client& client, const Actor& actor,
                                               const Uuid& conversation, std::int32_t uses,
                                               std::chrono::seconds lifetime) const {
    if (uses < 1 || uses > kMaxInviteUses) { return invalid(kInviteField, input::Reason::OutOfRange); }
    if (lifetime.count() <= 0 || lifetime > kMaxInviteLifetime) {
        return invalid(kInviteField, input::Reason::OutOfRange);
    }
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    if (!role_may(kinds_[me.value().conversation.kind], me.value().membership.role,
                  Right::CreateInvite)) {
        return fail(ErrorCode::Forbidden);
    }
    std::string token = crypto::random_token();
    const Status stored = repository_.insert_invite(
        client, crypto::sha256_with_pepper(token, invite_pepper_.span()), conversation, uses,
        db::now_ms() + std::chrono::duration_cast<std::chrono::milliseconds>(lifetime), actor.user);
    if (!stored) { return stored.error(); }
    return token;
}

Result<bool> ChatService::revoke_invite(mongocxx::client& client, const Actor& actor,
                                        const Uuid& conversation, std::string_view token) const {
    const Result<ConversationState> me = current(client, actor, conversation);
    if (!me) { return me.error(); }
    if (!role_may(kinds_[me.value().conversation.kind], me.value().membership.role,
                  Right::CreateInvite)) {
        return fail(ErrorCode::Forbidden);
    }
    return repository_.revoke_invite(
        client, crypto::sha256_with_pepper(token, invite_pepper_.span()), conversation);
}

Result<ConversationState> ChatService::join(mongocxx::client& client, const Actor& actor,
                                            std::string_view token) const {
    // A token is 43 base64url characters; anything else is not one, and costs
    // a length compare rather than a hash and a round trip.
    if (token.size() != 43) { return fail(ErrorCode::NotFound); }
    const crypto::Digest256 digest = crypto::sha256_with_pepper(token, invite_pepper_.span());
    Uuid conversation{};
    std::int64_t seq = 0;
    const db::TimeMs now = db::now_ms();
    bool already = false;
    const Status written = transact(client, "join", [&](mongocxx::client_session& txn) {
        already = false;
        const std::optional<Uuid> target =
            must(repository_.redeem_invite(client, txn, digest, now));
        if (!target.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        conversation = *target;
        const std::optional<MemberRecord> existing =
            must(repository_.find_member(client, txn, conversation, actor.user));
        if (existing.has_value() && existing->current()) {
            // Already in it: answered with the membership, and the use the
            // redeem took is given back by aborting this transaction.
            already = true;
            throw AbortTransaction{fail(ErrorCode::Conflict)};
        }
        const std::optional<ConversationRecord> row =
            must(repository_.find_conversation(client, txn, conversation));
        if (!row.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const ConversationKindSpec& kind = kinds_[row->kind];
        const std::int64_t present =
            must(repository_.count_members(client, txn, conversation, kind.max_members + 1));
        if (present >= kind.max_members) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        const std::optional<Allocation> alloc =
            must(repository_.allocate_for_membership(client, txn, conversation));
        if (!alloc.has_value()) { throw AbortTransaction{fail(ErrorCode::NotFound)}; }
        must(repository_.join(client, txn,
                              member_row(conversation, actor.user, Role::Member,
                                         joined_at(kind, alloc->seq), alloc->seq, now)));
        must(repository_.insert_message(
            client, txn,
            system_message(alloc->seq, actor.user, SystemEvent::JoinedByInvite, actor.user,
                           Role::Member, 0, now),
            conversation));
        seq = alloc->seq;
    });
    if (!written) {
        if (already) { return state(client, actor, conversation); }
        return written.error();
    }
    report(MembershipEvent{conversation, actor.user, actor.user, seq, SystemEvent::JoinedByInvite});
    return state(client, actor, conversation);
}

Status ChatService::block(mongocxx::client& client, const Actor& actor, const Uuid& target) const {
    if (target == actor.user || is_nil(target)) { return invalid(kMembersField, input::Reason::NotAllowed); }
    return repository_.block(client, actor.user, target);
}

Status ChatService::unblock(mongocxx::client& client, const Actor& actor,
                            const Uuid& target) const {
    return repository_.unblock(client, actor.user, target);
}

}  // namespace anvil::chat
