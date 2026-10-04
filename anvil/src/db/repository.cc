#include "anvil/db/repository.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <thread>

#include <bsoncxx/array/element.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/analytics/counters.h"
#include "anvil/crypto/random.h"

namespace anvil::repo {
namespace {

// The server's duplicate-key code.
constexpr std::int32_t kDuplicateKey = 11000;

// Runs `visit` over every place a server error code can appear in a reply. There
// are THREE, and which one is used depends on how the command failed:
//
//   code           a command-level failure, such as findAndModify
//   writeErrors[]  a per-document failure inside an insert or update
//   errorReplies[] a command that failed INSIDE A TRANSACTION
//
// The third was missing, and its absence was not cosmetic: a write conflict and
// a duplicate key raised inside a transaction both land there, so both were read
// as "no code at all" and fell through to Internal.
template <typename Fn>
void for_each_error_code(const bsoncxx::document::view& error, Fn&& visit) {
    if (const bsoncxx::document::element code = error["code"];
        code && code.type() == bsoncxx::type::k_int32) {
        visit(code.get_int32().value);
    }
    for (const char* array_field : {"writeErrors", "errorReplies"}) {
        const bsoncxx::document::element entries = error[array_field];
        if (!entries || entries.type() != bsoncxx::type::k_array) { continue; }
        for (const bsoncxx::array::element& entry : entries.get_array().value) {
            if (entry.type() != bsoncxx::type::k_document) { continue; }
            if (const bsoncxx::document::element code = entry.get_document().value["code"];
                code && code.type() == bsoncxx::type::k_int32) {
                visit(code.get_int32().value);
            }
        }
    }
}

// The numeric server code, for the log line only. Never for a response: a
// server error code names the operation that failed and is exactly the kind of
// internal detail §4 keeps out of a reply.
[[nodiscard]] std::int32_t server_code_of(const bsoncxx::document::view& error) noexcept {
    std::int32_t found = 0;
    for_each_error_code(error, [&found](std::int32_t code) {
        if (found == 0) { found = code; }
    });
    return found;
}

[[nodiscard]] bool is_duplicate_key(const bsoncxx::document::view& error) noexcept {
    bool duplicate = false;
    for_each_error_code(error, [&duplicate](std::int32_t code) {
        if (code == kDuplicateKey) { duplicate = true; }
    });
    return duplicate;
}

}  // namespace

Failure translate(const mongocxx::operation_exception& error) noexcept {
    const auto& raw = error.raw_server_error();
    if (!raw) {
        // No server reply at all: server selection timed out, the connection
        // dropped, or the primary is mid-election. The write never happened and
        // the caller should shed rather than report an internal fault.
        return fail(ErrorCode::ServiceUnavailable);
    }
    if (is_duplicate_key(raw->view())) { return fail(ErrorCode::Conflict); }
    // An Internal from here means a server error this layer does not model, and
    // the caller returns an opaque 500. Without this line that 500 is invisible:
    // the response carries no detail by design, so the log is the ONLY place the
    // cause can exist (CLAUDE.md §5). The code is logged, never the message —
    // a driver message can quote the offending document.
    LOG_ERROR << "unmodelled MongoDB error, server code " << server_code_of(raw->view());
    return fail(ErrorCode::Internal);
}

bool is_transient_transaction_error(const mongocxx::operation_exception& error) noexcept {
    // The driver reads the label off the reply for us. Parsing errorLabels by
    // hand would mean tracking where the server chooses to put it, which is the
    // mistake for_each_error_code above exists to undo.
    return error.has_error_label("TransientTransactionError");
}

Failure translate(const mongocxx::exception& error) noexcept {
    // The base type covers client-side failures — URI, authentication, server
    // selection — none of which reached a server, so they read as unavailable
    // rather than internal. error is unused beyond its type: its message is for
    // the server-side log, never for the caller (CLAUDE.md §5).
    static_cast<void>(error);
    return fail(ErrorCode::ServiceUnavailable);
}

void transaction_backoff(std::uint32_t attempt) {
    // The first try is not a retry.
    if (attempt <= 1) { return; }

    // Doubling, capped, and shifted by at most 30 so the shift itself cannot be
    // undefined on a transaction that somehow retried thirty-two times.
    const std::uint32_t doublings = std::min<std::uint32_t>(attempt - 2, 30);
    const std::uint64_t ceiling =
        std::min<std::uint64_t>(static_cast<std::uint64_t>(kMaxTransactionBackoff.count()),
                                static_cast<std::uint64_t>(kBaseTransactionBackoff.count())
                                    << doublings);

    std::uint32_t waited_us = 0;
    try {
        // FULL jitter: a uniform draw from [0, ceiling), not ceiling itself. A
        // fixed delay re-synchronises the workers it was meant to spread — they
        // all wait the same time and collide again on the same document.
        waited_us = crypto::random_below(static_cast<std::uint32_t>(ceiling));
    } catch (const std::exception&) {
        // The CSPRNG is unavailable, which is a much larger problem than this
        // sleep. Back off by the ceiling rather than not at all: an unjittered
        // delay still breaks the tight loop, and throwing from here would turn a
        // routine write conflict into a failed transaction.
        waited_us = static_cast<std::uint32_t>(ceiling);
    }
    if (waited_us == 0) { return; }
    std::this_thread::sleep_for(std::chrono::microseconds{waited_us});
}

void count_transaction_attempts(std::uint32_t attempts, bool committed) noexcept {
    if (attempts <= 1) { return; }
    // attempts - 1, because the first attempt was not an abort. Charged to the
    // outcome of the transaction that contained them: aborts that eventually
    // committed and aborts that did not are different incidents, and one number
    // for both is a number nobody can act on.
    analytics::count_many(analytics::Internal::TransactionsAborted,
                          committed ? analytics::TxnOutcome::Committed
                                    : analytics::TxnOutcome::Failed,
                          attempts - 1);
}

}  // namespace anvil::repo
