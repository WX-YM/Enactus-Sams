#pragma once

// What every repository shares: the collection names, the driver-error
// translation, and the two filter rules that must never be forgotten.
//
// A repository never sees an HttpRequestPtr and a controller never sees BSON
// (docs/00-architecture.md §2). Repositories take a mongocxx::client& rather
// than acquiring one themselves, so a service performing several writes inside
// one transaction uses one client and one session — acquiring per call would
// make that impossible and would hide a second pool wait inside every method.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/operation_exception.hpp>

#include "anvil/core/result.h"
#include "anvil/db/codec.h"

namespace anvil::repo {

// Collection names are compile-time constants and are never derived from
// request data. A name built from a request-supplied id is collection-name
// injection, and it is why form submissions live in one partitioned collection
// instead of one collection per form.
// Collection names, the database each lives in, and which of them hold rows with
// a LIFETIME all come from the application's table — see anvil/db/collections.h
// and docs/01-seams.md. anvil never derives either name from request data, and
// the table is what makes that structural rather than a rule somebody follows.

// Appends `{<field>: {$gt: <now>}}`. Read as: only documents the TTL monitor has
// not yet reached but whose expiry has not passed are visible.
template <typename Builder>
void append_not_expired(Builder& filter, std::string_view field, db::TimeMs now) {
    filter.append(bsoncxx::builder::basic::kvp(
        db::codec::key_of(field), [now](bsoncxx::builder::basic::sub_document sub) {
            sub.append(bsoncxx::builder::basic::kvp(db::codec::key_of("$gt"),
                                                    db::codec::time_date(now)));
        }));
}

// Driver exception -> Failure. Two outcomes matter to a caller:
//
//   Conflict            a unique index rejected the write. That is a business
//                       outcome (email taken, capability already consumed, a
//                       second submission for a one-per-user form), not a fault.
//   ServiceUnavailable  the driver never reached a server. The request should
//                       shed with 503 rather than report a 500, and the circuit
//                       breaker in docs/09-mongodb.md §8 keys off exactly this
//                       outcome.
//
// Everything else is Internal. No branch of this function puts driver text,
// a query fragment, or a submitted value into the Failure (ENGINEERING_RULES.md §5).
[[nodiscard]] Failure translate(const mongocxx::operation_exception& error) noexcept;
[[nodiscard]] Failure translate(const mongocxx::exception& error) noexcept;

// True when the server labelled the error TransientTransactionError: the whole
// transaction was rolled back and the only correct response is to run it again.
// The label is attached ONLY to failures inside a transaction, which is what
// makes "rethrow it" a safe rule rather than a guess.
[[nodiscard]] bool is_transient_transaction_error(
    const mongocxx::operation_exception& error) noexcept;

// Wraps a repository body so no driver exception escapes into a thread-pool
// task, where it would call std::terminate. `body` returns the
// same Result<T> this returns.
//
// Use this ONLY outside a transaction. Inside one, use guarded_in_transaction:
// swallowing a driver exception there breaks the retry the whole transaction
// convention depends on. See the comment there.
template <typename Fn>
[[nodiscard]] auto guarded(Fn&& body) noexcept -> decltype(body()) {
    try {
        return body();
    } catch (const mongocxx::operation_exception& e) {
        return translate(e);
    } catch (const mongocxx::exception& e) {
        return translate(e);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

// The in-transaction counterpart, for every repository method that takes a
// `client_session&`.
//
// mongocxx::client_session::with_transaction retries the callback when the
// server reports a TransientTransactionError — a write conflict on a contended
// document is the ordinary case, and retrying is how a transaction is SUPPOSED
// to make progress. The driver states the requirement outright: "the user
// callback MUST allow those exceptions to propagate up the stack so they can be
// caught and processed by the with_transaction() helper."
//
// guarded() cannot do that. It converts the exception into a Failure, the
// service turns that Failure into its own abort exception, and the helper never
// sees a retryable error — so a routine write conflict became a 500 and 56% of
// transaction work was thrown away under load. Rethrowing the labelled
// exception, and ONLY the labelled one, restores the retry while keeping every
// other driver failure translated exactly as before.
//
// Deliberately not noexcept: the rethrow is the point. It is bounded — the
// exception is thrown inside a with_transaction callback and caught by that
// helper, so it never reaches a pool task — where an escaping exception calls
// std::terminate and takes the process down (ENGINEERING_RULES.md §4).
template <typename Fn>
[[nodiscard]] auto guarded_in_transaction(Fn&& body) -> decltype(body()) {
    try {
        return body();
    } catch (const mongocxx::operation_exception& e) {
        if (is_transient_transaction_error(e)) { throw; }
        return translate(e);
    } catch (const mongocxx::exception& e) {
        return translate(e);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

// --- running a transaction --------------------------------------------------
//
// Two things belong around with_transaction and neither belongs INSIDE
// guarded_in_transaction, which is the question docs/15 asked and this is the
// answer. guarded_in_transaction runs inside the callback: it sees one attempt
// and translates its exceptions, and it cannot see the retry boundary at all
// because the retry is the driver's. Both of these are about the boundary, so
// they live in a wrapper around the call rather than inside the body of it.
//
// --- what the counter is for ------------------------------------------------
//
// A retried transaction produces no error response and no log line, so it is
// invisible from outside — which is why docs/00 §9 names the counter and says to
// alert when it stops tracking request volume. A load run against an application
// built on this library measured 393,116 aborted attempts, and nothing in a
// passing test suite could have shown it.
//
// The attempts are charged to the OUTCOME of the transaction that contained
// them, because 393,116 aborts that all eventually committed and 393,116 that
// did not are different incidents.
//
// --- what the backoff is for ------------------------------------------------
//
// with_transaction retries a labelled transient error IMMEDIATELY, and keeps
// doing so for up to 120 seconds. anvil added no delay of its own, so N workers
// contending for one document became a tight loop against the one document they
// were all waiting for — each retry arriving at exactly the moment the others
// did, which is the shape that makes a conflict storm self-sustaining rather
// than self-clearing.
//
// Full jitter, exponential, bounded: the delay before attempt n is a uniform
// draw from [0, min(kMaxTransactionBackoff, 2^(n-1) x kBaseTransactionBackoff)).
// A uniform draw and not a fixed delay, because a fixed one re-synchronises the
// workers it was meant to spread — they all wait the same time and collide
// again. The draw comes from crypto/random.h's rejection sampler for the reason
// stated there: `% bound` biases towards the low values, and jitter that
// clusters is jitter that does not do its job.
//
// It SLEEPS, on the calling thread. That is only correct because every caller is
// already on db_pool — the driver is synchronous, so a transaction is a blocking
// call by construction — and a loop thread may never reach here (ENGINEERING_RULES.md §4).
inline constexpr std::chrono::microseconds kBaseTransactionBackoff{2000};
inline constexpr std::chrono::microseconds kMaxTransactionBackoff{64000};

// Sleeps before `attempt`, which is 1-based: attempt 1 never sleeps, because the
// first try is not a retry.
void transaction_backoff(std::uint32_t attempt);

// Charges `attempts - 1` aborted attempts to the outcome of the transaction.
void count_transaction_attempts(std::uint32_t attempts, bool committed) noexcept;

// The one way this library starts a transaction.
//
// `body` takes a `mongocxx::client_session*`, exactly as with_transaction's
// callback does, so a call site changes by one line and the body is unchanged.
// Exceptions propagate: an application's own abort exception is how a
// transaction reports a business outcome, and swallowing one here would turn a
// rejected write into a committed one.
template <typename Fn>
void in_transaction(mongocxx::client_session& session, Fn&& body) {
    std::uint32_t attempts = 0;
    try {
        session.with_transaction([&](mongocxx::client_session* txn) {
            // Inside the callback because the driver exposes no hook between
            // attempts. The sleep is therefore inside the transaction's own time
            // limit, which is correct: the limit bounds how long one logical
            // transaction may spend making progress, and waiting for a contended
            // document is what it is spending it on.
            transaction_backoff(++attempts);
            body(txn);
        });
    } catch (...) {
        count_transaction_attempts(attempts, false);
        throw;
    }
    count_transaction_attempts(attempts, true);
}

class RepositoryBase {
public:
    [[nodiscard]] std::string_view database() const noexcept { return database_; }
    [[nodiscard]] std::string_view collection_name() const noexcept { return collection_; }

protected:
    // `collection` is one of the constants above and therefore has static
    // storage duration; holding a view rather than a string keeps the object
    // small and allocation-free.
    RepositoryBase(std::string database, std::string_view collection) noexcept
        : database_{std::move(database)}, collection_{collection} {}

    // Not polymorphic: no repository is ever deleted through this type, so it
    // carries no vtable and stays trivially destructible (rule of zero).
    ~RepositoryBase() = default;
    RepositoryBase(const RepositoryBase&) = default;
    RepositoryBase& operator=(const RepositoryBase&) = default;
    RepositoryBase(RepositoryBase&&) noexcept = default;
    RepositoryBase& operator=(RepositoryBase&&) noexcept = default;

    // The client is not thread-safe and the returned collection borrows from it:
    // both are valid only inside the pool task that acquired the client
    // (docs/09-mongodb.md §2).
    [[nodiscard]] mongocxx::collection bind(mongocxx::client& client) const {
        return client[database_][std::string{collection_}];
    }

private:
    std::string      database_;
    std::string_view collection_;
};

}  // namespace anvil::repo
