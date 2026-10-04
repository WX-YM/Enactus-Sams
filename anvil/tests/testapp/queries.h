#pragma once

// The reference application's query catalogue: every query shape it issues,
// explained against a live server and asserted not to scan.
//
// It is the counterpart of tests/testapp/indexes.h, and the pair is the whole
// contract from CLAUDE.md §7 — adding a query without adding its index in the
// same commit is not allowed, and this is what makes that a test failure rather
// than a rule somebody remembers.
//
// The filter VALUES do not matter. A planner picks a plan from the query's
// SHAPE, not from what it matches, so a spec built with placeholder ids proves
// exactly what it needs to on an empty collection.

#include <array>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/analytics/event.h"
#include "anvil/audit/record.h"
#include "anvil/chat/device_queue.h"
#include "anvil/chat/devices.h"
#include "anvil/chat/prekeys.h"
#include "anvil/chat/record.h"
#include "anvil/db/query_catalogue.h"
#include "anvil/entries/document.h"
#include "anvil/forms/repository.h"
#include "anvil/identity/capabilities.h"
#include "anvil/identity/sessions.h"
#include "anvil/identity/user_fields.h"
#include "anvil/identity/verification.h"
#include "anvil/media/record.h"
#include "anvil/notifications/repository.h"
#include "anvil/sections/repository.h"

namespace testapp::queries {

namespace uf = anvil::identity::fields;
namespace sf = anvil::identity::session_fields;
namespace cf = anvil::identity::capability_fields;
namespace vf = anvil::identity::verification_fields;
namespace af = anvil::audit::audit_fields;
namespace mf = anvil::media::media_fields;
namespace ff = anvil::forms::form_fields;
namespace nf = anvil::notifications::notification_fields;
namespace secf = anvil::sections::section_fields;
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

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

namespace detail {

[[nodiscard]] inline bsoncxx::types::b_binary placeholder_uuid() noexcept {
    static constexpr std::array<std::uint8_t, 16> kBytes{};
    return bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_uuid, 16, kBytes.data()};
}

[[nodiscard]] inline bsoncxx::types::b_binary placeholder_digest() noexcept {
    static constexpr std::array<std::uint8_t, 32> kBytes{};
    return bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary, 32, kBytes.data()};
}

[[nodiscard]] inline bsoncxx::types::b_binary placeholder_digest_16() noexcept {
    static constexpr std::array<std::uint8_t, 16> kBytes{};
    return bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary, 16, kBytes.data()};
}

[[nodiscard]] inline bsoncxx::types::b_date placeholder_time() noexcept {
    return bsoncxx::types::b_date{std::chrono::milliseconds{0}};
}

}  // namespace detail

// --- users ------------------------------------------------------------------

[[nodiscard]] inline bsoncxx::document::value login_by_email() {
    return make_document(kvp(anvil::db::codec::key_of(uf::kEmailNormalised),
                             bsoncxx::types::b_string{"someone@example.test"}));
}

[[nodiscard]] inline bsoncxx::document::value login_by_username() {
    return make_document(kvp(anvil::db::codec::key_of(uf::kUsernameNormalised),
                             bsoncxx::types::b_string{"someone"}));
}

[[nodiscard]] inline bsoncxx::document::value login_by_phone() {
    return make_document(
        kvp(anvil::db::codec::key_of(uf::kPhone), bsoncxx::types::b_string{"+201000000000"}));
}

[[nodiscard]] inline bsoncxx::document::value account_listing() {
    return make_document(kvp(anvil::db::codec::key_of(uf::kUserType),
                             bsoncxx::types::b_int32{1}));
}

[[nodiscard]] inline bsoncxx::document::value account_listing_sort() {
    return make_document(kvp(anvil::db::codec::key_of(uf::kUserType), 1),
                         kvp(anvil::db::codec::key_of(uf::kId), 1));
}

// Repeats the partial index's filter VERBATIM. A predicate that means the same
// thing in different words does not prove to the planner that the query is a
// subset of the filter, so it falls back to a scan.
[[nodiscard]] inline bsoncxx::document::value stale_pending_sweep() {
    return make_document(
        kvp(anvil::db::codec::key_of(uf::kStatus),
            bsoncxx::types::b_int32{
                static_cast<std::int32_t>(anvil::UserStatus::PendingVerification)}),
        kvp(anvil::db::codec::key_of(uf::kCreatedAt), [](sub_document sub) {
            sub.append(kvp("$lt", detail::placeholder_time()));
        }));
}

// --- user_sessions ----------------------------------------------------------

[[nodiscard]] inline bsoncxx::document::value refresh_current() {
    return make_document(kvp(anvil::db::codec::key_of(sf::kRefreshHash),
                             detail::placeholder_digest()),
                         kvp(anvil::db::codec::key_of(sf::kRevoked),
                             bsoncxx::types::b_bool{false}),
                         kvp(anvil::db::codec::key_of(sf::kExpiresAt), [](sub_document sub) {
                             sub.append(kvp("$gt", detail::placeholder_time()));
                         }));
}

[[nodiscard]] inline bsoncxx::document::value refresh_previous() {
    return make_document(kvp(anvil::db::codec::key_of(sf::kPreviousHash),
                             detail::placeholder_digest()),
                         kvp(anvil::db::codec::key_of(sf::kRevoked),
                             bsoncxx::types::b_bool{false}),
                         kvp(anvil::db::codec::key_of(sf::kExpiresAt), [](sub_document sub) {
                             sub.append(kvp("$gt", detail::placeholder_time()));
                         }));
}

[[nodiscard]] inline bsoncxx::document::value sessions_for_user() {
    return make_document(kvp(anvil::db::codec::key_of(sf::kUserId), detail::placeholder_uuid()),
                         kvp(anvil::db::codec::key_of(sf::kRevoked),
                             bsoncxx::types::b_bool{false}),
                         kvp(anvil::db::codec::key_of(sf::kExpiresAt), [](sub_document sub) {
                             sub.append(kvp("$gt", detail::placeholder_time()));
                         }));
}

// A bulk revocation's page of ids. No expiry term: an expired row is still
// readable until the TTL monitor reaches it, and revoking it is what a "sign out
// everywhere" must do.
[[nodiscard]] inline bsoncxx::document::value sessions_to_revoke() {
    return make_document(kvp(anvil::db::codec::key_of(sf::kUserId), detail::placeholder_uuid()),
                         kvp(anvil::db::codec::key_of(sf::kRevoked),
                             bsoncxx::types::b_bool{false}));
}

[[nodiscard]] inline bsoncxx::document::value sessions_for_user_sort() {
    return make_document(kvp(anvil::db::codec::key_of(sf::kLastSeen), -1));
}

// --- capability_tokens ------------------------------------------------------

[[nodiscard]] inline bsoncxx::document::value capability_consume() {
    return make_document(kvp(anvil::db::codec::key_of(cf::kHash), detail::placeholder_digest()),
                         kvp(anvil::db::codec::key_of(cf::kUsedAt), bsoncxx::types::b_null{}),
                         kvp(anvil::db::codec::key_of(cf::kExpiresAt), [](sub_document sub) {
                             sub.append(kvp("$gt", detail::placeholder_time()));
                         }));
}

// --- email_verifications ----------------------------------------------------

[[nodiscard]] inline bsoncxx::document::value verification_consume() {
    return make_document(
        kvp(anvil::db::codec::key_of(vf::kAddressHash), detail::placeholder_digest()),
        kvp(anvil::db::codec::key_of(vf::kCodeHash), detail::placeholder_digest()),
        kvp(anvil::db::codec::key_of(vf::kUsedAt), bsoncxx::types::b_null{}),
        kvp(anvil::db::codec::key_of(vf::kExpiresAt), [](sub_document sub) {
            sub.append(kvp("$gt", detail::placeholder_time()));
        }));
}

// --- media ------------------------------------------------------------------

[[nodiscard]] inline bsoncxx::document::value media_dedup() {
    return make_document(
        kvp(anvil::db::codec::key_of(mf::kNamespace), bsoncxx::types::b_int32{0}),
        kvp(anvil::db::codec::key_of(mf::kSha256), detail::placeholder_digest()),
        kvp(anvil::db::codec::key_of(mf::kSource),
            make_document(kvp("$exists", bsoncxx::types::b_bool{false}))));
}

// The same edit of the same source, which is how a retried edit finds the
// object its first attempt made.
[[nodiscard]] inline bsoncxx::document::value media_edit_lookup() {
    return make_document(
        kvp(anvil::db::codec::key_of(mf::kNamespace), bsoncxx::types::b_int32{0}),
        kvp(anvil::db::codec::key_of(mf::kSource), detail::placeholder_uuid()),
        kvp(anvil::db::codec::key_of(mf::kEditSha), detail::placeholder_digest()));
}

[[nodiscard]] inline bsoncxx::document::value media_listing() {
    return make_document(
        kvp(anvil::db::codec::key_of(mf::kNamespace), bsoncxx::types::b_int32{0}));
}

[[nodiscard]] inline bsoncxx::document::value media_listing_sort() {
    return make_document(kvp(anvil::db::codec::key_of(mf::kCreatedAt), -1),
                         kvp(anvil::db::codec::key_of(mf::kId), -1));
}

// --- sections ---------------------------------------------------------------

// The only shape this collection is ever read by: an equality on the compound
// `_id`. Field ORDER inside an embedded-document `_id` is significant to
// MongoDB, so this spells `{k, s}` exactly as the repository builds it — a
// filter spelling `{s, k}` is a different key and would scan.
//
// Declared even though the primary-key index is free, because "free" is an
// assumption about the shape rather than about the collection: a read that
// filtered on `_id.k` alone would not use it, and the symptom would be a
// COLLSCAN on the hottest read path in a CMS rather than an error.
[[nodiscard]] inline bsoncxx::document::value section_by_key_and_state() {
    return make_document(
        kvp(anvil::db::codec::key_of(secf::kId), [](sub_document sub) {
            sub.append(kvp(anvil::db::codec::key_of(secf::kIdKey),
                           bsoncxx::types::b_string{"home.hero"}));
            sub.append(kvp(anvil::db::codec::key_of(secf::kIdState), bsoncxx::types::b_int32{0}));
        }));
}


// --- entries ----------------------------------------------------------------
//
// Every shape anvil/entries/repository.cc issues, spelled as it spells them.
// The four that list share one index, `{sc, live, o, _id}`, and each of them
// must be answered by walking it in order — a SORT stage here would be an
// in-memory sort of a whole scope on every page of a feed.

[[nodiscard]] inline bsoncxx::document::value entry_page_filter() {
    return make_document(kvp(anvil::db::codec::key_of(enf::kScope),
                             bsoncxx::types::b_string{"blog.post"}),
                         kvp(anvil::db::codec::key_of(enf::kLive), bsoncxx::types::b_bool{true}),
                         kvp(anvil::db::codec::key_of(enf::kPosition),
                             bsoncxx::types::b_int64{0}));
}

// A time-ordered page after a cursor: the range continues the index into `_id`.
[[nodiscard]] inline bsoncxx::document::value entry_cursor_filter() {
    return make_document(
        kvp(anvil::db::codec::key_of(enf::kScope), bsoncxx::types::b_string{"blog.post"}),
        kvp(anvil::db::codec::key_of(enf::kLive), bsoncxx::types::b_bool{true}),
        kvp(anvil::db::codec::key_of(enf::kPosition), bsoncxx::types::b_int64{0}),
        kvp(anvil::db::codec::key_of(enf::kId), [](sub_document sub) {
            sub.append(kvp("$lt", detail::placeholder_uuid()));
        }));
}

[[nodiscard]] inline bsoncxx::document::value entry_newest_sort() {
    return make_document(kvp(anvil::db::codec::key_of(enf::kPosition), -1),
                         kvp(anvil::db::codec::key_of(enf::kId), -1));
}

// An editor's listing of a manual scope: both stages as a two-value $in, which
// the planner answers by merging two ordered scans rather than sorting.
[[nodiscard]] inline bsoncxx::document::value entry_working_filter() {
    return make_document(
        kvp(anvil::db::codec::key_of(enf::kScope), bsoncxx::types::b_string{"gallery.item"}),
        kvp(anvil::db::codec::key_of(enf::kLive), [](sub_document sub) {
            sub.append(kvp("$in", bsoncxx::builder::basic::make_array(
                                      bsoncxx::types::b_bool{false},
                                      bsoncxx::types::b_bool{true})));
        }));
}

[[nodiscard]] inline bsoncxx::document::value entry_manual_sort() {
    return make_document(kvp(anvil::db::codec::key_of(enf::kPosition), 1),
                         kvp(anvil::db::codec::key_of(enf::kId), 1));
}

// The pinned few: a flag test is a residual over the scope's range.
[[nodiscard]] inline bsoncxx::document::value entry_flagged_filter() {
    return make_document(
        kvp(anvil::db::codec::key_of(enf::kScope), bsoncxx::types::b_string{"gallery.item"}),
        kvp(anvil::db::codec::key_of(enf::kLive), bsoncxx::types::b_bool{true}),
        kvp(anvil::db::codec::key_of(enf::kFlags), [](sub_document sub) {
            sub.append(kvp("$bitsAllSet", bsoncxx::types::b_int32{1}));
        }));
}

[[nodiscard]] inline bsoncxx::document::value entry_by_id() {
    return make_document(
        kvp(anvil::db::codec::key_of(enf::kId), detail::placeholder_uuid()),
        kvp(anvil::db::codec::key_of(enf::kKind), bsoncxx::types::b_string{"blog.post"}));
}

[[nodiscard]] inline bsoncxx::document::value entry_by_slug() {
    return make_document(
        kvp(anvil::db::codec::key_of(enf::kKind), bsoncxx::types::b_string{"blog.post"}),
        kvp(anvil::db::codec::key_of(enf::kSlug), bsoncxx::types::b_string{"hello"}),
        kvp(anvil::db::codec::key_of(enf::kLive), bsoncxx::types::b_bool{true}));
}

// The capacity count and the reorder snapshot: a prefix of the listing index.
[[nodiscard]] inline bsoncxx::document::value entry_scope() {
    return make_document(
        kvp(anvil::db::codec::key_of(enf::kScope), bsoncxx::types::b_string{"gallery.item"}));
}

// --- form_definitions -------------------------------------------------------

// The staff listing: ordered and paginated by `_id` alone, which is descending
// creation time because form ids are UUIDv7. Declared with no index behind it on
// purpose — the primary key provides both the order and the cursor, and this is
// what proves that claim against a real planner rather than leaving it as one.
[[nodiscard]] inline bsoncxx::document::value form_listing() {
    return make_document(kvp(anvil::db::codec::key_of(ff::kId), [](sub_document sub) {
        sub.append(kvp("$lt", detail::placeholder_uuid()));
    }));
}

[[nodiscard]] inline bsoncxx::document::value form_listing_sort() {
    return make_document(kvp(anvil::db::codec::key_of(ff::kId), -1));
}

[[nodiscard]] inline bsoncxx::document::value forms_by_creator() {
    return make_document(
        kvp(anvil::db::codec::key_of(ff::kCreator), detail::placeholder_uuid()),
        kvp(anvil::db::codec::key_of(ff::kStatus), bsoncxx::types::b_int32{1}));
}

// --- form_submissions -------------------------------------------------------

// The first page of one form's submissions. The discriminator is in EVERY filter
// against this collection — it is what makes one partitioned collection safe
// where one collection per form was not.
[[nodiscard]] inline bsoncxx::document::value submissions_first_page() {
    return make_document(kvp(anvil::db::codec::key_of(ff::kForm), detail::placeholder_uuid()));
}

[[nodiscard]] inline bsoncxx::document::value submissions_page_sort() {
    return make_document(kvp(anvil::db::codec::key_of(ff::kSubmittedAt), -1),
                         kvp(anvil::db::codec::key_of(ff::kId), -1));
}

// The cursor page. The `$or` is the standard two-branch keyset cursor — strictly
// older, or the same instant with a smaller id — and it is here as its own entry
// because an `$or` is exactly the shape that can stop riding an index without
// anything saying so.
[[nodiscard]] inline bsoncxx::document::value submissions_cursor_page() {
    return make_document(
        kvp(anvil::db::codec::key_of(ff::kForm), detail::placeholder_uuid()),
        kvp("$or", [](bsoncxx::builder::basic::sub_array branches) {
            branches.append([](sub_document sub) {
                sub.append(kvp(anvil::db::codec::key_of(ff::kSubmittedAt),
                               [](sub_document range) {
                                   range.append(kvp("$lt", detail::placeholder_time()));
                               }));
            });
            branches.append([](sub_document sub) {
                sub.append(kvp(anvil::db::codec::key_of(ff::kSubmittedAt),
                               detail::placeholder_time()));
                sub.append(kvp(anvil::db::codec::key_of(ff::kId), [](sub_document range) {
                    range.append(kvp("$lt", detail::placeholder_uuid()));
                }));
            });
        }));
}

// Duplicate detection, answered without decrypting anything. Repeats the partial
// index's `$exists` filter implicitly through the equality: an equality on a field
// proves it exists, which is what lets the planner use a partial index whose
// filter is exactly that existence.
[[nodiscard]] inline bsoncxx::document::value submissions_blind_index() {
    return make_document(
        kvp(anvil::db::codec::key_of(ff::kForm), detail::placeholder_uuid()),
        kvp(anvil::db::codec::key_of(ff::kPiiIndex), detail::placeholder_digest()));
}


// --- notifications ----------------------------------------------------------

// ONE branch of the broadcast read-merge, which is exactly what the code issues.
//
// The read is a union: every subscribed fan-out-on-read topic gets its own
// bounded `$match`/`$sort`/`$limit` and the results are merged. So a single
// branch is not an approximation of the query — it IS the query, once per
// subscribed topic, and its plan is the one that decides whether the read is
// bounded.
//
// What must hold is an IXSCAN on ntf_kind_subject_id with the sort provided by
// the index rather than a blocking SORT: `kind` and `subject` are point
// equalities, so `_id` descending falls out of the index for free. The subject is
// a nil BinData and not null for that reason — `{subject: null}` matches
// missing-or-null, which is not an equality and costs the elision.
[[nodiscard]] inline bsoncxx::document::value broadcast_branch() {
    return make_document(
        kvp(anvil::db::codec::key_of(nf::kKind), bsoncxx::types::b_int32{0}),
        kvp(anvil::db::codec::key_of(nf::kSubject),
            anvil::db::codec::uuid_bin(anvil::kNilUuid)),
        kvp(anvil::db::codec::key_of(nf::kId),
            [](sub_document sub) { sub.append(kvp("$gt", detail::placeholder_uuid())); }),
        kvp(anvil::db::codec::key_of(nf::kExpiresAt),
            [](sub_document sub) { sub.append(kvp("$gt", detail::placeholder_time())); }));
}

// `_id` descending across every branch, which is what makes the page the newest
// rows the reader can see rather than one topic's feed. It is satisfied by
// ntf_kind_subject_id because each branch pins the two leading fields to
// equalities; this entry exists to catch the day it stops being satisfied and
// becomes a blocking sort.
[[nodiscard]] inline bsoncxx::document::value broadcast_branch_sort() {
    return make_document(kvp(anvil::db::codec::key_of(nf::kId), -1));
}

// Repeats the partial index's filter VERBATIM. `disp_at: null` is the predicate
// the index was built on, and a query that meant the same thing in different
// words would not prove to the planner that it is a subset of it.
[[nodiscard]] inline bsoncxx::document::value outbox_sweep() {
    return make_document(
        kvp(anvil::db::codec::key_of(nf::kDispatchedAt), bsoncxx::types::b_null{}),
        kvp(anvil::db::codec::key_of(nf::kId),
            [](sub_document sub) { sub.append(kvp("$lt", detail::placeholder_uuid())); }),
        kvp(anvil::db::codec::key_of(nf::kExpiresAt),
            [](sub_document sub) { sub.append(kvp("$gt", detail::placeholder_time())); }));
}

// The idempotent-retry read. The expiry predicate is a residual over the unique
// dedupe index rather than part of it, and it is here because it is in the query:
// the TTL monitor lags by up to a minute, so a publish that lost the insert race
// to an expiring row must not be handed that row back (CLAUDE.md §7).
[[nodiscard]] inline bsoncxx::document::value notification_dedupe() {
    return make_document(
        kvp(anvil::db::codec::key_of(nf::kDedupe), detail::placeholder_digest_16()),
        kvp(anvil::db::codec::key_of(nf::kExpiresAt),
            [](sub_document sub) { sub.append(kvp("$gt", detail::placeholder_time())); }));
}

// --- notification_inbox -----------------------------------------------------

[[nodiscard]] inline bsoncxx::document::value inbox_page() {
    return make_document(
        kvp(anvil::db::codec::key_of(nf::kUid), detail::placeholder_uuid()),
        kvp(anvil::db::codec::key_of(nf::kId),
            [](sub_document sub) { sub.append(kvp("$lt", detail::placeholder_uuid())); }),
        kvp(anvil::db::codec::key_of(nf::kExpiresAt),
            [](sub_document sub) { sub.append(kvp("$gt", detail::placeholder_time())); }));
}

[[nodiscard]] inline bsoncxx::document::value inbox_page_sort() {
    return make_document(kvp(anvil::db::codec::key_of(nf::kId), -1));
}

[[nodiscard]] inline bsoncxx::document::value inbox_unread_count() {
    return make_document(
        kvp(anvil::db::codec::key_of(nf::kUid), detail::placeholder_uuid()),
        kvp(anvil::db::codec::key_of(nf::kReadAt), bsoncxx::types::b_null{}),
        kvp(anvil::db::codec::key_of(nf::kExpiresAt),
            [](sub_document sub) { sub.append(kvp("$gt", detail::placeholder_time())); }));
}

// --- notification_clients ---------------------------------------------------

// $elemMatch and not two dotted predicates: the distinction is whether a client
// holding {k, other} and {otherk, s} matches (k, s), and it is the difference
// between delivering a staff-only topic to the right client and to a wrong one.
[[nodiscard]] inline bsoncxx::document::value subscriber_scan() {
    return make_document(
        kvp(anvil::db::codec::key_of(nf::kSubs),
            [](sub_document sub) {
                sub.append(kvp("$elemMatch", [](sub_document match) {
                    match.append(kvp(anvil::db::codec::key_of(nf::kSubKind),
                                     bsoncxx::types::b_int32{0}));
                    match.append(kvp(anvil::db::codec::key_of(nf::kSubSubject),
                                     bsoncxx::types::b_null{}));
                }));
            }),
        kvp(anvil::db::codec::key_of(nf::kDisabledAt), bsoncxx::types::b_null{}));
}

[[nodiscard]] inline bsoncxx::document::value subscriber_scan_sort() {
    return make_document(kvp(anvil::db::codec::key_of(nf::kId), 1));
}

[[nodiscard]] inline bsoncxx::document::value inapp_client_lookup() {
    return make_document(
        kvp(anvil::db::codec::key_of(nf::kOwner), detail::placeholder_uuid()),
        kvp(anvil::db::codec::key_of(nf::kType), bsoncxx::types::b_int32{0}));
}

// A chat nudge's endpoints: a page of recipients in one `$in` (chat/push.h).
[[nodiscard]] inline bsoncxx::document::value push_targets() {
    return make_document(
        kvp(anvil::db::codec::key_of(nf::kOwner),
            make_document(kvp("$in", bsoncxx::builder::basic::make_array(
                                         detail::placeholder_uuid(), detail::placeholder_uuid())))),
        kvp(anvil::db::codec::key_of(nf::kType), bsoncxx::types::b_int32{1}),
        kvp(anvil::db::codec::key_of(nf::kDisabledAt), bsoncxx::types::b_null{}));
}

[[nodiscard]] inline bsoncxx::document::value webhook_listing() {
    return make_document(
        kvp(anvil::db::codec::key_of(nf::kType), bsoncxx::types::b_int32{3}));
}

[[nodiscard]] inline bsoncxx::document::value webhook_listing_sort() {
    return make_document(kvp(anvil::db::codec::key_of(nf::kId), 1));
}

// --- audit_log --------------------------------------------------------------

[[nodiscard]] inline bsoncxx::document::value audit_by_actor() {
    return make_document(kvp(anvil::db::codec::key_of(af::kActor), detail::placeholder_uuid()));
}

[[nodiscard]] inline bsoncxx::document::value audit_by_actor_sort() {
    return make_document(kvp(anvil::db::codec::key_of(af::kAt), -1),
                         kvp(anvil::db::codec::key_of(af::kId), -1));
}

// --- analytics --------------------------------------------------------------
//
// Six shapes, and they are the WHOLE read surface of the subsystem: the raw
// collection has exactly two readers — the rollup job and the erasure path — and
// a dashboard reads rollups, never rows (docs/17-analytics.md §15).

// The rollup's window walk, resumed. The $or is the only resume clause that is
// both total and expressible against (at, _id): `at >= cursor.at AND _id > id`
// silently drops every row at a later instant whose id sorts lower.
[[nodiscard]] inline bsoncxx::document::value analytics_window_page() {
    return make_document(
        kvp(anvil::db::codec::key_of(aef::kAt),
            make_document(kvp("$gte", detail::placeholder_time()),
                          kvp("$lt", detail::placeholder_time()))),
        kvp(anvil::db::codec::key_of(aef::kExpiresAt),
            make_document(kvp("$gt", detail::placeholder_time()))),
        kvp("$or", bsoncxx::builder::basic::make_array(
                       make_document(kvp(anvil::db::codec::key_of(aef::kAt),
                                         make_document(kvp("$gt",
                                                           detail::placeholder_time())))),
                       make_document(kvp(anvil::db::codec::key_of(aef::kAt),
                                         detail::placeholder_time()),
                                     kvp(anvil::db::codec::key_of(aef::kId),
                                         make_document(kvp("$gt",
                                                           detail::placeholder_uuid())))))));
}

[[nodiscard]] inline bsoncxx::document::value analytics_window_sort() {
    return make_document(kvp(anvil::db::codec::key_of(aef::kAt), 1),
                         kvp(anvil::db::codec::key_of(aef::kId), 1));
}

// Erasure. Deliberately carries NO expiry predicate: it must reach rows the TTL
// monitor has not collected yet, which is the one read here where "still
// physically present" is exactly what is being asked about.
[[nodiscard]] inline bsoncxx::document::value analytics_erase_subject() {
    return make_document(
        kvp(anvil::db::codec::key_of(aef::kSubject), detail::placeholder_uuid()));
}

[[nodiscard]] inline bsoncxx::document::value analytics_sessions_by_day() {
    return make_document(kvp("_id.day", bsoncxx::types::b_int32{0}),
                         kvp(anvil::db::codec::key_of(asf::kExpiresAt),
                             make_document(kvp("$gt", detail::placeholder_time()))));
}

[[nodiscard]] inline bsoncxx::document::value analytics_rollup_series() {
    return make_document(kvp("_id.code", bsoncxx::types::b_int32{0}),
                         kvp("_id.gran", bsoncxx::types::b_int32{1}),
                         kvp("_id.bucket",
                             make_document(kvp("$gte", detail::placeholder_time()),
                                           kvp("$lt", detail::placeholder_time()))));
}

[[nodiscard]] inline bsoncxx::document::value analytics_rollup_series_sort() {
    return make_document(kvp("_id.bucket", 1));
}

[[nodiscard]] inline bsoncxx::document::value analytics_rollup_series_for_dims() {
    return make_document(
        kvp("_id.code", bsoncxx::types::b_int32{0}),
        kvp("_id.gran", bsoncxx::types::b_int32{1}),
        kvp("_id.dims", bsoncxx::builder::basic::make_array(
                            bsoncxx::types::b_int32{0}, bsoncxx::types::b_int32{0},
                            bsoncxx::types::b_int32{255}, bsoncxx::types::b_int32{255})),
        kvp("_id.bucket", make_document(kvp("$gte", detail::placeholder_time()),
                                        kvp("$lt", detail::placeholder_time()))));
}

// The rollup's write filter, which is a primary-key equality and therefore the
// cheapest shape in the system. It is in the catalogue anyway: an upsert that
// stopped being a point query would be a full scan per bucket per run, and
// nothing else would report it.
[[nodiscard]] inline bsoncxx::document::value analytics_rollup_upsert() {
    return make_document(
        kvp("_id", make_document(kvp(anvil::db::codec::key_of(aef::kCode),
                                     bsoncxx::types::b_int32{0}),
                                 kvp("gran", bsoncxx::types::b_int32{1}),
                                 kvp("bucket", detail::placeholder_time()),
                                 kvp("dims", bsoncxx::builder::basic::make_array(
                                                 bsoncxx::types::b_int32{0},
                                                 bsoncxx::types::b_int32{0},
                                                 bsoncxx::types::b_int32{255},
                                                 bsoncxx::types::b_int32{255})))));
}

// --- chat (anvil/chat/repository.h) -------------------------------------------

namespace chat_detail {

[[nodiscard]] inline bsoncxx::stdx::string_view k(std::string_view name) noexcept {
    return anvil::db::codec::key_of(name);
}

[[nodiscard]] inline bsoncxx::document::value not_present() {
    return make_document(kvp("$exists", bsoncxx::types::b_bool{false}));
}

// "Not expired": no expiry, or one in the future. Every message read carries it.
[[nodiscard]] inline bsoncxx::document::value live_branches() {
    return make_document(kvp(
        "$or", bsoncxx::builder::basic::make_array(
                   make_document(kvp(k(cgf::kExpiresAt), not_present())),
                   make_document(kvp(k(cgf::kExpiresAt),
                                     make_document(kvp("$gt", detail::placeholder_time())))))));
}

}  // namespace chat_detail

[[nodiscard]] inline bsoncxx::document::value chat_direct_pair() {
    return make_document(kvp(chat_detail::k(ccf::kDirectPair), detail::placeholder_digest()));
}

[[nodiscard]] inline bsoncxx::document::value chat_created() {
    return make_document(kvp(chat_detail::k(ccf::kCreatedBy), detail::placeholder_uuid()),
                         kvp(chat_detail::k(ccf::kClientId), detail::placeholder_digest_16()));
}

[[nodiscard]] inline bsoncxx::document::value chat_conversations_in() {
    return make_document(kvp(chat_detail::k(ccf::kId),
                             make_document(kvp("$in", bsoncxx::builder::basic::make_array(
                                                          detail::placeholder_uuid())))));
}

[[nodiscard]] inline bsoncxx::document::value chat_membership() {
    return make_document(kvp(chat_detail::k(cmf::kConversation), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cmf::kUser), detail::placeholder_uuid()));
}

// Current members of one conversation: the count, the activity bump's
// update_many, and (with the cursor) the member listing.
[[nodiscard]] inline bsoncxx::document::value chat_current_members() {
    return make_document(kvp(chat_detail::k(cmf::kConversation), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cmf::kLeftSeq), chat_detail::not_present()));
}

[[nodiscard]] inline bsoncxx::document::value chat_members_in() {
    return make_document(
        kvp(chat_detail::k(cmf::kConversation), detail::placeholder_uuid()),
        kvp(chat_detail::k(cmf::kUser), make_document(kvp("$in", bsoncxx::builder::basic::make_array(
                                                                     detail::placeholder_uuid())))),
        kvp(chat_detail::k(cmf::kLeftSeq), chat_detail::not_present()));
}

[[nodiscard]] inline bsoncxx::document::value chat_member_page() {
    return make_document(
        kvp(chat_detail::k(cmf::kConversation), detail::placeholder_uuid()),
        kvp(chat_detail::k(cmf::kUser), make_document(kvp("$gt", detail::placeholder_uuid()))),
        kvp(chat_detail::k(cmf::kLeftSeq), chat_detail::not_present()));
}

[[nodiscard]] inline bsoncxx::document::value chat_member_page_sort() {
    return make_document(kvp(chat_detail::k(cmf::kConversation), 1),
                         kvp(chat_detail::k(cmf::kUser), 1));
}

[[nodiscard]] inline bsoncxx::document::value chat_senior() {
    return make_document(kvp(chat_detail::k(cmf::kConversation), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cmf::kRole), bsoncxx::types::b_int32{1}),
                         kvp(chat_detail::k(cmf::kLeftSeq), chat_detail::not_present()));
}

[[nodiscard]] inline bsoncxx::document::value chat_senior_sort() {
    return make_document(kvp(chat_detail::k(cmf::kConversation), 1),
                         kvp(chat_detail::k(cmf::kRole), 1),
                         kvp(chat_detail::k(cmf::kJoinedSeq), 1));
}

[[nodiscard]] inline bsoncxx::document::value chat_list() {
    return make_document(kvp(chat_detail::k(cmf::kUser), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cmf::kArchived), bsoncxx::types::b_bool{false}));
}

[[nodiscard]] inline bsoncxx::document::value chat_list_cursor() {
    return make_document(
        kvp(chat_detail::k(cmf::kUser), detail::placeholder_uuid()),
        kvp(chat_detail::k(cmf::kArchived), bsoncxx::types::b_bool{false}),
        kvp("$or", bsoncxx::builder::basic::make_array(
                       make_document(kvp(chat_detail::k(cmf::kActivity),
                                         make_document(kvp("$lt", detail::placeholder_time())))),
                       make_document(kvp(chat_detail::k(cmf::kActivity), detail::placeholder_time()),
                                     kvp(chat_detail::k(cmf::kId),
                                         make_document(kvp("$lt", detail::placeholder_uuid())))))));
}

[[nodiscard]] inline bsoncxx::document::value chat_list_sort() {
    return make_document(kvp(chat_detail::k(cmf::kUser), 1),
                         kvp(chat_detail::k(cmf::kArchived), 1),
                         kvp(chat_detail::k(cmf::kActivity), -1), kvp(chat_detail::k(cmf::kId), -1));
}

// A device change's walk over one person's conversations (§7.4).
[[nodiscard]] inline bsoncxx::document::value chat_memberships_of() {
    return make_document(
        kvp(chat_detail::k(cmf::kUser), detail::placeholder_uuid()),
        kvp(chat_detail::k(cmf::kConversation), make_document(kvp("$gt", detail::placeholder_uuid()))),
        kvp(chat_detail::k(cmf::kLeftSeq), chat_detail::not_present()));
}

[[nodiscard]] inline bsoncxx::document::value chat_memberships_of_sort() {
    return make_document(kvp(chat_detail::k(cmf::kUser), 1),
                         kvp(chat_detail::k(cmf::kConversation), 1));
}

[[nodiscard]] inline bsoncxx::document::value chat_pinned() {
    return make_document(kvp(chat_detail::k(cmf::kUser), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cmf::kPinned), bsoncxx::types::b_bool{true}));
}

[[nodiscard]] inline bsoncxx::document::value chat_read_by() {
    return make_document(
        kvp(chat_detail::k(cmf::kConversation), detail::placeholder_uuid()),
        kvp(chat_detail::k(cmf::kRead), make_document(kvp("$gte", bsoncxx::types::b_int64{1}))),
        kvp(chat_detail::k(cmf::kReadPrivate), bsoncxx::types::b_bool{false}),
        kvp(chat_detail::k(cmf::kLeftSeq), chat_detail::not_present()));
}

[[nodiscard]] inline bsoncxx::document::value chat_delivered_to() {
    return make_document(
        kvp(chat_detail::k(cmf::kConversation), detail::placeholder_uuid()),
        kvp(chat_detail::k(cmf::kDelivered), make_document(kvp("$gte", bsoncxx::types::b_int64{1}))),
        kvp(chat_detail::k(cmf::kLeftSeq), chat_detail::not_present()));
}

[[nodiscard]] inline bsoncxx::document::value chat_message_by_seq() {
    bsoncxx::builder::basic::document filter;
    filter.append(kvp(chat_detail::k(cgf::kConversation), detail::placeholder_uuid()));
    filter.append(kvp(chat_detail::k(cgf::kSeq), bsoncxx::types::b_int64{1}));
    filter.append(bsoncxx::builder::concatenate(chat_detail::live_branches().view()));
    return filter.extract();
}

[[nodiscard]] inline bsoncxx::document::value chat_message_by_cid() {
    return make_document(kvp(chat_detail::k(cgf::kConversation), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cgf::kSender), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cgf::kClientId), detail::placeholder_digest_16()));
}

[[nodiscard]] inline bsoncxx::document::value chat_history() {
    bsoncxx::builder::basic::document filter;
    filter.append(kvp(chat_detail::k(cgf::kConversation), detail::placeholder_uuid()));
    filter.append(kvp(chat_detail::k(cgf::kSeq),
                      make_document(kvp("$gte", bsoncxx::types::b_int64{0}),
                                    kvp("$lt", bsoncxx::types::b_int64{100}))));
    filter.append(bsoncxx::builder::concatenate(chat_detail::live_branches().view()));
    return filter.extract();
}

// The mutation catch-up: changed messages past a cursor, inside the reader's
// range, in the order they changed (§4.5).
[[nodiscard]] inline bsoncxx::document::value chat_changed() {
    bsoncxx::builder::basic::document filter;
    filter.append(kvp(chat_detail::k(cgf::kConversation), detail::placeholder_uuid()));
    filter.append(kvp(chat_detail::k(cgf::kMutation),
                      make_document(kvp("$exists", bsoncxx::types::b_bool{true}),
                                    kvp("$gt", bsoncxx::types::b_int64{0}),
                                    kvp("$lte", bsoncxx::types::b_int64{100}))));
    filter.append(kvp(chat_detail::k(cgf::kSeq),
                      make_document(kvp("$gte", bsoncxx::types::b_int64{0}),
                                    kvp("$lt", bsoncxx::types::b_int64{100}))));
    filter.append(bsoncxx::builder::concatenate(chat_detail::live_branches().view()));
    return filter.extract();
}

[[nodiscard]] inline bsoncxx::document::value chat_changed_sort() {
    return make_document(kvp(chat_detail::k(cgf::kConversation), 1),
                         kvp(chat_detail::k(cgf::kMutation), 1));
}

// The nudge's read: the newest messages below the head, with no lower bound.
[[nodiscard]] inline bsoncxx::document::value chat_nudge_recent() {
    bsoncxx::builder::basic::document filter;
    filter.append(kvp(chat_detail::k(cgf::kConversation), detail::placeholder_uuid()));
    filter.append(kvp(chat_detail::k(cgf::kSeq),
                      make_document(kvp("$lt", bsoncxx::types::b_int64{100}))));
    filter.append(bsoncxx::builder::concatenate(chat_detail::live_branches().view()));
    return filter.extract();
}

[[nodiscard]] inline bsoncxx::document::value chat_history_sort() {
    return make_document(kvp(chat_detail::k(cgf::kConversation), 1),
                         kvp(chat_detail::k(cgf::kSeq), -1));
}

[[nodiscard]] inline bsoncxx::document::value chat_after_sort() {
    return make_document(kvp(chat_detail::k(cgf::kConversation), 1),
                         kvp(chat_detail::k(cgf::kSeq), 1));
}

[[nodiscard]] inline bsoncxx::document::value chat_expired() {
    return make_document(kvp(chat_detail::k(cgf::kExpiresAt),
                             make_document(kvp("$exists", bsoncxx::types::b_bool{true}),
                                           kvp("$lte", detail::placeholder_time()))));
}

[[nodiscard]] inline bsoncxx::document::value chat_expired_sort() {
    return make_document(kvp(chat_detail::k(cgf::kExpiresAt), 1),
                         kvp(chat_detail::k(cgf::kId), 1));
}

[[nodiscard]] inline bsoncxx::document::value chat_reactions_for_page() {
    return make_document(kvp(chat_detail::k(crf::kConversation), detail::placeholder_uuid()),
                         kvp(chat_detail::k(crf::kSeq),
                             make_document(kvp("$in", bsoncxx::builder::basic::make_array(
                                                          bsoncxx::types::b_int64{1})))));
}

[[nodiscard]] inline bsoncxx::document::value chat_reaction() {
    return make_document(kvp(chat_detail::k(crf::kConversation), detail::placeholder_uuid()),
                         kvp(chat_detail::k(crf::kSeq), bsoncxx::types::b_int64{1}),
                         kvp(chat_detail::k(crf::kUser), detail::placeholder_uuid()));
}

[[nodiscard]] inline bsoncxx::document::value chat_block_one() {
    return make_document(kvp(chat_detail::k(cbf::kBlocker), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cbf::kBlocked), detail::placeholder_uuid()));
}

[[nodiscard]] inline bsoncxx::document::value chat_block_pair() {
    return make_document(kvp(
        "$or", bsoncxx::builder::basic::make_array(
                   make_document(kvp(chat_detail::k(cbf::kBlocker), detail::placeholder_uuid()),
                                 kvp(chat_detail::k(cbf::kBlocked), detail::placeholder_uuid())),
                   make_document(kvp(chat_detail::k(cbf::kBlocker), detail::placeholder_uuid()),
                                 kvp(chat_detail::k(cbf::kBlocked), detail::placeholder_uuid())))));
}

// A report filed again over the same range (§9.2), and the staff listing.
[[nodiscard]] inline bsoncxx::document::value chat_report_range() {
    return make_document(kvp(chat_detail::k(cpr::kConversation), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cpr::kReporter), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cpr::kFrom), bsoncxx::types::b_int64{1}),
                         kvp(chat_detail::k(cpr::kTo), bsoncxx::types::b_int64{2}));
}

[[nodiscard]] inline bsoncxx::document::value chat_reports_page() {
    return make_document(
        kvp(chat_detail::k(cpr::kId), make_document(kvp("$lt", detail::placeholder_uuid()))));
}

[[nodiscard]] inline bsoncxx::document::value chat_reports_page_sort() {
    return make_document(kvp(chat_detail::k(cpr::kId), -1));
}

// --- chat devices (docs/22-chat.md §7.3) -------------------------------------

// Every other device query is by account (_id): the approver read, the session
// lookup, devices_of's $in, the touch and every write.
[[nodiscard]] inline bsoncxx::document::value chat_idle_devices() {
    return make_document(kvp(chat_detail::k(cdf::kLastSeenPath),
                             make_document(kvp("$lt", detail::placeholder_time()))));
}

[[nodiscard]] inline bsoncxx::document::value chat_device_session() {
    return make_document(kvp(chat_detail::k(cdf::kId), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cdf::kSessionPath), detail::placeholder_uuid()));
}

// The device-change sweeper's scan, oldest pending first.
[[nodiscard]] inline bsoncxx::document::value chat_pending_device_changes() {
    return make_document(kvp(chat_detail::k(cdf::kPendingSince),
                             make_document(kvp("$lt", detail::placeholder_time()))));
}

[[nodiscard]] inline bsoncxx::document::value chat_pending_device_changes_sort() {
    return make_document(kvp(chat_detail::k(cdf::kPendingSince), 1));
}

// One device's one-time keys: the claim, the count and the discard.
[[nodiscard]] inline bsoncxx::document::value chat_prekeys_of_device() {
    return make_document(kvp(chat_detail::k(cpf::kDevice), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cpf::kUser), detail::placeholder_uuid()));
}

// A device's queue after its cursor, unexpired, in send order (§7.6).
[[nodiscard]] inline bsoncxx::document::value chat_queue_read() {
    return make_document(
        kvp(chat_detail::k(cqf::kDevice), detail::placeholder_uuid()),
        kvp(chat_detail::k(cqf::kUser), detail::placeholder_uuid()),
        kvp(chat_detail::k(cqf::kId), make_document(kvp("$gt", detail::placeholder_uuid()))),
        kvp(chat_detail::k(cqf::kExpiresAt), make_document(kvp("$gt", detail::placeholder_time()))));
}

[[nodiscard]] inline bsoncxx::document::value chat_queue_read_sort() {
    return make_document(kvp(chat_detail::k(cqf::kDevice), 1), kvp(chat_detail::k(cqf::kId), 1));
}

// The acknowledgement: everything up to an id.
[[nodiscard]] inline bsoncxx::document::value chat_queue_ack() {
    return make_document(
        kvp(chat_detail::k(cqf::kDevice), detail::placeholder_uuid()),
        kvp(chat_detail::k(cqf::kUser), detail::placeholder_uuid()),
        kvp(chat_detail::k(cqf::kId), make_document(kvp("$lte", detail::placeholder_uuid()))));
}

// One message's rows, for its revoke.
[[nodiscard]] inline bsoncxx::document::value chat_queue_message() {
    return make_document(kvp(chat_detail::k(cqf::kConversation), detail::placeholder_uuid()),
                         kvp(chat_detail::k(cqf::kSeq), bsoncxx::types::b_int64{1}));
}

// The relay's replacement of a session's earlier request (§7.3.1).
[[nodiscard]] inline bsoncxx::document::value chat_link_by_session() {
    return make_document(kvp(chat_detail::k(clf::kUser), detail::placeholder_uuid()),
                         kvp(chat_detail::k(clf::kSession), detail::placeholder_uuid()));
}

// --- end chat devices --------------------------------------------------------

inline constexpr std::array<anvil::db::QuerySpec, 80> kQueries{{
    {"login_by_email", "users", &login_by_email, nullptr},
    {"login_by_username", "users", &login_by_username, nullptr},
    {"login_by_phone", "users", &login_by_phone, nullptr},
    {"account_listing", "users", &account_listing, &account_listing_sort},
    {"stale_pending_sweep", "users", &stale_pending_sweep, nullptr},

    {"refresh_current", "user_sessions", &refresh_current, nullptr},
    {"refresh_previous", "user_sessions", &refresh_previous, nullptr},
    {"sessions_for_user", "user_sessions", &sessions_for_user, &sessions_for_user_sort},
    {"sessions_to_revoke", "user_sessions", &sessions_to_revoke, nullptr},

    {"capability_consume", "capability_tokens", &capability_consume, nullptr},
    {"verification_consume", "email_verifications", &verification_consume, nullptr},

    {"media_dedup", "media", &media_dedup, nullptr},
    {"media_listing", "media", &media_listing, &media_listing_sort},
    {"media_edit_lookup", "media", &media_edit_lookup, nullptr},

    {"section_by_key_and_state", "sections", &section_by_key_and_state, nullptr},

    {"entry_page", "entries", &entry_page_filter, &entry_newest_sort},
    {"entry_cursor_page", "entries", &entry_cursor_filter, &entry_newest_sort},
    {"entry_working_list", "entries", &entry_working_filter, &entry_manual_sort},
    {"entry_last_position", "entries", &entry_working_filter, &entry_newest_sort},
    {"entry_flagged", "entries", &entry_flagged_filter, &entry_manual_sort},
    {"entry_by_id", "entries", &entry_by_id, nullptr},
    {"entry_by_slug", "entries", &entry_by_slug, nullptr},
    {"entry_scope", "entries", &entry_scope, nullptr},

    {"form_listing", "form_definitions", &form_listing, &form_listing_sort},
    {"forms_by_creator", "form_definitions", &forms_by_creator, nullptr},

    {"submissions_first_page", "form_submissions", &submissions_first_page,
     &submissions_page_sort},
    {"submissions_cursor_page", "form_submissions", &submissions_cursor_page,
     &submissions_page_sort},
    {"submissions_blind_index", "form_submissions", &submissions_blind_index, nullptr},

    {"broadcast_branch", "notifications", &broadcast_branch, &broadcast_branch_sort},
    {"outbox_sweep", "notifications", &outbox_sweep, nullptr},
    {"notification_dedupe", "notifications", &notification_dedupe, nullptr},

    {"inbox_page", "notification_inbox", &inbox_page, &inbox_page_sort},
    {"inbox_unread_count", "notification_inbox", &inbox_unread_count, nullptr},

    {"subscriber_scan", "notification_clients", &subscriber_scan, &subscriber_scan_sort},
    {"inapp_client_lookup", "notification_clients", &inapp_client_lookup, nullptr},
    {"push_targets", "notification_clients", &push_targets, nullptr},
    {"webhook_listing", "notification_clients", &webhook_listing, &webhook_listing_sort},

    {"audit_by_actor", "audit_log", &audit_by_actor, &audit_by_actor_sort},
    // Deliberately the SAME shape with no sort, so the check proves that the
    // sort clause is what it claims to be: a query that passes unsorted and
    // fails sorted has an index that covers the filter and not the order, which
    // is a different defect with a different fix.
    {"audit_by_actor_unsorted", "audit_log", &audit_by_actor, nullptr},

    {"analytics_window_page", "analytics_events", &analytics_window_page,
     &analytics_window_sort},
    {"analytics_erase_subject", "analytics_events", &analytics_erase_subject, nullptr},
    {"analytics_sessions_by_day", "analytics_sessions", &analytics_sessions_by_day, nullptr},
    {"analytics_rollup_series", "analytics_rollups", &analytics_rollup_series,
     &analytics_rollup_series_sort},
    {"analytics_rollup_series_for_dims", "analytics_rollups",
     &analytics_rollup_series_for_dims, &analytics_rollup_series_sort},
    {"analytics_rollup_upsert", "analytics_rollups", &analytics_rollup_upsert, nullptr},

    {"chat_direct_pair", "chat_conversations", &chat_direct_pair, nullptr},
    {"chat_created", "chat_conversations", &chat_created, nullptr},
    {"chat_conversations_in", "chat_conversations", &chat_conversations_in, nullptr},
    {"chat_membership", "chat_members", &chat_membership, nullptr},
    {"chat_current_members", "chat_members", &chat_current_members, nullptr},
    {"chat_members_in", "chat_members", &chat_members_in, nullptr},
    {"chat_member_page", "chat_members", &chat_member_page, &chat_member_page_sort},
    {"chat_senior", "chat_members", &chat_senior, &chat_senior_sort},
    {"chat_list", "chat_members", &chat_list, &chat_list_sort},
    {"chat_list_cursor", "chat_members", &chat_list_cursor, &chat_list_sort},
    {"chat_pinned", "chat_members", &chat_pinned, nullptr},
    {"chat_memberships_of", "chat_members", &chat_memberships_of, &chat_memberships_of_sort},
    {"chat_read_by", "chat_members", &chat_read_by, nullptr},
    {"chat_delivered_to", "chat_members", &chat_delivered_to, nullptr},
    {"chat_message_by_seq", "chat_messages", &chat_message_by_seq, nullptr},
    {"chat_message_by_cid", "chat_messages", &chat_message_by_cid, nullptr},
    {"chat_history", "chat_messages", &chat_history, &chat_history_sort},
    {"chat_after", "chat_messages", &chat_history, &chat_after_sort},
    {"chat_changed", "chat_messages", &chat_changed, &chat_changed_sort},
    {"chat_nudge_recent", "chat_messages", &chat_nudge_recent, &chat_history_sort},
    {"chat_expired", "chat_messages", &chat_expired, &chat_expired_sort},
    {"chat_reactions_for_page", "chat_reactions", &chat_reactions_for_page, nullptr},
    {"chat_reaction", "chat_reactions", &chat_reaction, nullptr},
    {"chat_block_pair", "chat_blocks", &chat_block_pair, nullptr},
    {"chat_block_one", "chat_blocks", &chat_block_one, nullptr},
    {"chat_report_range", "chat_reports", &chat_report_range, nullptr},
    {"chat_reports_page", "chat_reports", &chat_reports_page, &chat_reports_page_sort},
    // --- chat devices -------------------------------------------------------
    {"chat_idle_devices", "chat_identities", &chat_idle_devices, nullptr},
    {"chat_pending_device_changes", "chat_identities", &chat_pending_device_changes,
     &chat_pending_device_changes_sort},
    {"chat_device_session", "chat_identities", &chat_device_session, nullptr},
    {"chat_prekeys_of_device", "chat_prekeys", &chat_prekeys_of_device, nullptr},
    {"chat_queue_read", "chat_device_queue", &chat_queue_read, &chat_queue_read_sort},
    {"chat_queue_ack", "chat_device_queue", &chat_queue_ack, nullptr},
    {"chat_queue_message", "chat_device_queue", &chat_queue_message, nullptr},
    {"chat_link_by_session", "chat_link_requests", &chat_link_by_session, nullptr},
    // --- end chat devices ---------------------------------------------------
}};

static_assert(anvil::db::query_catalogue_is_well_formed(kQueries),
              "empty name, a null filter, a duplicate name, or a collection that is not in "
              "the application's table");

}  // namespace testapp::queries
