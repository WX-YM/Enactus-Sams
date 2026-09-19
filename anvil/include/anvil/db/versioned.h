#pragma once

// Optimistic concurrency: the only sanctioned way to modify a versioned
// document.
//
// find_one followed by an unconditional update_one is a lost update. Two staff
// members editing the same document from two tabs is not a rare event — it is
// the normal case for a small team working the same content — and the loser's
// write vanishes with no error anywhere (ENGINEERING_RULES.md §6).
//
// The mechanism: the expected version is part of the FILTER, and the update
// $incs it. The server matches at most one document, so exactly one of N
// concurrent writers wins and the rest see zero matched — which is reported as
// VersionMismatch and never as success. There is no window between the check
// and the act, because there is no separate check.
//
// tools/check-db-discipline.sh fails the build on a repository write that does
// not go through this header, so "we forgot the version filter" is a build
// error rather than a code-review question.

#include <cstdint>
#include <string_view>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/find_one_and_update.hpp>

#include "anvil/core/result.h"
#include "anvil/db/codec.h"
#include "anvil/db/repository.h"

namespace anvil::repo {

// Short by design: a 1-character key repeated across every versioned document
// is the cheapest field name available (docs/09-mongodb.md §2).
inline constexpr std::string_view kVersionField = "v";

// Every versioned document is created at 1, so 0 is unambiguously "never
// written" and cannot be confused with a legitimate stored version.
inline constexpr std::int64_t kInitialVersion = 1;

enum class TouchUpdatedAt : bool { No = false, Yes = true };

// Applies `$set: set_fields` to the one document matching `identity` AND the
// expected version, and increments the version.
//
// Returns the NEW version on success. Returns VersionMismatch when the server
// matched nothing — which covers both a stale version and a document that no
// longer exists. The two are deliberately not distinguished: telling a caller
// "your version is stale" versus "it is gone" on an admin route is an existence
// oracle, and the remedy for both is the same, which is to re-read and retry.
//
// `identity` is the primary key (a `_id` equality for most collections, the
// compound {k, s} for sections) plus any scoping the caller requires, such as
// an owner check. It must never contain the version — that is this function's
// job, and a caller-supplied version filter would silently allow an unversioned
// write to pass the lint.
//
// Overload set rather than a defaulted pointer: a versioned write that must
// commit together with something else — a section update and the media
// reference counts it changes — passes the caller's session, and one that
// stands alone does not. A defaulted `session = nullptr` reads as though the
// session were optional to the CORRECTNESS of the write, which for every caller
// that has one it is not (docs/09-mongodb.md §4).
namespace detail {

// Which wrapper the body needs. The session overload below runs inside a
// caller's transaction, and a transient write conflict there must reach
// with_transaction to be retried rather than becoming a 500 (base.h).
enum class TransactionScope : std::uint8_t { Standalone, InTransaction };

template <TransactionScope Scope, typename Fn>
[[nodiscard]] inline Result<std::int64_t> update_versioned_impl(
    const bsoncxx::document::view& identity, std::int64_t expected_version,
    const bsoncxx::document::view& set_fields, TouchUpdatedAt touch, Fn&& run) {
    const auto body = [&]() -> Result<std::int64_t> {
        bsoncxx::builder::basic::document filter;
        filter.append(bsoncxx::builder::concatenate(identity));
        filter.append(bsoncxx::builder::basic::kvp(
            db::codec::key_of(kVersionField), bsoncxx::types::b_int64{expected_version}));

        bsoncxx::builder::basic::document update;
        update.append(bsoncxx::builder::basic::kvp(
            "$set", [&set_fields](bsoncxx::builder::basic::sub_document sub) {
                sub.append(bsoncxx::builder::concatenate(set_fields));
            }));
        update.append(bsoncxx::builder::basic::kvp(
            "$inc", [](bsoncxx::builder::basic::sub_document sub) {
                sub.append(bsoncxx::builder::basic::kvp(db::codec::key_of(kVersionField),
                                                        bsoncxx::types::b_int64{1}));
            }));
        if (touch == TouchUpdatedAt::Yes) {
            // $currentDate, not a client clock: the stored instant then comes
            // from one authority however many application instances are running.
            update.append(bsoncxx::builder::basic::kvp(
                "$currentDate", [](bsoncxx::builder::basic::sub_document sub) {
                    sub.append(bsoncxx::builder::basic::kvp("updated_at",
                                                            bsoncxx::types::b_bool{true}));
                }));
        }

        mongocxx::options::find_one_and_update options{};
        options.return_document(mongocxx::options::return_document::k_after);
        // Only the new version comes back. Returning the whole document to read
        // one integer costs network, BSON decode CPU and heap on every write
        // (ENGINEERING_RULES.md §7).
        options.projection(bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp(db::codec::key_of(kVersionField),
                                         bsoncxx::types::b_int32{1})));

        const auto updated = run(filter.view(), update.view(), options);
        if (!updated) { return fail(ErrorCode::VersionMismatch, kVersionField); }
        return db::codec::read_int64(updated->view(), kVersionField);
    };

    if constexpr (Scope == TransactionScope::InTransaction) {
        return guarded_in_transaction(body);
    } else {
        return guarded(body);
    }
}

}  // namespace detail

[[nodiscard]] inline Result<std::int64_t> update_versioned(
    mongocxx::collection& collection, const bsoncxx::document::view& identity,
    std::int64_t expected_version, const bsoncxx::document::view& set_fields,
    TouchUpdatedAt touch = TouchUpdatedAt::Yes) {
    return detail::update_versioned_impl<detail::TransactionScope::Standalone>(
        identity, expected_version, set_fields, touch,
        [&collection](const bsoncxx::document::view& filter,
                      const bsoncxx::document::view& update,
                      const mongocxx::options::find_one_and_update& options) {
            return collection.find_one_and_update(filter, update, options);
        });
}

[[nodiscard]] inline Result<std::int64_t> update_versioned(
    mongocxx::collection& collection, mongocxx::client_session& session,
    const bsoncxx::document::view& identity, std::int64_t expected_version,
    const bsoncxx::document::view& set_fields,
    TouchUpdatedAt touch = TouchUpdatedAt::Yes) {
    return detail::update_versioned_impl<detail::TransactionScope::InTransaction>(
        identity, expected_version, set_fields, touch,
        [&collection, &session](const bsoncxx::document::view& filter,
                                const bsoncxx::document::view& update,
                                const mongocxx::options::find_one_and_update& options) {
            return collection.find_one_and_update(session, filter, update, options);
        });
}

// The version of a document just read, so a caller can pass it back to
// update_versioned without knowing the field name.
[[nodiscard]] inline Result<std::int64_t> document_version(const bsoncxx::document::view& doc) {
    return db::codec::read_int64(doc, kVersionField);
}

// Writes `v: 1` into a document being created. Every versioned document is
// created through this, so the field exists as an int64 from the first write —
// $inc preserves the stored width, so a version created as int32 would stay
// int32 and never match an int64 filter (docs/09-mongodb.md §2).
template <typename Builder>
void append_initial_version(Builder& doc) {
    db::codec::append_int64(doc, kVersionField, kInitialVersion);
}

}  // namespace anvil::repo
