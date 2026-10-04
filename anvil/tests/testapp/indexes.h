#pragma once

// The reference application's index catalogue.
//
// anvil ships apply_migrations and the IndexSpec shape; which indexes exist is
// the application's, and this table is the single source of truth its explain
// check reads (docs/09-mongodb.md, tests/testapp/queries.h).
//
// The field names come from anvil's own headers rather than from string
// literals. That is the point of publishing them: an index over a column anvil
// does not write is an index the planner never uses, and the symptom is a
// COLLSCAN on the login path rather than a compile error.

#include <array>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/analytics/event.h"
#include "anvil/chat/device_queue.h"
#include "anvil/chat/devices.h"
#include "anvil/chat/prekeys.h"
#include "anvil/chat/record.h"
#include "anvil/audit/record.h"
#include "anvil/db/migrations.h"
#include "anvil/entries/document.h"
#include "anvil/forms/repository.h"
#include "anvil/identity/capabilities.h"
#include "anvil/identity/sessions.h"
#include "anvil/identity/user_fields.h"
#include "anvil/identity/verification.h"
#include "anvil/media/record.h"
#include "anvil/notifications/repository.h"

namespace testapp {

namespace uf = anvil::identity::fields;
namespace sf = anvil::identity::session_fields;
namespace cf = anvil::identity::capability_fields;
namespace vf = anvil::identity::verification_fields;
namespace af = anvil::audit::audit_fields;
namespace mf = anvil::media::media_fields;
namespace ff = anvil::forms::form_fields;
namespace nf = anvil::notifications::notification_fields;
namespace aef = anvil::analytics::event_fields;
namespace asf = anvil::analytics::session_fields;
namespace enf = anvil::entries::entry_fields;
namespace ccf = anvil::chat::conversation_fields;
namespace cmf = anvil::chat::member_fields;
namespace cgf = anvil::chat::message_fields;
namespace crf = anvil::chat::reaction_fields;
namespace cif = anvil::chat::invite_fields;
namespace cbf = anvil::chat::block_fields;
namespace cpr = anvil::chat::report_fields;
namespace cdf = anvil::chat::identity_fields;
namespace cpf = anvil::chat::prekey_fields;
namespace cqf = anvil::chat::device_queue_fields;
namespace clf = anvil::chat::link_fields;

// A partial-index filter, BUILT rather than named.
//
// It repeats the shape a query will use, verbatim: the planner uses a partial
// index only when it can prove the query is a subset of the filter, so a
// predicate that means the same thing in different words gets a COLLSCAN and
// nothing says so.
[[nodiscard]] inline bsoncxx::document::value pending_accounts_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(uf::kStatus),
        bsoncxx::types::b_int32{
            static_cast<std::int32_t>(anvil::UserStatus::PendingVerification)}));
}

// The phone is OPTIONAL, and a unique index treats a missing field as null — so
// without this filter the first account with no phone would lock out every other
// one. It is the single most consequential partial filter in the schema.
[[nodiscard]] inline bsoncxx::document::value phone_present_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(uf::kPhone),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only the media rows that carry a sending address, which is the minority. A
// full index here would hold one entry per row to serve the few that have one.
[[nodiscard]] inline bsoncxx::document::value uploader_ip_present_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(mf::kUploaderIp),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only the media rows that are EDITS. Unique over {ns, src, esha}, so the same
// recipe applied to the same source is one object however many requests race to
// make it (docs/21-image-edits.md §3) — and partial, because every upload in a
// namespace would otherwise collide on a missing pair.
[[nodiscard]] inline bsoncxx::document::value media_edits_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(mf::kSource),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only the analytics rows that name a SUBJECT, which on a consented product is
// the minority and on an anonymous one is none of them. The erasure path is a
// point query against this index; a full index would hold one entry per row in
// the highest-volume collection in the system to serve the few that can ever be
// erased (docs/17-analytics.md §12).
[[nodiscard]] inline bsoncxx::document::value analytics_subject_present_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(aef::kSubject),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// The one-submission-per-person rule, as a CONSTRAINT rather than as a check.
//
// `uniq` is written only when the form asks for one submission per person, so the
// filter is what keeps every other row — anonymous, or on a form that allows
// several — out of the index entirely. Without it the first row with no `uniq`
// would lock out every other one, which is the same failure the optional phone
// number has.
[[nodiscard]] inline bsoncxx::document::value one_per_user_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(ff::kUnique),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// One stored object, one submission. Multikey and unique, so a second submission
// naming an attachment somebody else already bound is rejected BY THE SERVER —
// a read-then-write loses that race to a double-click.
[[nodiscard]] inline bsoncxx::document::value bound_media_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(ff::kMedia),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only the rows that carry an identity, which on most forms is none of them. A
// full index would hold one entry per submission to serve the few that have one.
[[nodiscard]] inline bsoncxx::document::value pii_present_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(ff::kPiiIndex),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// The outbox sweeper looks for rows committed but never dispatched, which is a
// transient minority: every healthy publish clears the marker within a second.
// The partial filter is what keeps the index the size of the backlog rather than
// the size of the collection.
[[nodiscard]] inline bsoncxx::document::value undispatched_only() {
    return bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp(anvil::db::codec::key_of(nf::kDispatchedAt),
                                     bsoncxx::types::b_null{}));
}

// The unread count, and nothing else, rides this. Unread is the minority state
// for anyone who reads their notifications at all.
[[nodiscard]] inline bsoncxx::document::value unread_only() {
    return bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp(anvil::db::codec::key_of(nf::kReadAt),
                                     bsoncxx::types::b_null{}));
}

// A dedupe key is required on every publish, so this is not sparse for the sake
// of optional data — it is unique so that a retried publish is a no-op rather
// than a second notification, which is the whole idempotency guarantee.
[[nodiscard]] inline bsoncxx::document::value dedupe_present_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(nf::kDedupe),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// A slug is unique within its kind, and a kind with no slugs writes none. The
// equality a slug lookup issues implies this filter, which is what lets the
// planner choose the partial index for it.
[[nodiscard]] inline bsoncxx::document::value entry_slug_present_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(enf::kSlug),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only DIRECT conversations carry a pair key. Unique over it, so two people
// opening each other at once converge on one row (docs/22-chat.md §3.2); and
// partial, because every group would otherwise collide on the missing key.
[[nodiscard]] inline bsoncxx::document::value direct_pairs_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(ccf::kDirectPair),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only conversations a client created with a key: a direct conversation has
// none, and neither does one a seed created at boot.
[[nodiscard]] inline bsoncxx::document::value keyed_creations_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(ccf::kClientId),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only pinned memberships, which are a handful per person.
[[nodiscard]] inline bsoncxx::document::value pinned_members_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(cmf::kPinned),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only messages with a timer, which the expiry sweeper walks. NOT a TTL index:
// the monitor would remove a message without releasing its attachments.
[[nodiscard]] inline bsoncxx::document::value expiring_messages_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(cgf::kExpiresAt),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only messages somebody edited, revoked or reacted to, which the mutation
// catch-up walks (docs/22-chat.md §4.5). A message nobody touched, which is
// nearly every one, costs this index nothing.
[[nodiscard]] inline bsoncxx::document::value mutated_messages_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(cgf::kMutation),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// --- chat devices (docs/22-chat.md §7.3) -------------------------------------
//
// Only accounts holding a device. A multikey unique index indexes an EMPTY
// array as one key, so two accounts that had unlinked every device would
// collide with each other; the partial filter leaves them out.
[[nodiscard]] inline bsoncxx::document::value accounts_with_devices_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(cdf::kDeviceIdPath),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}

// Only accounts with a device change not yet pushed to their conversations,
// which is nobody for longer than a request, or a minute after a crash.
[[nodiscard]] inline bsoncxx::document::value pending_device_changes_only() {
    return bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp(
        anvil::db::codec::key_of(cdf::kPendingSince),
        bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("$exists", bsoncxx::types::b_bool{true}))));
}
// --- end chat devices --------------------------------------------------------

inline constexpr std::array<anvil::db::IndexSpec, 68> kIndexes{{
    // --- users -------------------------------------------------------------
    //
    // The three login identities, each its own unique index. Three indexes and
    // not one compound: a login is ONE equality on ONE field, chosen from the
    // identifier's shape before the query is issued, because an `$or` across
    // three cannot use a single index and turns the login path into a scan.
    //
    // The constraint is the SERVER's, not the application's: a check-then-act in
    // code loses to two concurrent registrations, and a unique index does not.
    {{{{uf::kEmailNormalised, 1}}}, "users", "users_email_unique", nullptr, -1, 1, true, false},
    {{{{uf::kUsernameNormalised, 1}}},
     "users",
     "users_username_unique",
     nullptr,
     -1,
     1,
     true,
     false},
    {{{{uf::kPhone, 1}}}, "users", "users_phone_unique", &phone_present_only, -1, 1, true, false},

    // The administrative listing's sort order, so the page comes off the index
    // walk rather than out of a blocking in-memory sort.
    {{{{uf::kUserType, 1}, {uf::kId, 1}}}, "users", "users_type_id", nullptr, -1, 2, false,
     false},

    // The unverified-account sweep. Partial, because the rows it looks for are a
    // transient minority of the collection.
    {{{{uf::kCreatedAt, 1}}}, "users", "users_pending_created", &pending_accounts_only, -1, 1,
     false, false},

    // --- user_sessions -----------------------------------------------------
    //
    // The two hashes a refresh is looked up by. Sparse on the previous one: it
    // is null until the first rotation, and most live sessions have never
    // rotated.
    {{{{sf::kRefreshHash, 1}}}, "user_sessions", "sessions_refresh", nullptr, -1, 1, false,
     false},
    {{{{sf::kPreviousHash, 1}}}, "user_sessions", "sessions_previous", nullptr, -1, 1, false,
     true},
    // The listing and the concurrent-session cap, both ordered by last_seen.
    {{{{sf::kUserId, 1}, {sf::kLastSeen, -1}}}, "user_sessions", "sessions_user_seen", nullptr,
     -1, 2, false, false},
    // TTL. The collection also names `expires_at` in kCollections, which is what
    // makes every query against it carry an explicit expiry filter as well — the
    // monitor lags by up to a minute, so the index alone is a garbage collector
    // and not an access control.
    {{{{sf::kExpiresAt, 1}}}, "user_sessions", "sessions_ttl", nullptr, 0, 1, false, false},

    // --- capability_tokens -------------------------------------------------
    {{{{cf::kHash, 1}}}, "capability_tokens", "capabilities_hash", nullptr, -1, 1, true, false},
    {{{{cf::kExpiresAt, 1}}}, "capability_tokens", "capabilities_ttl", nullptr, 0, 1, false,
     false},

    // --- email_verifications -----------------------------------------------
    //
    // Unique on the address index: one live code per address, which is what
    // makes the re-issue an upsert rather than a delete-then-insert with a
    // window in the middle.
    {{{{vf::kAddressHash, 1}}}, "email_verifications", "verifications_address", nullptr, -1, 1,
     true, false},
    {{{{vf::kExpiresAt, 1}}}, "email_verifications", "verifications_ttl", nullptr, 0, 1, false,
     false},

    // --- media --------------------------------------------------------------
    //
    // Both leading on the namespace, because the namespace is in EVERY media
    // filter — it is what stops an id from one API resolving through another's
    // handler.
    {{{{mf::kNamespace, 1}, {mf::kSha256, 1}}}, "media", "media_ns_hash", nullptr, -1, 2, false,
     false},
    {{{{mf::kNamespace, 1}, {mf::kCreatedAt, -1}, {mf::kId, -1}}}, "media", "media_ns_created",
     nullptr, -1, 3, false, false},
    {{{{mf::kNamespace, 1}, {mf::kSource, 1}, {mf::kEditSha, 1}}}, "media", "media_ns_edit",
     &media_edits_only, -1, 3, true, false},

    // --- drafts -------------------------------------------------------------
    //
    // The one collection in the second database, and its TTL is what keeps that
    // churn out of the hot one. Declared here because kCollections names an
    // expiry field for it: naming the field without building the index gives
    // rows that are filtered out of every read and never actually removed, which
    // is a collection that grows forever while looking empty.
    {{{{"expires_at", 1}}}, "drafts", "drafts_ttl", nullptr, 0, 1, false, false},

    // --- audit_log ----------------------------------------------------------
    //
    // Trailing keys `(at, -1)` and `(_id, -1)`, so the compound cursor comes off
    // the walk. A TTL here is a RETENTION policy rather than a lifetime, which is
    // why audit_log names no expiry field in kCollections: filtering its reads on
    // `at > now` would return nothing at all.
    {{{{af::kActor, 1}, {af::kAt, -1}, {af::kId, -1}}}, "audit_log", "audit_actor_at", nullptr,
     -1, 3, false, false},

    // --- form_definitions ---------------------------------------------------
    //
    // The staff listing orders and paginates by `_id` alone, which is descending
    // creation time because form ids are UUIDv7 — so the primary key already
    // provides both the order and the cursor and no index is needed for it.
    //
    // This one serves the other question a dashboard asks: which forms did this
    // person create, and in what state. It leads on the creator because that is
    // the equality; a compound leading on `status` would serve one screen and
    // scan for the other.
    {{{{ff::kCreator, 1}, {ff::kStatus, 1}}}, "form_definitions", "forms_creator_status",
     nullptr, -1, 2, false, false},

    // --- form_submissions ---------------------------------------------------
    //
    // The retrieval cursor, in the order the page is walked. The trailing
    // `(submitted_at, -1), (_id, -1)` are what make the sort come off the index
    // rather than out of a blocking in-memory sort — which is bounded by a
    // server-side memory budget and fails outright past it, at a collection size
    // nobody chose.
    {{{{ff::kForm, 1}, {ff::kSubmittedAt, -1}, {ff::kId, -1}}}, "form_submissions",
     "submissions_form_time", nullptr, -1, 3, false, false},

    // Duplicate detection without decrypting anything, scoped to one form: the
    // same person may legitimately submit their identity to two different forms.
    {{{{ff::kForm, 1}, {ff::kPiiIndex, 1}}}, "form_submissions", "submissions_form_pii",
     &pii_present_only, -1, 2, false, false},

    // The two UNIQUE constraints. Both are rules that would be races if they were
    // checked in code and then acted on, so both are the server's job.
    {{{{ff::kForm, 1}, {ff::kUnique, 1}}}, "form_submissions", "submissions_one_per_user",
     &one_per_user_only, -1, 2, true, false},
    {{{{ff::kMedia, 1}}}, "form_submissions", "submissions_media_unique", &bound_media_only, -1,
     1, true, false},

    // --- notifications ------------------------------------------------------
    //
    // The read-merge scan. `kind` and `subject` are equalities in every `$or`
    // branch and `_id` descending is the order within one, which is why the sort
    // names this index's own prefix rather than `_id` alone: a sort naming only
    // `_id` is satisfiable by the primary key, and the planner would then walk
    // every notification newest-first applying the topic predicate as a filter.
    {{{{nf::kKind, 1}, {nf::kSubject, 1}, {nf::kId, -1}}}, "notifications",
     "ntf_kind_subject_id", nullptr, -1, 3, false, false},

    // Idempotency. A retried publish finds this and becomes a no-op; without it
    // every at-least-once queue in the system eventually sends a notification
    // twice.
    {{{{nf::kDedupe, 1}}}, "notifications", "ntf_dedupe_unique", &dedupe_present_only, -1, 1,
     true, false},

    // The outbox sweep, partial on the marker it looks for.
    {{{{nf::kDispatchedAt, 1}, {nf::kId, 1}}}, "notifications", "ntf_undispatched",
     &undispatched_only, -1, 2, false, false},

    // Retention. A LIFETIME, which is why kCollections names the field as well
    // and every read filters on it explicitly.
    {{{{nf::kExpiresAt, 1}}}, "notifications", "ntf_ttl", nullptr, 0, 1, false, false},

    // --- notification_inbox -------------------------------------------------
    {{{{nf::kUid, 1}, {nf::kId, -1}}}, "notification_inbox", "inbox_uid_id", nullptr, -1, 2,
     false, false},
    {{{{nf::kUid, 1}, {nf::kReadAt, 1}}}, "notification_inbox", "inbox_uid_unread",
     &unread_only, -1, 2, false, false},
    // What makes a partially completed fan-out safe to re-run — which is exactly
    // what the outbox sweeper does after a crash.
    {{{{nf::kUid, 1}, {nf::kNid, 1}}}, "notification_inbox", "inbox_uid_nid", nullptr, -1, 2,
     true, false},
    {{{{nf::kExpiresAt, 1}}}, "notification_inbox", "inbox_ttl", nullptr, 0, 1, false, false},

    // --- notification_clients -----------------------------------------------
    {{{{nf::kOwner, 1}, {nf::kType, 1}}}, "notification_clients", "clients_owner_type",
     nullptr, -1, 2, false, false},
    // The fan-out lookup. Multikey over the subscription array, and the query
    // uses $elemMatch so a client holding {k, other} and {otherk, s} does not
    // match (k, s) — which would deliver a staff-only topic to the wrong client.
    {{{{"subs.kind", 1}, {"subs.subject", 1}, {nf::kId, 1}}}, "notification_clients",
     "clients_subs", nullptr, -1, 3, false, false},
    // An operator's webhook screen, and the type filter every client scan leads
    // with.
    {{{{nf::kType, 1}, {nf::kId, 1}}}, "notification_clients", "clients_type_id", nullptr, -1,
     2, false, false},

    // --- analytics_events ---------------------------------------------------
    //
    // Three, and each has exactly one query behind it. The raw collection has
    // only two readers — the rollup job and the erasure path — so a fourth index
    // here would be a write cost on the highest-volume collection in the system
    // with nothing reading it (docs/17-analytics.md §15). Six across the three
    // analytics collections, and query_catalogue_db_test asserts that every one
    // of them is actually ridden by a declared query.

    // The rollup's window walk: a range on `at` and a sort on (at, _id). Both
    // come from this index, so the server never sorts in memory, and it is what
    // the compound cursor is shaped to ride.
    {{{{aef::kAt, 1}, {aef::kId, 1}}}, "analytics_events", "events_at_id", nullptr, -1, 2,
     false, false},
    // Erasure, riding a partial index anonymous rows never enter.
    {{{{aef::kSubject, 1}}}, "analytics_events", "events_subject",
     &analytics_subject_present_only, -1, 1, false, false},
    // Retention. A LIFETIME, which is why kCollections names the field as well
    // and every read but the erasure filters on it explicitly.
    {{{{aef::kExpiresAt, 1}}}, "analytics_events", "events_ttl", nullptr, 0, 1, false, false},

    // --- analytics_sessions -------------------------------------------------
    //
    // The (visitor, day) pair is the `_id`, so the sessionisation needs no index
    // of its own: the primary key IS the unique constraint, and N instances
    // upserting it converge with no coordination.
    {{{{"_id.day", 1}, {asf::kExpiresAt, 1}}}, "analytics_sessions", "sessions_day", nullptr,
     -1, 2, false, false},
    {{{{asf::kExpiresAt, 1}}}, "analytics_sessions", "sessions_ttl", nullptr, 0, 1, false,
     false},

    // --- analytics_rollups --------------------------------------------------
    // Also keyed on the identity tuple, so the upsert is a primary-key write.
    // This one serves the READS: the equalities on code and granularity lead and
    // the bucket range trails, which is the order the planner needs to turn the
    // range into a bound rather than a residual.
    //
    // There is deliberately NO second index carrying `_id.dims`. The dimension
    // slots are an ARRAY, so an index over them is multikey — and a multikey
    // index cannot provide a sort on a key that follows it, which is exactly
    // what the bucket ordering needs. The planner correctly refuses such an
    // index, so shipping one would cost a write per rollup document to serve a
    // plan nothing chooses. The dimension-narrowed query applies its dimensions
    // as a residual over a range that is already bounded and limited.
    {{{{"_id.code", 1}, {"_id.gran", 1}, {"_id.bucket", 1}}}, "analytics_rollups",
     "rollups_code_gran_bucket", nullptr, -1, 3, false, false},

    // --- entries -----------------------------------------------------------
    //
    // Every listing, both stages, both directions: equality on the scope, the
    // stage as an equality or a two-value $in, then the order and its tiebreak.
    {{{{enf::kScope, 1}, {enf::kLive, 1}, {enf::kPosition, 1}, {enf::kId, 1}}}, "entries",
     "entries_listing", nullptr, -1, 4, false, false},
    // The URL identity. The constraint is the server's: two staff saving the
    // same slug at once are one insert and one Conflict, never two rows.
    {{{{enf::kKind, 1}, {enf::kSlug, 1}}}, "entries", "entries_slug_unique",
     &entry_slug_present_only, -1, 2, true, false},

    // --- chat (docs/22-chat.md §9.1) ----------------------------------------
    //
    // Sixteen, and each has one query behind it in tests/testapp/queries.h.
    {{{{ccf::kDirectPair, 1}}}, "chat_conversations", "chat_direct_pair", &direct_pairs_only,
     -1, 1, true, false},
    // The idempotent create: a retry with the same key finds the first (§3.1).
    {{{{ccf::kCreatedBy, 1}, {ccf::kClientId, 1}}}, "chat_conversations", "chat_created_cid",
     &keyed_creations_only, -1, 2, true, false},
    // Every membership check, and the member listing by user id.
    {{{{cmf::kConversation, 1}, {cmf::kUser, 1}}}, "chat_members", "chat_member_unique",
     nullptr, -1, 2, true, false},
    // The chat list: one person's memberships, archived or not, most recently
    // active first, with the member id as the tiebreak a cursor needs.
    {{{{cmf::kUser, 1}, {cmf::kArchived, 1}, {cmf::kActivity, -1}, {cmf::kId, -1}}},
     "chat_members", "chat_member_list", nullptr, -1, 4, false, false},
    {{{{cmf::kUser, 1}, {cmf::kPinned, 1}}}, "chat_members", "chat_member_pinned",
     &pinned_members_only, -1, 2, false, false},
    // Owner succession: the longest-standing holder of a role.
    {{{{cmf::kConversation, 1}, {cmf::kRole, 1}, {cmf::kJoinedSeq, 1}}}, "chat_members",
     "chat_member_role", nullptr, -1, 3, false, false},
    // "Read by" for one message.
    {{{{cmf::kConversation, 1}, {cmf::kRead, 1}}}, "chat_members", "chat_member_read", nullptr,
     -1, 2, false, false},
    // "Delivered to" for one message, beside it (§5.1).
    {{{{cmf::kConversation, 1}, {cmf::kDelivered, 1}}}, "chat_members", "chat_member_delivered",
     nullptr, -1, 2, false, false},
    // One person's conversations by id: a device change walks every one of
    // them, paged by a key that does not move while it walks (§7.4).
    {{{{cmf::kUser, 1}, {cmf::kConversation, 1}}}, "chat_members", "chat_member_of_user",
     nullptr, -1, 2, false, false},
    // The log. Unique, so a seq names one message however a write raced.
    {{{{cgf::kConversation, 1}, {cgf::kSeq, 1}}}, "chat_messages", "chat_message_seq", nullptr,
     -1, 2, true, false},
    // The idempotent send: a retry with the same client id finds the first.
    {{{{cgf::kConversation, 1}, {cgf::kSender, 1}, {cgf::kClientId, 1}}}, "chat_messages",
     "chat_message_cid", nullptr, -1, 3, true, false},
    {{{{cgf::kExpiresAt, 1}, {cgf::kId, 1}}}, "chat_messages", "chat_message_expiry",
     &expiring_messages_only, -1, 2, false, false},
    // The mutation catch-up: one conversation's changed messages in the order
    // they changed (§4.5).
    {{{{cgf::kConversation, 1}, {cgf::kMutation, 1}}}, "chat_messages", "chat_message_mutation",
     &mutated_messages_only, -1, 2, false, false},
    {{{{crf::kConversation, 1}, {crf::kSeq, 1}, {crf::kUser, 1}}}, "chat_reactions",
     "chat_reaction_unique", nullptr, -1, 3, true, false},
    // A garbage collector for spent links; the redeem filter is what refuses one.
    {{{{cif::kExpiresAt, 1}}}, "chat_invites", "chat_invite_ttl", nullptr, 0, 1, false, false},
    {{{{cbf::kBlocker, 1}, {cbf::kBlocked, 1}}}, "chat_blocks", "chat_block_unique", nullptr,
     -1, 2, true, false},
    // One report per range per reporter, so a retried report is the first
    // (§9.2). The staff listing walks _id, which needs no index of its own.
    {{{{cpr::kConversation, 1}, {cpr::kReporter, 1}, {cpr::kFrom, 1}, {cpr::kTo, 1}}},
     "chat_reports", "chat_report_range", nullptr, -1, 4, true, false},

    // --- chat devices (docs/22-chat.md §7.3) --------------------------------
    //
    // A device id names one device anywhere: the server refuses a second
    // account linking an id somebody else holds. No read uses it.
    {{{{cdf::kDeviceIdPath, 1}}}, "chat_identities", "chat_identity_device",
     &accounts_with_devices_only, -1, 1, true, false},
    // The idle sweeper (multikey: one key per device).
    {{{{cdf::kLastSeenPath, 1}}}, "chat_identities", "chat_identity_seen", nullptr, -1, 1,
     false, false},
    // One key id per device, and the claim's find_one_and_delete by device
    // rides its prefix.
    {{{{cpf::kDevice, 1}, {cpf::kKeyId, 1}}}, "chat_prekeys", "chat_prekey_unique", nullptr,
     -1, 2, true, false},
    // The device-change sweeper, oldest first (§7.4).
    {{{{cdf::kPendingSince, 1}}}, "chat_identities", "chat_identity_pending",
     &pending_device_changes_only, -1, 1, false, false},
    // One device's queue in send order: the read, and the acknowledgement's
    // range delete (§7.6).
    {{{{cqf::kDevice, 1}, {cqf::kId, 1}}}, "chat_device_queue", "chat_queue_device", nullptr,
     -1, 2, false, false},
    // One message's rows, which a revoke removes with its common ciphertext.
    {{{{cqf::kConversation, 1}, {cqf::kSeq, 1}}}, "chat_device_queue", "chat_queue_message",
     nullptr, -1, 2, false, false},
    // The garbage collector for a device that never comes back. Not the access
    // control: the read filters the expiry too.
    {{{{cqf::kExpiresAt, 1}}}, "chat_device_queue", "chat_queue_ttl", nullptr, 0, 1, false,
     false},
    // The link relay: a session's earlier request, which its next one
    // replaces, and the collector for one nobody approved (§7.3.1). Every
    // other read is by the token's digest, the _id.
    {{{{clf::kUser, 1}, {clf::kSession, 1}}}, "chat_link_requests", "chat_link_session",
     nullptr, -1, 2, false, false},
    {{{{clf::kExpiresAt, 1}}}, "chat_link_requests", "chat_link_ttl", nullptr, 0, 1, false,
     false},
    // --- end chat devices ---------------------------------------------------
}};

static_assert(anvil::db::catalogue_is_well_formed(kIndexes),
              "empty name or collection, a zero or over-long key count, a direction that is "
              "neither 1 nor -1, or two indexes sharing a name on one collection");

// Indexes a previous version created and this one no longer wants.
//
// Named here rather than simply deleted from the catalogue above, because
// deleting the entry stops CREATING it and leaves it in place on every cluster
// that already has one. Naming it here removes it.
//
// It matters most for a UNIQUE index: a superseded constraint that survives keeps
// rejecting writes against a rule nobody meant to still be in force, and the
// rejection looks exactly like a legitimate conflict.
inline constexpr std::array<anvil::db::RetiredIndex, 2> kRetiredIndexes{{
    {"users", "users_email_legacy"},
    {"users", "users_status_legacy"},
}};

// Bumped when an index is added, removed or respecified. Recorded in each
// database's schema_meta so a deploy can tell whether migrations have run against
// a given cluster.
inline constexpr std::int32_t kSchemaVersion = 9;

}  // namespace testapp
