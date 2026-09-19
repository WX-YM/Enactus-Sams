#pragma once

// Who a visitor is, without ever storing who they are.
//
//   visitor = HMAC-SHA256(ANALYTICS_VISITOR_PEPPER, packed_address || day)[0:16]
//
// Five properties, in the order they matter (docs/17-analytics.md §11):
//
//   * THE ADDRESS NEVER REACHES A ROW. Not truncated, not hashed without a key.
//     An IPv4 address is a 32-bit input space, and an unkeyed digest of one is
//     reversible by anybody holding a database dump in the time it takes to
//     enumerate it. The pepper is what makes the derivation one-way in practice,
//     and it lives in a SecretBuffer like every other secret.
//   * THE INPUT IS THE ADDRESS AND THE DAY, AND NOTHING ELSE. A user-agent
//     string looks like free entropy and is not: it makes a browser update a new
//     visitor, which inflates the count the measurement exists to produce, and
//     it widens the input beyond what a keyed digest of a 32-bit space needs.
//   * IT ROTATES DAILY, so the same visitor on two days is two ids. That bounds
//     what the collection can be used to reconstruct to a day, which is also the
//     window the product question needs.
//   * IPv6 IS COARSENED TO /64 AND IPv4 IS NOT. PackedAddress is sixteen bytes
//     with v4 mapped into it, so the naive implementation hashes all of them —
//     and privacy addressing rotates the v6 interface identifier, minting a new
//     visitor for the same person several times a day and inflating exactly the
//     number this exists to produce. /64 is the smallest block a site is
//     assigned, so it is the coarsest cut that does not merge households. Going
//     further, or coarsening v4 at all, merges everyone behind one NAT into a
//     single visitor and deflates it instead.
//   * A SESSION IS A (visitor, day) UPSERT, and the unique index IS the
//     sessionisation — there is no read-then-write, so N instances converge with
//     no coordination at all (ENGINEERING_RULES.md §6).
//
// PackedAddress comes from anvil/http/client_address.h, which already resolves
// the true client behind the trusted proxy set. Analytics does not re-do that
// and must not: two implementations of "who is the client" is one of them being
// wrong.

#include <cstdint>

#include "anvil/analytics/event.h"
#include "anvil/crypto/secret.h"
#include "anvil/db/codec.h"
#include "anvil/http/client_address.h"

namespace anvil::analytics {

// Days since the Unix epoch, UTC. Stored on the session row and mixed into the
// digest, so "the same visitor tomorrow" is a different id by construction
// rather than by a job that has to remember to rotate something.
using DayNumber = std::int32_t;

[[nodiscard]] DayNumber day_of(db::TimeMs at) noexcept;

// The visitor pepper. 32 bytes, from ANALYTICS_VISITOR_PEPPER.
//
// Installed once at boot and never rotated on a running process: rotating it
// mid-day would split one visitor into two and report the split as growth.
// Rotating it BETWEEN days is free, because the day is in the input anyway.
void install_visitor_pepper(crypto::Key256 pepper);

// Whether a pepper has been installed. An UNPEPPERED deployment does not fall
// back to an unkeyed digest — see visitor_id — so this is what lets boot refuse
// to start rather than silently recording reversible ids.
[[nodiscard]] bool visitor_pepper_installed() noexcept;

// Drops the installed pepper, which cleanses its SecretBuffer.
//
// Called at SHUTDOWN, beside Pools::shutdown, for the reason every secret in
// this library is held in a SecretBuffer at all: a key that lives until process
// exit is a key in a core dump. Sessionisation stops working the moment this
// runs — every subsequent visitor_id is all-zero and every subsequent offer is
// refused — which is correct on the way out and a mistake anywhere else.
void clear_visitor_pepper() noexcept;

// The 16-byte visitor id, or all zeroes when no pepper is installed.
//
// All-zero is the "no visitor" value, and returning it is the correct failure:
// an unpeppered digest of a 32-bit input space is reversible from a dump, so
// falling back to one would turn a configuration mistake into a privacy breach
// that looks exactly like success. The ingest path refuses a zero visitor.
[[nodiscard]] VisitorId visitor_id(const http::PackedAddress& address, DayNumber day) noexcept;

// The /64 cut, exposed because it is asserted directly: a v6 address that
// differs only in its interface identifier must produce the SAME visitor, and a
// v4 address must not be coarsened at all.
[[nodiscard]] http::PackedAddress coarsen_for_visitor(
    const http::PackedAddress& address) noexcept;

// --- sampling ---------------------------------------------------------------
//
// Above a high-water mark the sink samples, and it samples DETERMINISTICALLY PER
// SESSION: a session is kept whole or dropped whole. Per-event sampling keeps a
// random half of every session, and a half-observed funnel is worse than an
// unobserved one — it reports a drop-off that is an artefact of the sampler, and
// there is no way to tell it from a real one afterwards
// (docs/17-analytics.md §13).
//
// It lives here rather than beside the sink because it is a pure function of a
// session id and nothing else, which is exactly what makes it testable a
// thousand times over without a sink at all.
//
// A denominator of 0 or 1 keeps everything: sampling is a pressure valve, not a
// policy.
[[nodiscard]] bool session_is_sampled_in(const VisitorId& session,
                                         std::uint32_t denominator) noexcept;

[[nodiscard]] constexpr bool is_anonymous_visitor(const VisitorId& id) noexcept {
    for (const std::uint8_t byte : id) {
        if (byte != 0) { return false; }
    }
    return true;
}

}  // namespace anvil::analytics
