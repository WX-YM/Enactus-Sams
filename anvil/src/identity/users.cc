// versioned-write-exempt: the unversioned writes here are targeted $set or $inc
// of a single field that no reader round-trips — the failure counters, the
// epoch, the status, and a password hash guarded by a compare-and-swap on the
// value it replaces. There is no read-modify-write to lose, so a version filter
// would add a mandatory re-read to the login path and protect nothing
// (ENGINEERING_RULES.md §6). Every write that IS a read-modify-write — permissions, user
// type, the password change — goes through db/versioned.h below.

#include "anvil/identity/users.h"

#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_delete.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/update.hpp>

#include "anvil/db/versioned.h"
#include "anvil/identity/user_fields.h"

namespace anvil::identity {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

namespace codec = db::codec;
namespace f = fields;

[[nodiscard]] bsoncxx::types::b_string text_of(std::string_view value) noexcept {
    return bsoncxx::types::b_string{codec::key_of(value)};
}

// Which field a login identity resolves against. A function rather than a
// branch at each call site: the three fields each have their own unique index,
// and the whole reason the caller passes a `kind` is that anvil must issue ONE
// equality rather than an `$or` the planner cannot serve from a single index.
[[nodiscard]] std::string_view field_of(LoginIdentity kind) noexcept {
    switch (kind) {
        case LoginIdentity::Email:    return f::kEmailNormalised;
        case LoginIdentity::Username: return f::kUsernameNormalised;
        case LoginIdentity::Phone:    return f::kPhone;
    }
    return f::kEmailNormalised;
}

// Projections. Each names its fields explicitly, and which one a method uses is
// a security decision rather than an optimisation — see the header.
[[nodiscard]] mongocxx::options::find login_projection() {
    mongocxx::options::find options{};
    options.projection(make_document(kvp(codec::key_of(f::kPasswordHash), 1),
                                     kvp(codec::key_of(f::kUserType), 1),
                                     kvp(codec::key_of(f::kEffectivePerms), 1),
                                     kvp(codec::key_of(f::kPermEpoch), 1),
                                     kvp(codec::key_of(f::kStatus), 1),
                                     kvp(codec::key_of(f::kFailureCount), 1),
                                     kvp(codec::key_of(f::kLockUntil), 1),
                                     kvp(codec::key_of(f::kLocale), 1)));
    return options;
}

[[nodiscard]] mongocxx::options::find perm_projection() {
    mongocxx::options::find options{};
    // No `_id`: the caller already holds it — it is what this was looked up by —
    // and sixteen bytes per refresh is sixteen bytes of nothing.
    options.projection(make_document(kvp(codec::key_of(f::kId), 0),
                                     kvp(codec::key_of(f::kUserType), 1),
                                     kvp(codec::key_of(f::kEffectivePerms), 1),
                                     kvp(codec::key_of(f::kPermEpoch), 1),
                                     kvp(codec::key_of(f::kStatus), 1),
                                     kvp(codec::key_of(f::kLocale), 1)));
    return options;
}

// Returned BY VALUE and handed to options.projection() by value, never as a
// .view() into the temporary.
//
// view_or_value takes ownership of a value and stores a bare pointer for a view,
// so `options.projection(build().view())` leaves the options holding a view into
// a document that died at the end of that statement — a heap-use-after-free the
// driver hits on the next call, which is a long way from where the mistake is.
// Exactly the hazard anvil/db/migrations.h's index_options documents for
// partial_filter_expression.
[[nodiscard]] bsoncxx::document::value account_projection_document() {
    return make_document(kvp(codec::key_of(f::kUsernameDisplay), 1),
                         kvp(codec::key_of(f::kEmailDisplay), 1),
                         kvp(codec::key_of(f::kDirectPerms), 1),
                         kvp(codec::key_of(f::kEffectivePerms), 1),
                         kvp(codec::key_of(f::kUserType), 1),
                         kvp(codec::key_of(f::kStatus), 1),
                         kvp(codec::key_of(f::kLocale), 1),
                         kvp(codec::key_of(f::kCreatedAt), 1),
                         kvp(codec::key_of(repo::kVersionField), 1));
}

[[nodiscard]] Result<UserAuthRecord> decode_auth(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<std::string_view> hash = codec::read_text(doc, f::kPasswordHash);
    if (!hash) { return hash.error(); }
    const Result<PermSet> effective = codec::read_perm_set(doc, f::kEffectivePerms);
    if (!effective) { return effective.error(); }
    const Result<std::int64_t> epoch = codec::read_int64(doc, f::kPermEpoch);
    if (!epoch) { return epoch.error(); }
    const Result<std::int32_t> failures = codec::read_int32(doc, f::kFailureCount);
    if (!failures) { return failures.error(); }
    const Result<UserType> type = codec::read_enum(doc, f::kUserType, kMaxUserType);
    if (!type) { return type.error(); }
    const Result<UserStatus> status = codec::read_enum(doc, f::kStatus, kMaxUserStatus);
    if (!status) { return status.error(); }
    const Result<std::optional<db::TimeMs>> lock = codec::read_optional_time(doc, f::kLockUntil);
    if (!lock) { return lock.error(); }
    const Result<std::int32_t> locale_index = codec::read_int32(doc, f::kLocale);
    if (!locale_index) { return locale_index.error(); }
    // Range-checked rather than cast. A stored index outside the application's
    // table is corruption or a row written by a build with more locales, and a
    // Locale synthesised from it would index off the end of every localised
    // array in the process.
    const std::optional<Locale> locale =
        locale_index.value() < 0 ? std::nullopt
                                 : Locale::from_index(static_cast<std::size_t>(
                                       locale_index.value()));
    if (!locale.has_value()) { return fail(ErrorCode::Internal, f::kLocale); }

    // The hash is COPIED out of the document view here. `read_text` borrows into
    // the driver's buffer, which dies with the find_one result, and this record
    // outlives it by crossing back to the caller (ENGINEERING_RULES.md §2.2).
    return UserAuthRecord{.password_hash = std::string{hash.value()},
                          .lock_until = lock.value(),
                          .id = id.value(),
                          .effective_permissions = effective.value(),
                          .perm_epoch = epoch.value(),
                          .failure_count = failures.value(),
                          .user_type = type.value(),
                          .status = status.value(),
                          .locale = *locale};
}

[[nodiscard]] Result<Locale> decode_locale(const bsoncxx::document::view& doc) {
    const Result<std::int32_t> index = codec::read_int32(doc, f::kLocale);
    if (!index) { return index.error(); }
    const std::optional<Locale> locale =
        index.value() < 0 ? std::nullopt
                          : Locale::from_index(static_cast<std::size_t>(index.value()));
    if (!locale.has_value()) { return fail(ErrorCode::Internal, f::kLocale); }
    return *locale;
}

[[nodiscard]] Result<AccountRecord> decode_account(const bsoncxx::document::view& doc) {
    const Result<Uuid> id = codec::read_uuid(doc, f::kId);
    if (!id) { return id.error(); }
    const Result<std::string_view> username = codec::read_text(doc, f::kUsernameDisplay);
    if (!username) { return username.error(); }
    const Result<std::string_view> email = codec::read_text(doc, f::kEmailDisplay);
    if (!email) { return email.error(); }
    const Result<PermSet> direct = codec::read_perm_set(doc, f::kDirectPerms);
    if (!direct) { return direct.error(); }
    const Result<PermSet> effective = codec::read_perm_set(doc, f::kEffectivePerms);
    if (!effective) { return effective.error(); }
    const Result<std::int64_t> version = repo::document_version(doc);
    if (!version) { return version.error(); }
    const Result<UserType> type = codec::read_enum(doc, f::kUserType, kMaxUserType);
    if (!type) { return type.error(); }
    const Result<UserStatus> status = codec::read_enum(doc, f::kStatus, kMaxUserStatus);
    if (!status) { return status.error(); }
    const Result<db::TimeMs> created = codec::read_time(doc, f::kCreatedAt);
    if (!created) { return created.error(); }
    const Result<Locale> locale = decode_locale(doc);
    if (!locale) { return locale.error(); }

    return AccountRecord{.created_at = created.value(),
                         .username = std::string{username.value()},
                         .email = std::string{email.value()},
                         .id = id.value(),
                         .direct_permissions = direct.value(),
                         .effective_permissions = effective.value(),
                         .version = version.value(),
                         .user_type = type.value(),
                         .status = status.value(),
                         .locale = locale.value()};
}

}  // namespace

Result<std::optional<UserAuthRecord>> UserRepository::find_for_login(
    mongocxx::client& client, std::string_view normalised, LoginIdentity kind) const {
    return repo::guarded([&]() -> Result<std::optional<UserAuthRecord>> {
        mongocxx::collection users = bind(client);
        const auto found = users.find_one(
            make_document(kvp(codec::key_of(field_of(kind)), text_of(normalised))),
            login_projection());
        if (!found) { return std::optional<UserAuthRecord>{}; }
        const Result<UserAuthRecord> record = decode_auth(found->view());
        if (!record) { return record.error(); }
        return std::optional<UserAuthRecord>{record.value()};
    });
}

Result<std::optional<UserPermRecord>> UserRepository::find_permissions(
    mongocxx::client& client, const Uuid& user_id) const {
    return repo::guarded([&]() -> Result<std::optional<UserPermRecord>> {
        mongocxx::collection users = bind(client);
        const auto found =
            users.find_one(make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(user_id))),
                           perm_projection());
        if (!found) { return std::optional<UserPermRecord>{}; }

        const bsoncxx::document::view doc = found->view();
        const Result<PermSet> effective = codec::read_perm_set(doc, f::kEffectivePerms);
        if (!effective) { return effective.error(); }
        const Result<std::int64_t> epoch = codec::read_int64(doc, f::kPermEpoch);
        if (!epoch) { return epoch.error(); }
        const Result<UserType> type = codec::read_enum(doc, f::kUserType, kMaxUserType);
        if (!type) { return type.error(); }
        const Result<UserStatus> status = codec::read_enum(doc, f::kStatus, kMaxUserStatus);
        if (!status) { return status.error(); }
        const Result<Locale> locale = decode_locale(doc);
        if (!locale) { return locale.error(); }

        return std::optional<UserPermRecord>{UserPermRecord{
            .effective_permissions = effective.value(),
            .perm_epoch = epoch.value(),
            .user_type = type.value(),
            .status = status.value(),
            .locale = locale.value()}};
    });
}

Status UserRepository::insert(mongocxx::client& client, const NewUser& user) const {
    return repo::guarded([&]() -> Status {
        const db::TimeMs now = db::now_ms();

        bsoncxx::builder::basic::document doc;
        codec::append_uuid(doc, f::kId, user.id);
        doc.append(kvp(codec::key_of(f::kEmailNormalised), text_of(user.email_normalised)));
        doc.append(kvp(codec::key_of(f::kEmailDisplay), text_of(user.email_display)));
        doc.append(kvp(codec::key_of(f::kUsernameNormalised),
                       text_of(user.username_normalised)));
        doc.append(kvp(codec::key_of(f::kUsernameDisplay), text_of(user.username_display)));
        doc.append(kvp(codec::key_of(f::kPasswordHash), text_of(user.password_hash)));
        // ABSENT, never empty and never null: the unique index over `ph` is
        // partial on `{ph: {$exists: true}}`, and an empty string is a value
        // every phoneless account would share — which is the same lockout the
        // partial filter exists to prevent, wearing a different shape.
        if (!user.phone_e164.empty()) {
            doc.append(kvp(codec::key_of(f::kPhone), text_of(user.phone_e164)));
        }
        codec::append_enum(doc, f::kUserType, UserType::Client);
        codec::append_perm_set(doc, f::kDirectPerms, PermSet{});
        // The field must EXIST even when it is empty: the login projection reads
        // it, and a missing field is a decode error rather than an empty set.
        codec::append_perm_set(doc, f::kEffectivePerms, PermSet{});
        // 1 rather than 0, so "never bumped" and "no epoch recorded" stay
        // distinguishable in a token.
        codec::append_int64(doc, f::kPermEpoch, 1);
        codec::append_enum(doc, f::kStatus, user.status);
        doc.append(kvp(codec::key_of(f::kFailureCount), bsoncxx::types::b_int32{0}));
        codec::append_optional_time(doc, f::kLockUntil, std::nullopt);
        doc.append(kvp(codec::key_of(f::kLocale),
                       bsoncxx::types::b_int32{static_cast<std::int32_t>(user.locale.index())}));
        codec::append_time(doc, f::kCreatedAt, now);
        codec::append_time(doc, f::kUpdatedAt, now);
        repo::append_initial_version(doc);

        mongocxx::collection users = bind(client);
        users.insert_one(doc.view());
        return ok();
    });
}

Result<std::optional<AccountRecord>> UserRepository::find_account(mongocxx::client& client,
                                                                  const Uuid& user_id) const {
    return repo::guarded([&]() -> Result<std::optional<AccountRecord>> {
        mongocxx::options::find options{};
        options.projection(account_projection_document());

        mongocxx::collection users = bind(client);
        const auto found = users.find_one(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(user_id))), options);
        if (!found) { return std::optional<AccountRecord>{}; }
        const Result<AccountRecord> record = decode_account(found->view());
        if (!record) { return record.error(); }
        return std::optional<AccountRecord>{record.value()};
    });
}

Result<std::optional<AccountRecord>> UserRepository::find_by_identity(
    mongocxx::client& client, std::string_view normalised, LoginIdentity kind) const {
    return repo::guarded([&]() -> Result<std::optional<AccountRecord>> {
        mongocxx::options::find options{};
        options.projection(account_projection_document());

        mongocxx::collection users = bind(client);
        const auto found = users.find_one(
            make_document(kvp(codec::key_of(field_of(kind)), text_of(normalised))), options);
        if (!found) { return std::optional<AccountRecord>{}; }
        const Result<AccountRecord> record = decode_account(found->view());
        if (!record) { return record.error(); }
        return std::optional<AccountRecord>{record.value()};
    });
}

Result<std::vector<AccountRecord>> UserRepository::list_accounts(
    mongocxx::client& client, const AccountQuery& query) const {
    return repo::guarded([&]() -> Result<std::vector<AccountRecord>> {
        bsoncxx::builder::basic::document filter;
        if (query.user_type.has_value()) {
            codec::append_enum(filter, f::kUserType, *query.user_type);
        }
        if (query.status.has_value()) {
            codec::append_enum(filter, f::kStatus, *query.status);
        }
        if (!query.email_normalised.empty()) {
            // EQUALITY. A substring search across every address in the system is
            // a harvesting tool with a staff session in front of it, so an
            // address has to be known in full before it matches.
            filter.append(kvp(codec::key_of(f::kEmailNormalised),
                              text_of(query.email_normalised)));
        }
        if (!query.username_prefix.empty()) {
            // An anchored prefix, expressed as a RANGE rather than as a regex.
            // std::regex is banned on a request path and a server-side `$regex`
            // is no better: only an anchored literal can use an index, and
            // stating the range says so rather than hoping the planner notices
            // (ENGINEERING_RULES.md §5, §7).
            //
            // The upper bound appends one byte above the prefix's last: every
            // string starting with the prefix sorts below it, and nothing else
            // does. 0xFF cannot appear in valid UTF-8, so incrementing into it
            // can never collide with a real spelling.
            std::string upper{query.username_prefix};
            upper.push_back(static_cast<char>(0xFF));
            filter.append(kvp(codec::key_of(f::kUsernameNormalised),
                              [&query, &upper](sub_document sub) {
                                  sub.append(kvp("$gte", text_of(query.username_prefix)));
                                  sub.append(kvp("$lt", text_of(upper)));
                              }));
        }
        if (query.after.has_value()) {
            // The compound cursor, in the index's own key order: strictly after
            // the (user_type, _id) pair the last page ended on. Never skip(n),
            // which is O(n) server-side (ENGINEERING_RULES.md §7).
            const AccountCursor& cursor = *query.after;
            filter.append(kvp("$or", [&cursor](sub_array rows) {
                rows.append([&cursor](sub_document row) {
                    row.append(kvp(codec::key_of(f::kUserType), [&cursor](sub_document sub) {
                        sub.append(kvp("$gt", bsoncxx::types::b_int32{
                                                  static_cast<std::int32_t>(cursor.user_type)}));
                    }));
                });
                rows.append([&cursor](sub_document row) {
                    row.append(kvp(codec::key_of(f::kUserType),
                                   bsoncxx::types::b_int32{
                                       static_cast<std::int32_t>(cursor.user_type)}));
                    row.append(kvp(codec::key_of(f::kId), [&cursor](sub_document sub) {
                        sub.append(kvp("$gt", codec::uuid_bin(cursor.id)));
                    }));
                });
            }));
        }

        mongocxx::options::find options{};
        options.projection(account_projection_document());
        options.sort(make_document(kvp(codec::key_of(f::kUserType), 1),
                                   kvp(codec::key_of(f::kId), 1)));
        // Every result set is bounded (ENGINEERING_RULES.md §7). A limit the caller forgot
        // to set is one, not none.
        options.limit(query.limit > 0 ? query.limit : 1);

        std::vector<AccountRecord> rows;
        rows.reserve(static_cast<std::size_t>(query.limit > 0 ? query.limit : 1));
        mongocxx::collection users = bind(client);
        for (const bsoncxx::document::view doc : users.find(filter.view(), options)) {
            const Result<AccountRecord> record = decode_account(doc);
            if (!record) { return record.error(); }
            rows.push_back(record.value());
        }
        return rows;
    });
}

Result<std::vector<AccountName>> UserRepository::names_of(mongocxx::client& client,
                                                          std::span<const Uuid> ids) const {
    // No query at all for an empty input, which is the common case: the rows an
    // audit collection is busiest with are denials naming neither an actor nor a
    // subject, and a `$in` over nothing is a round trip to learn nothing.
    if (ids.empty()) { return std::vector<AccountName>{}; }

    return repo::guarded([&]() -> Result<std::vector<AccountName>> {
        bsoncxx::builder::basic::array wanted;
        for (const Uuid& id : ids) { wanted.append(codec::uuid_bin(id)); }
        const bsoncxx::array::value wanted_ids = wanted.extract();

        mongocxx::options::find options{};
        options.projection(make_document(kvp(codec::key_of(f::kUsernameDisplay), 1)));
        // Bounded by the input, which is itself a page of audit rows. An `$in`
        // is only as bounded as what the caller put in it, and every result set
        // in this system carries a limit (ENGINEERING_RULES.md §7).
        options.limit(static_cast<std::int64_t>(ids.size()));

        const auto filter = make_document(kvp(
            codec::key_of(f::kId),
            make_document(kvp("$in", bsoncxx::types::b_array{wanted_ids.view()}))));

        std::vector<AccountName> names;
        names.reserve(ids.size());
        mongocxx::collection users = bind(client);
        for (const bsoncxx::document::view doc : users.find(filter.view(), options)) {
            const Result<Uuid> id = codec::read_uuid(doc, f::kId);
            if (!id) { return id.error(); }
            const Result<std::string_view> name = codec::read_text(doc, f::kUsernameDisplay);
            if (!name) { return name.error(); }
            names.push_back(AccountName{.name = std::string{name.value()}, .id = id.value()});
        }
        // An id naming no account is ABSENT rather than an error: an audit row
        // is kept far longer than an account is, and a log that failed to render
        // because somebody was deleted is a log nobody can read after an
        // incident.
        return names;
    });
}

Result<std::int64_t> UserRepository::set_permissions(mongocxx::client& client,
                                                     const Uuid& user_id,
                                                     std::int64_t expected_version,
                                                     const PermSet& direct,
                                                     const PermSet& effective) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document set;
        codec::append_perm_set(set, f::kDirectPerms, direct);
        codec::append_perm_set(set, f::kEffectivePerms, effective);

        mongocxx::collection users = bind(client);
        return repo::update_versioned(
            users, make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(user_id))).view(),
            expected_version, set.view());
    });
}

Result<std::int64_t> UserRepository::set_user_type(mongocxx::client& client,
                                                   mongocxx::client_session& session,
                                                   const Uuid& user_id,
                                                   std::int64_t expected_version,
                                                   UserType user_type, const PermSet& direct,
                                                   const PermSet& effective) const {
    return repo::guarded_in_transaction([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document set;
        codec::append_enum(set, f::kUserType, user_type);
        codec::append_perm_set(set, f::kDirectPerms, direct);
        codec::append_perm_set(set, f::kEffectivePerms, effective);

        mongocxx::collection users = bind(client);
        return repo::update_versioned(
            users, session,
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(user_id))).view(),
            expected_version, set.view());
    });
}

Result<std::optional<UserStatus>> UserRepository::set_status(mongocxx::client& client,
                                                             mongocxx::client_session& session,
                                                             const Uuid& user_id,
                                                             UserStatus status,
                                                             db::TimeMs now) const {
    return repo::guarded_in_transaction([&]() -> Result<std::optional<UserStatus>> {
        bsoncxx::builder::basic::document set;
        codec::append_enum(set, f::kStatus, status);
        codec::append_time(set, f::kUpdatedAt, now);

        mongocxx::options::find_one_and_update options{};
        // The PRE-IMAGE, which is the whole reason this is one operation: the
        // audit row records what the account changed FROM, and reading that
        // separately would record a value that was true a moment earlier.
        options.return_document(mongocxx::options::return_document::k_before);
        options.projection(make_document(kvp(codec::key_of(f::kStatus), 1)));

        mongocxx::collection users = bind(client);
        const auto before = users.find_one_and_update(
            session, make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(user_id))).view(),
            make_document(kvp("$set", set.view())).view(), options);
        if (!before) { return std::optional<UserStatus>{}; }

        const Result<UserStatus> previous =
            codec::read_enum(before->view(), f::kStatus, kMaxUserStatus);
        if (!previous) { return previous.error(); }
        return std::optional<UserStatus>{previous.value()};
    });
}

Result<std::int64_t> UserRepository::count_active_of_type(mongocxx::client& client,
                                                          mongocxx::client_session& session,
                                                          UserType user_type) const {
    return repo::guarded_in_transaction([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document filter;
        codec::append_enum(filter, f::kUserType, user_type);
        codec::append_enum(filter, f::kStatus, UserStatus::Active);

        mongocxx::collection users = bind(client);
        return users.count_documents(session, filter.view());
    });
}

Result<bool> UserRepository::activate(mongocxx::client& client,
                                      mongocxx::client_session& session, const Uuid& user_id,
                                      db::TimeMs now) const {
    return repo::guarded_in_transaction([&]() -> Result<bool> {
        // Both the id AND the current status are in the filter. An account
        // disabled since the code was issued must not be quietly re-enabled by
        // somebody who still holds it, and reading the status first to decide
        // would be a check-then-act with a window in the middle.
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, user_id);
        codec::append_enum(filter, f::kStatus, UserStatus::PendingVerification);

        bsoncxx::builder::basic::document set;
        codec::append_enum(set, f::kStatus, UserStatus::Active);
        codec::append_time(set, f::kUpdatedAt, now);

        mongocxx::collection users = bind(client);
        const auto before = users.find_one_and_update(
            session, filter.view(), make_document(kvp("$set", set.view())).view());
        return before.has_value();
    });
}

Result<std::optional<Uuid>> UserRepository::delete_one_stale_pending(mongocxx::client& client,
                                                                      db::TimeMs before) const {
    return repo::guarded([&]() -> Result<std::optional<Uuid>> {
        // The status predicate repeats the shape of the partial index's filter
        // verbatim. The planner uses a partial index only when it can prove the
        // query is a subset of the filter, so a query that means the same thing
        // in different words gets a COLLSCAN.
        bsoncxx::builder::basic::document filter;
        codec::append_enum(filter, f::kStatus, UserStatus::PendingVerification);
        filter.append(kvp(codec::key_of(f::kCreatedAt), [before](sub_document sub) {
            sub.append(kvp("$lt", codec::time_date(before)));
        }));

        mongocxx::options::find_one_and_delete options{};
        options.projection(make_document(kvp(codec::key_of(f::kId), 1)));

        mongocxx::collection users = bind(client);
        const auto removed = users.find_one_and_delete(filter.view(), options);
        if (!removed) { return std::optional<Uuid>{}; }
        const Result<Uuid> id = codec::read_uuid(removed->view(), f::kId);
        if (!id) { return id.error(); }
        return std::optional<Uuid>{id.value()};
    });
}

Result<std::int32_t> UserRepository::record_login_failure(
    mongocxx::client& client, const Uuid& user_id,
    const std::optional<db::TimeMs>& lock_until) const {
    return repo::guarded([&]() -> Result<std::int32_t> {
        bsoncxx::builder::basic::document update;
        update.append(kvp("$inc", [](sub_document sub) {
            sub.append(kvp(codec::key_of(f::kFailureCount), bsoncxx::types::b_int32{1}));
        }));
        update.append(kvp("$set", [&lock_until](sub_document sub) {
            // Written on BOTH branches. A lock that is set but never cleared
            // keeps an account locked after the backoff it was computed from has
            // expired, and $unset on one branch and $set on the other is two
            // shapes of update for one fact.
            if (lock_until.has_value()) {
                sub.append(kvp(codec::key_of(f::kLockUntil), codec::time_date(*lock_until)));
            } else {
                sub.append(kvp(codec::key_of(f::kLockUntil), bsoncxx::types::b_null{}));
            }
        }));

        mongocxx::options::find_one_and_update options{};
        options.return_document(mongocxx::options::return_document::k_after);
        options.projection(make_document(kvp(codec::key_of(f::kFailureCount), 1)));

        mongocxx::collection users = bind(client);
        const auto after = users.find_one_and_update(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(user_id))).view(),
            update.view(), options);
        // No such account. Not a failure: the caller reached here because a
        // credential did not match, and whether the account exists is precisely
        // what it must not learn.
        if (!after) { return 0; }
        return codec::read_int32(after->view(), f::kFailureCount);
    });
}

Status UserRepository::clear_login_failures(mongocxx::client& client,
                                            const Uuid& user_id) const {
    return repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::document set;
        set.append(kvp(codec::key_of(f::kFailureCount), bsoncxx::types::b_int32{0}));
        set.append(kvp(codec::key_of(f::kLockUntil), bsoncxx::types::b_null{}));

        mongocxx::collection users = bind(client);
        users.update_one(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(user_id))).view(),
            make_document(kvp("$set", set.view())).view());
        return ok();
    });
}

Result<std::int64_t> UserRepository::bump_perm_epoch(mongocxx::client& client,
                                                     const Uuid& user_id) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        mongocxx::options::find_one_and_update options{};
        options.return_document(mongocxx::options::return_document::k_after);
        options.projection(make_document(kvp(codec::key_of(f::kPermEpoch), 1)));

        mongocxx::collection users = bind(client);
        // One atomic $inc, never a read followed by a write of read+1. N
        // concurrent permission changes then produce N distinct epochs and none
        // is lost — and a lost epoch is a revocation that silently did not
        // happen.
        const auto after = users.find_one_and_update(
            make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(user_id))).view(),
            make_document(kvp("$inc", make_document(kvp(codec::key_of(f::kPermEpoch),
                                                        bsoncxx::types::b_int64{1}))))
                .view(),
            options);
        if (!after) { return fail(ErrorCode::NotFound, f::kId); }
        return codec::read_int64(after->view(), f::kPermEpoch);
    });
}

Status UserRepository::replace_password_hash(mongocxx::client& client, const Uuid& user_id,
                                             std::string_view expected_hash,
                                             std::string_view new_hash) const {
    return repo::guarded([&]() -> Status {
        // The hash that was verified is in the FILTER, which makes this a
        // compare-and-swap. A password change racing the background rehash would
        // otherwise be overwritten by it, resurrecting the credential the person
        // just replaced.
        bsoncxx::builder::basic::document filter;
        codec::append_uuid(filter, f::kId, user_id);
        filter.append(kvp(codec::key_of(f::kPasswordHash), text_of(expected_hash)));

        mongocxx::collection users = bind(client);
        users.update_one(filter.view(),
                         make_document(kvp("$set", make_document(kvp(
                                                       codec::key_of(f::kPasswordHash),
                                                       text_of(new_hash)))))
                             .view());
        // A miss is SUCCESS, not an error: it means the stored hash changed
        // under us, which is the outcome the filter exists to produce. The
        // rehash was an optimisation and the newer credential is the correct one.
        return ok();
    });
}

Result<std::int64_t> UserRepository::set_password_hash(mongocxx::client& client,
                                                       const Uuid& user_id,
                                                       std::int64_t expected_version,
                                                       std::string_view new_hash) const {
    return repo::guarded([&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document set;
        set.append(kvp(codec::key_of(f::kPasswordHash), text_of(new_hash)));
        // Cleared in the same write. A password change that left a lock standing
        // would refuse the credential it just issued.
        set.append(kvp(codec::key_of(f::kFailureCount), bsoncxx::types::b_int32{0}));
        set.append(kvp(codec::key_of(f::kLockUntil), bsoncxx::types::b_null{}));

        mongocxx::collection users = bind(client);
        return repo::update_versioned(
            users, make_document(kvp(codec::key_of(f::kId), codec::uuid_bin(user_id))).view(),
            expected_version, set.view());
    });
}

}  // namespace anvil::identity
