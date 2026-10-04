#pragma once

// Every query the chat module issues, and nothing else (docs/22-chat.md §9.1).
//
// The shapes, so an application can declare the indexes behind them
// (tests/testapp/indexes.h has the rows to copy):
//
//   conversations  {dpk}  UNIQUE, partial on dpk existing     find-or-create direct
//   conversations  {by, cid}  UNIQUE, partial on cid existing  idempotent create
//   members        {c, u} UNIQUE                              every membership check
//   members        {u, ar, act, _id}                          the chat list
//   members        {u, pin}  partial on pin existing           the pinned rows
//   members        {u, c}                                     a device change's walk
//   members        {c, r, js}                                 owner succession
//   members        {c, rd}                                    "read by"
//   members        {c, dlv}                                   "delivered to"
//   messages       {c, s} UNIQUE                              history, sync, by seq
//   messages       {c, u, cid} UNIQUE                         idempotent send
//   messages       {exp, _id}  partial on exp existing         the expiry sweeper
//   messages       {c, mu}  partial on mu existing             the mutation catch-up
//   reactions      {c, s, u} UNIQUE                           one reaction per person
//   invites        {_id}                                      the digest is the key
//   blocks         {u, b} UNIQUE                              both directions, two seeks
//   reports        {c, by, f, to} UNIQUE                      one report per range per reporter
//
// A method taking a client_session& runs inside the caller's transaction and
// lets a transient error through for with_transaction to retry
// (tools/check-db-discipline.sh, rule 6).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/collection.hpp>

#include "anvil/chat/record.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/db/codec.h"
#include "anvil/db/collections.h"

namespace anvil::chat {

// The application's collection names (docs/01-seams.md §4). Views into its
// constexpr table, so they outlive every repository built from them.
struct ChatCollections final {
    std::string_view conversations;
    std::string_view members;
    std::string_view messages;
    std::string_view reactions;
    std::string_view invites;
    std::string_view blocks;
    // Members' reports of messages, for staff (docs/22 §9.2).
    std::string_view reports;
};

// A page of history, oldest first within the page, and where to continue.
struct HistoryPage final {
    std::vector<MessageRecord> messages;
    // The seq to pass as the next `before`, or nullopt when nothing older is
    // visible. Taken from the page itself, never from a count.
    std::optional<std::int64_t> older;
};

// One row of the chat list: the membership, the conversation it is in, and the
// counts derived by subtraction (§5.2).
struct ListEntry final {
    MemberRecord       member;
    ConversationRecord conversation;
};

struct ChatListPage final {
    std::vector<ListEntry> entries;
    // (activity, member id) of the last row, for the next page.
    std::optional<std::pair<db::TimeMs, Uuid>> next;
};

// One message as a push nudge reads it (chat/push.h): who sent it, when, whom it
// names, and the start of its text. Not a MessageRecord, because a nudge reads
// up to a hundred of them and needs none of the attachments, cards or previews
// a message can carry.
struct NudgeMessage final {
    // At most kNudgeTextCodePoints, cut by the server in the projection.
    std::string              text;
    std::vector<Uuid>        mentioned;
    db::TimeMs               sent_at;
    std::int64_t             seq;
    Uuid                     sender;
    MessageKind              kind;
    bool                     revoked;
    bool                     hidden_from_peer;
};

// What a nudge shows of a message: the template parameter limit
// (notifications/template_spec.h), so nothing longer ever leaves the database.
inline constexpr std::int32_t kNudgeTextCodePoints = 120;

// Whose message a mutation changed, and whether it is shown past a block: who
// may be told it changed (§3.6) is decided by both.
struct Mutated final {
    Uuid sender;
    bool hidden_from_peer;
};

// How a revoke left the message: its attachments, so the caller releases each
// reference in the same transaction, and whose it was.
struct Revoked final {
    std::vector<AttachmentRecord> attachments;
    Mutated                       message;
};

class ChatRepository final {
public:
    ChatRepository(const db::DatabaseNames& databases, const ChatCollections& collections,
                   std::span<const ConversationKindSpec> kinds);

    [[nodiscard]] std::span<const ConversationKindSpec> kinds() const noexcept { return kinds_; }

    // --- conversations -----------------------------------------------------------

    [[nodiscard]] Status insert_conversation(mongocxx::client& client,
                                             mongocxx::client_session& session,
                                             const ConversationRecord& row) const;

    // The direct conversation under `row.direct_pair`, inserted when absent, in
    // one upsert. `created` reports whether this call inserted it. Two people
    // opening each other at the same instant converge on one row because the
    // unique index decides, not a read before the write.
    [[nodiscard]] Result<std::pair<ConversationRecord, bool>> find_or_insert_direct(
        mongocxx::client& client, const ConversationRecord& row) const;

    [[nodiscard]] Result<std::optional<ConversationRecord>> find_conversation(
        mongocxx::client& client, const Uuid& id) const;
    [[nodiscard]] Result<std::optional<ConversationRecord>> find_conversation(
        mongocxx::client& client, mongocxx::client_session& session, const Uuid& id) const;

    // The conversation `creator` created under `client_id`: the first attempt
    // of a create being retried.
    [[nodiscard]] Result<std::optional<ConversationRecord>> find_created(
        mongocxx::client& client, const Uuid& creator,
        const std::array<std::uint8_t, 16>& client_id) const;

    // The page of conversations a chat list names, in one $in.
    [[nodiscard]] Result<std::vector<ConversationRecord>> find_conversations(
        mongocxx::client& client, std::span<const Uuid> ids) const;

    // One sequence number. NOT in a transaction (docs/22 §4.1): the conversation
    // is the one hot document here, and a transactional $inc conflicts with
    // every concurrent send to it. `fence`, when set, is the device-set version
    // the sender encrypted against; a mismatch matches nothing and answers
    // nullopt, as an unknown conversation does.
    [[nodiscard]] Result<std::optional<Allocation>> allocate(
        mongocxx::client& client, const Uuid& conversation,
        std::optional<std::int64_t> fence) const;

    // The same inside a membership transaction, which also bumps `mv`: the
    // system message and the member row commit together with the version a
    // cache validates against. It RAISES `dsv` too (raise_device_sets, below),
    // because a member arriving or leaving changes the set of devices a message
    // must be encrypted for exactly as a device does, and an encrypted send
    // fenced on the old set would reach a leaver or miss a joiner.
    [[nodiscard]] Result<std::optional<Allocation>> allocate_for_membership(
        mongocxx::client& client, mongocxx::client_session& session,
        const Uuid& conversation) const;

    // One mutation number (docs/22 §4.5), INSIDE the mutation's transaction,
    // unlike a send's seq. A reader's mutation cursor moves past every number it
    // reads, so a number allocated and committed out of order would be skipped
    // for good; inside the transaction, two mutations of one conversation
    // conflict on this document and commit in the order they numbered. The
    // cost lands on the rare side, as a membership change's does (§3.4).
    [[nodiscard]] Result<std::optional<Allocation>> allocate_mutation(
        mongocxx::client& client, mongocxx::client_session& session,
        const Uuid& conversation) const;

    // Moves each conversation's device-set version to max(dsv + 1, at) in one
    // update_many (docs/22 §7.4). STRICTLY increasing, not a $max of `at` alone:
    // `at` is one process's clock, and a change stamped by a process running
    // behind a version some other process already wrote would otherwise leave
    // the fence where it was, so a sender who read the device set before the
    // change would pass it. A second run raises it again, which costs a sender
    // one 409 and never lets one through stale.
    [[nodiscard]] Status raise_device_sets(mongocxx::client& client,
                                           std::span<const Uuid> conversations,
                                           db::TimeMs at) const;

    // Title, description and icon as a blind $set: these are not read and
    // re-written, so the last writer wins and there is nothing to lose.
    [[nodiscard]] Status set_info(mongocxx::client& client, mongocxx::client_session& session,
                                  const Uuid& conversation, std::optional<std::string_view> title,
                                  std::optional<std::string_view> description,
                                  std::optional<std::optional<Uuid>> icon) const;

    [[nodiscard]] Status set_timer(mongocxx::client& client, mongocxx::client_session& session,
                                   const Uuid& conversation, std::uint32_t timer_s) const;

    // --- members -------------------------------------------------------------------

    // Inserts the membership, or revives a past one (clearing `ls`, writing a
    // new `js`). Conflict when the person is already a current member.
    [[nodiscard]] Status join(mongocxx::client& client, mongocxx::client_session& session,
                              const MemberRecord& row) const;

    // The membership, inserted only when absent, with no transaction. For the
    // two people of a direct conversation, who never leave and whose rows both
    // openers may race to create: the unique {c, u} index decides and neither
    // attempt fails.
    [[nodiscard]] Status ensure_member(mongocxx::client& client, const MemberRecord& row) const;

    [[nodiscard]] Result<std::optional<MemberRecord>> find_member(
        mongocxx::client& client, const Uuid& conversation, const Uuid& user) const;
    [[nodiscard]] Result<std::optional<MemberRecord>> find_member(
        mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation,
        const Uuid& user) const;

    // Current members, by user id, after `after`, at most `limit`.
    [[nodiscard]] Result<std::vector<MemberRecord>> list_members(
        mongocxx::client& client, const Uuid& conversation, const std::optional<Uuid>& after,
        std::int32_t limit) const;

    // Current members, counted up to `ceiling` and no further, so a capacity
    // check costs at most that many index keys.
    [[nodiscard]] Result<std::int64_t> count_members(mongocxx::client& client,
                                                     mongocxx::client_session& session,
                                                     const Uuid& conversation,
                                                     std::int64_t ceiling) const;

    // How many of `users` are current members, in one $in over {c, u}. The
    // mention check: every person a message names must be in it.
    [[nodiscard]] Result<std::int64_t> count_current(mongocxx::client& client,
                                                     const Uuid& conversation,
                                                     std::span<const Uuid> users) const;

    // Which of `users` are current members, in one $in over {c, u}: the batch
    // claim's membership check, the mention check's query answering ids.
    [[nodiscard]] Result<std::vector<Uuid>> current_among(mongocxx::client& client,
                                                          const Uuid& conversation,
                                                          std::span<const Uuid> users) const;

    // One current member as the member cache holds it (chat/member_cache.h).
    struct MemberId final {
        Uuid         user;
        std::int64_t joined_seq;
    };

    // Current members as (user, joined seq), at most `limit`, in no order. The
    // member cache's loader: two fields per row and nothing else, because a miss
    // on a 1 024-member group reads 1 024 rows and the wake path uses neither the
    // role nor a watermark. The same filter as count_members, so the same index.
    [[nodiscard]] Result<std::vector<MemberId>> member_ids(mongocxx::client& client,
                                                           const Uuid& conversation,
                                                           std::int32_t limit) const;

    // Ends a current membership at `left_seq`, recording the conversation's
    // mutation counter then. False when there was none.
    [[nodiscard]] Result<bool> leave(mongocxx::client& client, mongocxx::client_session& session,
                                     const Uuid& conversation, const Uuid& user,
                                     std::int64_t left_seq, std::int64_t left_mutations) const;

    [[nodiscard]] Result<bool> set_role(mongocxx::client& client,
                                        mongocxx::client_session& session,
                                        const Uuid& conversation, const Uuid& user,
                                        Role role) const;

    // The longest-standing current member holding `role`, by joined seq.
    [[nodiscard]] Result<std::optional<MemberRecord>> senior(
        mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation,
        Role role) const;

    // How many current members hold `role`, up to 2: the question is only ever
    // "is there another one".
    [[nodiscard]] Result<std::int64_t> count_role(mongocxx::client& client,
                                                  mongocxx::client_session& session,
                                                  const Uuid& conversation, Role role) const;

    // $max on both watermarks, so a stale or reordered receipt cannot walk
    // either backwards. Answers the current membership's state after the write,
    // or nullopt when the caller is not a current member.
    [[nodiscard]] Result<std::optional<MemberRecord>> advance(mongocxx::client& client,
                                                              const Uuid& conversation,
                                                              const Uuid& user,
                                                              std::int64_t delivered,
                                                              std::int64_t read) const;

    // Mute, pin, archive, clear and read-privacy, as one blind $set on the
    // caller's own row. Every field is the member's own preference and none is
    // read back first.
    struct Preferences final {
        std::optional<std::optional<db::TimeMs>> muted_until;
        std::optional<bool>                      pinned;
        std::optional<bool>                      archived;
        std::optional<std::int64_t>              hide_before;
        std::optional<bool>                      read_private;
    };
    [[nodiscard]] Result<bool> set_preferences(mongocxx::client& client,
                                               const Uuid& conversation, const Uuid& user,
                                               const Preferences& preferences) const;

    // The chat-list bump: $max of `at` over every CURRENT member of the
    // conversation. The caller coalesces it (§5.3).
    [[nodiscard]] Status bump_activity(mongocxx::client& client, const Uuid& conversation,
                                       db::TimeMs at) const;

    // One page of a person's chat list, most recent first, archived or not.
    [[nodiscard]] Result<std::vector<MemberRecord>> list_memberships(
        mongocxx::client& client, const Uuid& user, bool archived,
        const std::optional<std::pair<db::TimeMs, Uuid>>& after, std::int32_t limit) const;

    // The conversations `user` is a CURRENT member of, by conversation id after
    // `after`, at most `limit`. A device change walks every one of them
    // (docs/22 §7.4), so this pages by a key that is total and stable, which
    // the chat list's activity order is not: a conversation bumped mid-walk
    // would move behind the cursor and be skipped.
    [[nodiscard]] Result<std::vector<Uuid>> memberships_of(mongocxx::client& client,
                                                           const Uuid& user,
                                                           const std::optional<Uuid>& after,
                                                           std::int32_t limit) const;

    // A person's pinned conversations, at most `limit`.
    [[nodiscard]] Result<std::vector<MemberRecord>> pinned(mongocxx::client& client,
                                                           const Uuid& user,
                                                           std::int32_t limit) const;

    // Current members whose delivered watermark covers `seq`. Not withheld by
    // `rp`, which is about reads: a person who keeps their reading private
    // still has the message, and a sender shown "delivered" for them learns no
    // more than that their device holds it (docs/22 §5.1).
    [[nodiscard]] Result<std::vector<Uuid>> delivered_to(mongocxx::client& client,
                                                         const Uuid& conversation,
                                                         std::int64_t seq,
                                                         std::int32_t limit) const;

    // Current members whose read watermark covers `seq` and is not private.
    [[nodiscard]] Result<std::vector<Uuid>> read_by(mongocxx::client& client,
                                                    const Uuid& conversation, std::int64_t seq,
                                                    std::int32_t limit) const;

    // --- messages ------------------------------------------------------------------

    // Conflict on a duplicate {c, u, cid}: a retry, which the caller answers by
    // reading the first attempt's row.
    [[nodiscard]] Status insert_message(mongocxx::client& client,
                                        mongocxx::client_session& session,
                                        const MessageRecord& row,
                                        const Uuid& conversation) const;

    [[nodiscard]] Result<std::optional<MessageRecord>> find_by_client_id(
        mongocxx::client& client, const Uuid& conversation, const Uuid& sender,
        const std::array<std::uint8_t, 16>& client_id) const;

    [[nodiscard]] Result<std::optional<MessageRecord>> find_message(
        mongocxx::client& client, const Uuid& conversation, std::int64_t seq,
        db::TimeMs now) const;
    [[nodiscard]] Result<std::optional<MessageRecord>> find_message(
        mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation,
        std::int64_t seq, db::TimeMs now) const;

    // Messages with `low <= s < high`, newest `limit` of them returned oldest
    // first. The caller derives the bounds from the member's visible range; an
    // expired message is filtered here as well as by the sweeper (§4.7).
    [[nodiscard]] Result<HistoryPage> history(mongocxx::client& client,
                                              const Uuid& conversation, std::int64_t low,
                                              std::int64_t high, std::int32_t limit,
                                              db::TimeMs now) const;

    // Messages with `s > after` and `s < high`, oldest first: the catch-up read
    // a device makes with its cursor.
    [[nodiscard]] Result<std::vector<MessageRecord>> after(mongocxx::client& client,
                                                           const Uuid& conversation,
                                                           std::int64_t after_seq,
                                                           std::int64_t high, std::int32_t limit,
                                                           db::TimeMs now) const;

    // The newest `limit` unexpired messages with `s < high`, newest first, as a
    // nudge reads them: the history index's range, projected to seven fields
    // and the first kNudgeTextCodePoints of the text.
    [[nodiscard]] Result<std::vector<NudgeMessage>> recent_for_nudge(
        mongocxx::client& client, const Uuid& conversation, std::int64_t high,
        std::int32_t limit, db::TimeMs now) const;

    // Replaces the body of the sender's own, unrevoked text message, only while
    // it is younger than `not_before`, and stamps `mutation` on it. The
    // conditions are in the FILTER, so an edit racing a revoke or the window
    // closing has no window of its own. nullopt when nothing matched.
    [[nodiscard]] Result<std::optional<Mutated>> edit(
        mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation,
        std::int64_t seq, const Uuid& sender, std::string_view body,
        std::span<const MentionSpan> mentions, db::TimeMs not_before, db::TimeMs at,
        std::int64_t mutation) const;

    // Takes the content and attachments out of a message and marks it revoked.
    // `sender`, when set, restricts it to the sender's own message, and
    // `not_before` to the window; a moderator passes neither. nullopt when
    // nothing matched.
    [[nodiscard]] Result<std::optional<Revoked>> revoke(
        mongocxx::client& client, mongocxx::client_session& session, const Uuid& conversation,
        std::int64_t seq, const std::optional<Uuid>& sender,
        std::optional<db::TimeMs> not_before, std::int64_t mutation) const;

    // The expiry sweeper's claim: removes ONE expired message and answers it, so
    // the caller releases its attachments in the same transaction.
    [[nodiscard]] Result<std::optional<MessageRecord>> claim_expired(
        mongocxx::client& client, mongocxx::client_session& session, db::TimeMs now) const;

    // --- reactions -----------------------------------------------------------------

    // Each answers whether it CHANGED anything: the same reaction twice, or
    // taking back one that is not there, moves no mutation counter.
    [[nodiscard]] Result<bool> set_reaction(mongocxx::client& client,
                                            mongocxx::client_session& session,
                                            const Uuid& conversation, std::int64_t seq,
                                            const Uuid& user, std::string_view reaction) const;
    [[nodiscard]] Result<bool> clear_reaction(mongocxx::client& client,
                                              mongocxx::client_session& session,
                                              const Uuid& conversation, std::int64_t seq,
                                              const Uuid& user) const;

    // Stamps `mutation` on one message, in the transaction that changed its
    // reactions.
    [[nodiscard]] Status stamp_mutation(mongocxx::client& client,
                                        mongocxx::client_session& session,
                                        const Uuid& conversation, std::int64_t seq,
                                        std::int64_t mutation) const;

    // Messages with `low <= s < high` whose mutation is past `after_mutation`
    // and, when given, at most `through_mutation`, oldest mutation first: the
    // catch-up a device makes with its mutation cursor (docs/22 §4.5).
    [[nodiscard]] Result<std::vector<MessageRecord>> changed(
        mongocxx::client& client, const Uuid& conversation, std::int64_t after_mutation,
        std::optional<std::int64_t> through_mutation, std::int64_t low, std::int64_t high,
        std::int32_t limit, db::TimeMs now) const;

    // One reaction on one message, counted, and whether `viewer` is among them.
    struct ReactionTally final {
        std::string  reaction;
        std::int64_t seq;
        std::int64_t count;
        bool         mine;
    };
    // The reactions on the given seqs, TALLIED on the server: a page of a
    // 1 024-member group could otherwise carry a row per person per message,
    // and what a client draws is a count and whether the viewer is in it.
    // Bounded by `limit` tallies.
    [[nodiscard]] Result<std::vector<ReactionTally>> reactions(mongocxx::client& client,
                                                               const Uuid& conversation,
                                                               std::span<const std::int64_t> seqs,
                                                               const Uuid& viewer,
                                                               std::int32_t limit) const;

    // Removed with the message they were on, in its revoke's transaction.
    [[nodiscard]] Status clear_reactions(mongocxx::client& client,
                                         mongocxx::client_session& session,
                                         const Uuid& conversation, std::int64_t seq) const;

    // --- invites -------------------------------------------------------------------

    [[nodiscard]] Status insert_invite(mongocxx::client& client,
                                       const crypto::Digest256& digest,
                                       const Uuid& conversation, std::int32_t cap,
                                       db::TimeMs expires_at, const Uuid& created_by) const;

    // One use of a live link with uses left: the conversation it is for, or
    // nullopt for anything else — unknown, expired, revoked and spent alike.
    [[nodiscard]] Result<std::optional<Uuid>> redeem_invite(mongocxx::client& client,
                                                            mongocxx::client_session& session,
                                                            const crypto::Digest256& digest,
                                                            db::TimeMs now) const;

    [[nodiscard]] Result<bool> revoke_invite(mongocxx::client& client,
                                             const crypto::Digest256& digest,
                                             const Uuid& conversation) const;

    // --- reports -------------------------------------------------------------------

    // The report, or the one this reporter already filed over exactly this
    // range of this conversation (`created` false): a retried report is the
    // first one, by the unique index, with no key of the client's.
    [[nodiscard]] Result<std::pair<ReportRecord, bool>> file_report(
        mongocxx::client& client, const ReportRecord& row) const;

    // Newest first, after `after` (a report id), at most `limit`.
    [[nodiscard]] Result<std::vector<ReportRecord>> list_reports(
        mongocxx::client& client, const std::optional<Uuid>& after, std::int32_t limit) const;

    // --- blocks --------------------------------------------------------------------

    [[nodiscard]] Status block(mongocxx::client& client, const Uuid& blocker,
                               const Uuid& blocked) const;
    [[nodiscard]] Status unblock(mongocxx::client& client, const Uuid& blocker,
                                 const Uuid& blocked) const;
    // One direction: whether `blocker` blocked `blocked`.
    [[nodiscard]] Result<bool> has_blocked(mongocxx::client& client, const Uuid& blocker,
                                           const Uuid& blocked) const;
    // Either direction, in one round trip.
    [[nodiscard]] Result<bool> blocked_between(mongocxx::client& client, const Uuid& a,
                                               const Uuid& b) const;

private:
    [[nodiscard]] mongocxx::collection conversations(mongocxx::client& client) const;
    [[nodiscard]] mongocxx::collection members(mongocxx::client& client) const;
    [[nodiscard]] mongocxx::collection messages(mongocxx::client& client) const;
    [[nodiscard]] mongocxx::collection reactions_collection(mongocxx::client& client) const;
    [[nodiscard]] mongocxx::collection invites(mongocxx::client& client) const;
    [[nodiscard]] mongocxx::collection blocks(mongocxx::client& client) const;
    [[nodiscard]] mongocxx::collection reports(mongocxx::client& client) const;

    // Database names per collection, resolved once at construction. The
    // reports collection is optional, and its name empty without it.
    std::array<std::string, 7>            databases_;
    ChatCollections                       collections_;
    std::span<const ConversationKindSpec> kinds_;
};

}  // namespace anvil::chat
