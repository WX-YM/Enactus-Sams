// versioned-write-exempt: a session carries no `v` field and is never
// read-modify-written. The one write that could race — refresh rotation — is a
// compare-and-swap on the stored hash inside its own filter, which is the same
// guarantee update_versioned provides and is what makes the two-tab race safe.
// Every other write is a targeted $set of a single field.

#include "anvil/identity/sessions.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/update.hpp>
#include <mongocxx/pipeline.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/core/uuid.h"

namespace anvil::identity {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace f = session_fields;

[[nodiscard]] Result<SessionRecord> decode(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<Uuid> user_id = codec::read_uuid(doc, f::kUserId);
    if (!user_id) { return user_id.error(); }
    const Result<db::TimeMs> last_seen = codec::read_time(doc, f::kLastSeen);
    if (!last_seen) { return last_seen.error(); }
    const Result<db::TimeMs> expires_at = codec::read_time(doc, f::kExpiresAt);
    if (!expires_at) { return expires_at.error(); }
    const Result<db::TimeMs> abs_expiry = codec::read_time(doc, f::kAbsoluteExpiry);
    if (!abs_expiry) { return abs_expiry.error(); }
    const Result<std::optional<db::TimeMs>> prev_until =
        codec::read_optional_time(doc, f::kPreviousUntil);
    if (!prev_until) { return prev_until.error(); }
    const Result<UserType> user_type = codec::read_enum(doc, f::kUserType, kMaxUserType);
    if (!user_type) { return user_type.error(); }
    const Result<bool> revoked = codec::read_bool(doc, f::kRevoked);
    if (!revoked) { return revoked.error(); }

    SessionRecord record{.last_seen = last_seen.value(),
                         .expires_at = expires_at.value(),
                         .abs_expiry = abs_expiry.value(),
                         .prev_until = prev_until.value(),
                         .id = id.value(),
                         .user_id = user_id.value(),
                         .ip = {},
                         .user_agent_hash = {},
                         .user_type = user_type.value(),
                         .revoked = revoked.value()};
    const Status ip = codec::read_bytes(doc, f::kIp, record.ip);
    if (!ip) { return ip.error(); }
    const Status agent = codec::read_bytes(doc, f::kUserAgentHash, record.user_agent_hash);
    if (!agent) { return agent.error(); }
    return record;
}

// The projection every read shares. The two stored token hashes are
// deliberately absent from it: nothing above this layer has a use for one, and
// a value that never leaves the database cannot leak from a log line.
[[nodiscard]] mongocxx::options::find session_projection() {
    mongocxx::options::find options{};
    options.projection(make_document(kvp(codec::key_of(f::kUserId), 1),
                                     kvp(codec::key_of(f::kLastSeen), 1),
                                     kvp(codec::key_of(f::kExpiresAt), 1),
                                     kvp(codec::key_of(f::kAbsoluteExpiry), 1),
                                     kvp(codec::key_of(f::kPreviousUntil), 1),
                                     kvp(codec::key_of(f::kIp), 1),
                                     kvp(codec::key_of(f::kUserAgentHash), 1),
                                     kvp(codec::key_of(f::kUserType), 1),
                                     kvp(codec::key_of(f::kRevoked), 1)));
    return options;
}

// A bulk revocation reads and revokes this many ids at a time, for at most this
// many pages. The cap on concurrent sessions is ten by default, so one page is
// the ordinary case and the bound exists for the unordinary one.
constexpr std::int64_t kRevokePage = 64;
constexpr std::int32_t kRevokePages = 16;

[[nodiscard]] bsoncxx::document::value revoke_update() {
    return make_document(kvp("$set", [](sub_document sub) {
        sub.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{true}));
    }));
}

}  // namespace

void report_sessions_revoked(const SessionsRevoked& hook, mongocxx::client& client,
                             const Uuid& user_id, std::span<const Uuid> session_ids) {
    if (!hook || session_ids.empty()) { return; }
    // The revocation has committed and its caller is owed its answer: what the
    // hook ends is best effort, and a throw out of it must neither fail the
    // sign-out nor escape a db_pool task.
    try {
        hook(client, user_id, session_ids);
    } catch (...) {
        LOG_ERROR << "session revocation hook threw for " << uuid::to_string(user_id);
    }
}

Status SessionRepository::insert(mongocxx::client& client, const NewSession& session) const {
    return repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document doc;
        codec::append_uuid(doc, f::kId, session.id);
        codec::append_uuid(doc, f::kUserId, session.user_id);
        codec::append_digest(doc, f::kRefreshHash, session.refresh_hash);
        // Written as null rather than omitted. The previous-hash lookup rides a
        // sparse index, and a document that sometimes carries the field and
        // sometimes does not makes the index's contents depend on write order —
        // which is a lookup that works until the day it does not.
        doc.append(kvp(codec::key_of(f::kPreviousHash), bsoncxx::types::b_null{}));
        codec::append_optional_time(doc, f::kPreviousUntil, std::nullopt);
        codec::append_time(doc, f::kLastSeen, session.now);
        codec::append_time(doc, f::kExpiresAt, session.expires_at);
        codec::append_time(doc, f::kAbsoluteExpiry, session.abs_expiry);
        doc.append(kvp(codec::key_of(f::kIp), codec::bytes_bin(session.ip)));
        doc.append(
            kvp(codec::key_of(f::kUserAgentHash), codec::bytes_bin(session.user_agent_hash)));
        codec::append_enum(doc, f::kUserType, session.user_type);
        doc.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));

        mongocxx::collection sessions = bind(client);
        sessions.insert_one(doc.view());
        return ok();
    });
}

Result<std::optional<RefreshLookup>> SessionRepository::find_by_refresh_hash(
    mongocxx::client& client, const crypto::Digest256& refresh_hash, db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::optional<RefreshLookup>> {
        mongocxx::collection sessions = bind(client);

        bsoncxx::builder::basic::document current;
        codec::append_digest(current, f::kRefreshHash, refresh_hash);
        current.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));
        repo::append_not_expired(current, f::kExpiresAt, now);

        if (const auto document = sessions.find_one(current.view(), session_projection())) {
            const Result<SessionRecord> record = decode(document->view());
            if (!record) { return record.error(); }
            return std::optional<RefreshLookup>{
                RefreshLookup{record.value(), RefreshMatch::Current}};
        }

        // The grace lookup deliberately does NOT filter on the window's end. A
        // token found inside the window is a concurrent tab; the SAME token
        // found after it closed is a replay of a rotated credential, and that
        // has to be detected and answered by revoking the session — not by
        // matching nothing and reporting an ordinary expired login.
        bsoncxx::builder::basic::document previous;
        codec::append_digest(previous, f::kPreviousHash, refresh_hash);
        previous.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));
        repo::append_not_expired(previous, f::kExpiresAt, now);

        const auto document = sessions.find_one(previous.view(), session_projection());
        if (!document) { return std::optional<RefreshLookup>{}; }

        const Result<SessionRecord> record = decode(document->view());
        if (!record) { return record.error(); }

        const bool within_grace =
            record.value().prev_until.has_value() && *record.value().prev_until > now;
        return std::optional<RefreshLookup>{
            RefreshLookup{record.value(), within_grace ? RefreshMatch::PreviousGrace
                                                       : RefreshMatch::PreviousStale}};
    });
}

Result<std::optional<SessionRecord>> SessionRepository::find_by_id(mongocxx::client& client,
                                                                   const Uuid& session_id,
                                                                   db::TimeMs now) const {
    return repo::guarded([&]() -> Result<std::optional<SessionRecord>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, session_id);
        filter.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        mongocxx::collection sessions = bind(client);
        const auto document = sessions.find_one(filter.view(), session_projection());
        if (!document) { return std::optional<SessionRecord>{}; }

        const Result<SessionRecord> record = decode(document->view());
        if (!record) { return record.error(); }
        return std::optional<SessionRecord>{record.value()};
    });
}

Result<bool> SessionRepository::rotate_refresh_hash(mongocxx::client& client,
                                                    const Uuid& session_id,
                                                    const crypto::Digest256& expected_current_hash,
                                                    const crypto::Digest256& new_hash,
                                                    db::TimeMs prev_until,
                                                    db::TimeMs new_expires_at,
                                                    db::TimeMs now) const {
    return repo::guarded([&]() -> Result<bool> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, session_id);
        // Compare-and-swap: the hash being replaced must still be the stored
        // one. A second tab that already rotated leaves this matching nothing,
        // and it reads the fresh grace window instead of being signed out.
        codec::append_digest(filter, f::kRefreshHash, expected_current_hash);
        filter.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        bsoncxx::builder::basic::document set;
        codec::append_digest(set, f::kRefreshHash, new_hash);
        codec::append_digest(set, f::kPreviousHash, expected_current_hash);
        codec::append_time(set, f::kPreviousUntil, prev_until);
        codec::append_time(set, f::kExpiresAt, new_expires_at);
        codec::append_time(set, f::kLastSeen, now);

        mongocxx::collection sessions = bind(client);
        const auto result =
            sessions.update_one(filter.view(), make_document(kvp("$set", [&set](sub_document sub) {
                                    sub.append(bsoncxx::builder::concatenate(set.view()));
                                })));
        return result.has_value() && result->modified_count() == 1;
    });
}

Status SessionRepository::revoke(mongocxx::client& client, const Uuid& session_id,
                                 const Uuid& user_id) const {
    return repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, session_id);
        // Scoped to the owner. A session id is not a secret — it travels in
        // every access token — and without this a caller could revoke somebody
        // else's session by presenting an id it had seen.
        codec::append_uuid(filter, f::kUserId, user_id);

        mongocxx::collection sessions = bind(client);
        // ttl-filter-exempt: revoking a session whose expiry has already passed
        // is harmless and idempotent. Filtering on expiry HERE would leave a
        // still-readable expired row un-revoked in the window before the TTL
        // monitor reaches it, which is the opposite of what the expiry filter
        // exists to achieve everywhere else in this file.
        sessions.update_one(filter.view(), revoke_update().view());
        return ok();
    });
}

Result<std::vector<Uuid>> SessionRepository::revoke_all(mongocxx::client& client,
                                                        const Uuid& user_id) const {
    return revoke_live(client, user_id, std::nullopt);
}

Result<std::vector<Uuid>> SessionRepository::revoke_all_except(
    mongocxx::client& client, const Uuid& user_id, const Uuid& keep_session_id) const {
    return revoke_live(client, user_id, keep_session_id);
}

Result<std::vector<Uuid>> SessionRepository::revoke_live(mongocxx::client& client,
                                                         const Uuid& user_id,
                                                         std::optional<Uuid> keep) const {
    return repo::guarded([&]() -> Result<std::vector<Uuid>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kUserId, user_id);
        filter.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));
        if (keep.has_value()) {
            filter.append(kvp(codec::key_of(f::kId), [&keep](sub_document sub) {
                sub.append(kvp("$ne", codec::uuid_bin(*keep)));
            }));
        }

        mongocxx::options::find options{};
        options.projection(make_document(kvp(codec::key_of(f::kId), 1)));
        options.limit(kRevokePage);

        std::vector<Uuid> ended;
        mongocxx::collection sessions = bind(client);
        for (std::int32_t page = 0; page < kRevokePages; ++page) {
            std::vector<Uuid> ids;
            ids.reserve(static_cast<std::size_t>(kRevokePage));
            // ttl-filter-exempt: same reason as revoke() — an expired row is
            // still readable for up to a minute, and revoking it is exactly what
            // a "sign out everywhere" must do.
            for (const bsoncxx::document::view doc : sessions.find(filter.view(), options)) {
                const Result<Uuid> id = codec::read_uuid(doc, f::kId);
                if (!id) { return id.error(); }
                ids.push_back(id.value());
            }
            if (ids.empty()) { return ended; }

            // The user stays in the filter so a page can only ever revoke the
            // rows that were read for this user, and `rev: false` so a row a
            // concurrent revocation took is not written twice. Such a row is
            // still reported: both revocations then name it, and ending what a
            // session made is idempotent where it is ended.
            bsoncxx::builder::basic::document by_id;
            codec::append_uuid(by_id, f::kUserId, user_id);
            by_id.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));
            by_id.append(kvp(codec::key_of(f::kId), [&ids](sub_document sub) {
                sub.append(kvp("$in", [&ids](sub_array values) {
                    for (const Uuid& id : ids) { values.append(codec::uuid_bin(id)); }
                }));
            }));
            sessions.update_many(by_id.view(), revoke_update().view());
            ended.insert(ended.end(), ids.begin(), ids.end());
        }

        // Past kRevokePages pages somebody is signing in faster than this can
        // name sessions, which the concurrent-session cap makes impossible for
        // one account in ordinary use. The rest are revoked unnamed rather than
        // left live: a sign-out that stops early is the worse failure, and what
        // a session made is ended durably elsewhere (SessionsRevoked).
        sessions.update_many(filter.view(), revoke_update().view());
        return ended;
    });
}

Result<std::vector<Uuid>> SessionRepository::overflow_sessions(mongocxx::client& client,
                                                               const Uuid& user_id,
                                                               db::TimeMs now,
                                                               std::int32_t keep) const {
    // Nothing to evict when the caller keeps everything. Guarded here rather
    // than at the call site because a negative or zero `keep` would otherwise
    // become a skip() the server has to walk.
    if (keep < 0) { return std::vector<Uuid>{}; }

    return repo::guarded([&]() -> Result<std::vector<Uuid>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kUserId, user_id);
        filter.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        mongocxx::options::find options{};
        options.projection(make_document(kvp(codec::key_of(f::kId), 1)));
        // Oldest FIRST, and the whole page is read. The alternative — sort
        // newest-first and skip(keep) — is the one shape docs/09 §5 rules out,
        // and it would be a skip over a list whose length is the cap anyway.
        // The cap is a handful of rows, so reading them all and dropping the
        // newest `keep` in memory costs less than the server walking a skip.
        options.sort(make_document(kvp(codec::key_of(f::kLastSeen), 1)));
        // One more than the cap is enough to know the cap was exceeded, but the
        // caller may be evicting several at once after a burst of sign-ins, so
        // the bound is generous and still a bound.
        options.limit(static_cast<std::int64_t>(keep) + 64);

        std::vector<Uuid> ordered;
        mongocxx::collection collection = bind(client);
        for (const bsoncxx::document::view doc : collection.find(filter.view(), options)) {
            const Result<Uuid> id = codec::read_uuid(doc, f::kId);
            if (!id) { return id.error(); }
            ordered.push_back(id.value());
        }

        const std::size_t cap = static_cast<std::size_t>(keep);
        if (ordered.size() <= cap) { return std::vector<Uuid>{}; }
        ordered.resize(ordered.size() - cap);
        return ordered;
    });
}

Result<std::vector<SessionRecord>> SessionRepository::list_for_user(mongocxx::client& client,
                                                                    const Uuid& user_id,
                                                                    db::TimeMs now,
                                                                    std::int32_t limit) const {
    return repo::guarded([&]() -> Result<std::vector<SessionRecord>> {
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kUserId, user_id);
        filter.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));
        repo::append_not_expired(filter, f::kExpiresAt, now);

        const std::int32_t bounded = limit > 0 ? limit : 1;
        mongocxx::options::find options = session_projection();
        options.sort(make_document(kvp(codec::key_of(f::kLastSeen), -1)));
        options.limit(bounded);

        std::vector<SessionRecord> sessions;
        sessions.reserve(static_cast<std::size_t>(bounded));

        mongocxx::collection collection = bind(client);
        for (const bsoncxx::document::view doc : collection.find(filter.view(), options)) {
            const Result<SessionRecord> record = decode(doc);
            if (!record) { return record.error(); }
            sessions.push_back(record.value());
        }
        return sessions;
    });
}

Result<std::vector<SessionCount>> SessionRepository::count_live_for_users(
    mongocxx::client& client, std::span<const Uuid> user_ids, db::TimeMs now) const {
    // An empty `$in` matches nothing, which is the right answer, but issuing it
    // still costs a round trip to learn it.
    if (user_ids.empty()) { return std::vector<SessionCount>{}; }

    return repo::guarded([&]() -> Result<std::vector<SessionCount>> {
        bsoncxx::builder::basic::document match;
        match.append(kvp(codec::key_of(f::kUserId), [user_ids](sub_document sub) {
            sub.append(kvp("$in", [user_ids](sub_array values) {
                for (const Uuid& id : user_ids) { values.append(codec::uuid_bin(id)); }
            }));
        }));
        match.append(kvp(codec::key_of(f::kRevoked), bsoncxx::types::b_bool{false}));
        // The TTL monitor lags by up to a minute, so an expired session is still
        // a document. Counting it would report a device that cannot authenticate
        // as signed in.
        repo::append_not_expired(match, f::kExpiresAt, now);

        mongocxx::pipeline stages;
        stages.match(match.view());
        stages.group(make_document(
            kvp("_id", "$" + std::string{f::kUserId}),
            kvp("n", make_document(kvp("$sum", bsoncxx::types::b_int32{1})))));

        std::vector<SessionCount> counts;
        counts.reserve(user_ids.size());
        mongocxx::collection collection = bind(client);
        for (const bsoncxx::document::view doc : collection.aggregate(stages)) {
            const Result<Uuid> id = codec::read_uuid(doc, "_id");
            if (!id) { return id.error(); }
            const Result<std::int32_t> live = codec::read_int32(doc, "n");
            if (!live) { return live.error(); }
            counts.push_back(SessionCount{.user_id = id.value(), .live = live.value()});
        }
        return counts;
    });
}

}  // namespace anvil::identity
