#pragma once

// The scrape body, and the policy that decides who may ask for it.
//
// Appended into ONE caller-owned std::string after a single reserve(), exactly
// as anvil/http/json_writer.h and anvil/http/csv_writer.h do, and for the same
// reason: a serialiser that allocates per field allocates once per series, and
// there are a thousand of them.
//
// --- byte-identical, and why that is a requirement --------------------------
//
// Two scrapes of an UNCHANGED registry are byte-identical, series order
// included. That is not tidiness — it is what lets an operator diff two scrapes
// and see only what moved. It falls out of the snapshot's layout (the order IS
// the structure, anvil/analytics/snapshot.h), so nothing here sorts and nothing
// here iterates a hash map.
//
// --- what is NOT here -------------------------------------------------------
//
// No quantiles. A p99 computed per process is a p99 of that process, and
// averaging them across N instances produces a number that is not a percentile
// of anything. The scrape emits buckets; the thing that aggregates across
// instances is the thing that can compute a quantile, and it is not this process
// (docs/17-analytics.md §5).
//
// No route. anvil registers none, here as everywhere: it ships ScrapePolicy and
// the application mounts the endpoint, declares it in its route table like any
// other, and gets the fail-closed treatment from docs/04-access-control.md for
// free.

#include <cstddef>
#include <string>
#include <string_view>

#include "anvil/analytics/snapshot.h"
#include "anvil/core/perm_set.h"
#include "anvil/http/client_address.h"

namespace anvil::analytics {

inline constexpr std::string_view kOpenMetricsContentType =
    "application/openmetrics-text; version=1.0.0; charset=utf-8";

// Appends the whole document, `# EOF` included. The caller reserves once; this
// never does.
void append_openmetrics(std::string& out, const Snapshot& snapshot);

// What to reserve. An over-estimate by construction — it assumes the widest
// plausible decimal for every value — because reserving short costs a
// reallocation and a copy of the whole body, and reserving long costs bytes that
// are freed at the end of the request either way.
[[nodiscard]] std::size_t estimate_openmetrics_bytes(const Snapshot& snapshot) noexcept;

// Escapes `\`, `\n` and `"`, per the one escaped-string production OpenMetrics
// uses for both HELP text and label values. Exposed because it is asserted
// directly: an unescaped quote in a label value splits the sample into two the
// collector reads as malformed, and an unescaped newline ends the line early —
// both of which corrupt every series AFTER the one carrying it.
void append_openmetrics_escaped(std::string& out, std::string_view text);

// --- who may scrape ---------------------------------------------------------
//
// Queue depths, cache hit rates, denial counts and transaction aborts are a live
// map of where the system is weak and when it is weakest. They are exactly what
// an attacker would collect first, so the endpoint is BOTH permissioned and
// CIDR-restricted — two independent controls, because a permission bit leaks
// with a token and a network range does not (docs/17-analytics.md §6).
class ScrapePolicy final {
public:
    ScrapePolicy() = default;

    // `cidrs` is METRICS_SCRAPE_CIDRS: comma-separated CIDRs, a bare address
    // meaning a single host. False on a malformed entry, so a deployment can
    // refuse to boot rather than silently restricting to nothing.
    //
    // The matcher is http::TrustedProxies, which is a CIDR set with a
    // proxy-shaped name. Reused rather than reimplemented: a second CIDR parser
    // is a second chance to get a prefix length wrong, and the v4-mapped
    // handling here is the part that is easy to get wrong.
    [[nodiscard]] bool parse_cidrs(std::string_view cidrs) noexcept {
        return sources_.parse(cidrs);
    }

    void set_required(const PermSet& required) noexcept { required_ = required; }

    [[nodiscard]] const PermSet& required() const noexcept { return required_; }

    // DENY BY DEFAULT: an empty CIDR set allows nobody. A deployment that has
    // not set METRICS_SCRAPE_CIDRS has not decided who may scrape, and the
    // failing direction of that is a 404 on a metrics route rather than a live
    // map of the system served to whoever asks.
    [[nodiscard]] bool allows(const http::PackedAddress& source) const noexcept {
        return !sources_.empty() && sources_.contains(source);
    }

    [[nodiscard]] std::size_t cidr_count() const noexcept { return sources_.size(); }

private:
    // Largest first: PermSet is 16 bytes, TrustedProxies carries a fixed array.
    PermSet              required_{};
    http::TrustedProxies sources_{};
};

}  // namespace anvil::analytics
