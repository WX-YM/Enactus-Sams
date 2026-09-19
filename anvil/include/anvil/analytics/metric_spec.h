#pragma once

// The metrics seam: what an application needs to declare what it counts, with no
// dependency on its own configuration header.
//
// anvil ships the registry, the cells, the snapshot and the OpenMetrics writer.
// WHAT gets counted, beyond anvil's own mechanisms, is a list of the
// application's own (docs/01-seams.md §11).
//
// --- why a label's value space is part of the declaration --------------------
//
// An unbounded label space is `series x shards x 64` bytes of resident memory
// chosen by whoever can reach the increment, and that is the security risk in
// the metrics half of this subsystem. A LabelSpec therefore carries the ENTIRE
// set of values the label may take, which is what lets well_formed() multiply
// the value spaces and refuse a table whose cell count is past the ceiling. The
// registry's memory cost is then a compile-time constant and a cardinality
// explosion is a build failure rather than an OOM at 3am
// (docs/17-analytics.md §6).
//
// The second layer is the API and it is in counters.h: observe() takes label
// INDICES and has no string_view overload, so a function that cannot be handed a
// request byte cannot be made to accept one by a refactor that was not thinking
// about metrics. The third is tools/check-source-bans.sh, which bans
// std::to_string under src/analytics/ — a label value built from a number is one
// change away from a label value built from a request.
//
// --- why the ceiling counts CELLS ------------------------------------------
//
// A histogram series is `buckets + 3` cells — one per boundary, one overflow, a
// _sum and a _count — so twelve buckets and four label values is sixty cells,
// not four. A ceiling counting series would not bound anything, which is the
// whole job of having one.
//
// Same shape and same reason as anvil/db/collection_spec.h and
// anvil/notifications/topic_spec.h: an application's own `metrics.h` includes
// THIS to spell its table, and hands the table over as a std::span.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/core/ct_text.h"

namespace anvil::analytics {

// At most three labels per metric. Not a storage limit — the cell ceiling is
// already that — but a cardinality one: three label spaces multiply, and a
// fourth is almost always the point where somebody reaches for a request field.
inline constexpr std::size_t kMaxLabels = 3;

// At most twelve bucket boundaries. The bucket search is a linear scan, and at
// this size a scan of one or two cache lines beats a binary search with no
// branch misprediction worth naming (docs/17-analytics.md §5).
inline constexpr std::size_t kMaxBuckets = 12;

// The per-metric and whole-table cell ceilings.
//
// 1024 cells is roughly three times what a substantial application needs —
// twenty counters, ten gauges and five histograms come to a few hundred — and
// the right response to exceeding it is almost always to reduce a label's value
// space rather than to raise the ceiling. Raising it is a one-line change with
// the memory cost written beside it, which is the point of having the formula
// in counters.h.
inline constexpr std::size_t kMaxCellsPerMetric = 64;
inline constexpr std::size_t kMaxCells = 1024;

enum class MetricKind : std::uint8_t {
    // Accumulates. Sharded across cache lines and summed on read, which is what
    // keeps an increment to one uncontended add.
    Counter = 0,
    // A SAMPLED CURRENT VALUE — queue depth, resident rows, pool wait. NOT
    // sharded, because summing sixteen shards of a gauge reports up to sixteen
    // times the depth that exists, silently, in the one direction an operator
    // acts on (docs/17-analytics.md §4).
    Gauge = 1,
    // Fixed buckets, no quantiles. A p99 computed per process is a p99 of that
    // process, and averaging them across N instances produces a number that is
    // not a percentile of anything.
    Histogram = 2,
};

// The unit, as OpenMetrics spells it. A metric declaring one must END with it,
// which the conformance check enforces: the scrape carries a `# UNIT` line and a
// name that disagrees with it is a metric two collectors interpret differently.
//
// Bucket boundaries are integers in this unit, never double. A floating-point
// boundary makes two processes disagree about which bucket a value landed in,
// and a bucket count that differs by one between instances is indistinguishable
// from a real signal — which is why there is no Seconds here: a pool wait
// expressed in whole seconds has one useful boundary.
enum class MetricUnit : std::uint8_t {
    None = 0,
    Microseconds = 1,
    Bytes = 2,
    Rows = 3,
};

[[nodiscard]] constexpr std::string_view unit_suffix(MetricUnit unit) noexcept {
    switch (unit) {
        case MetricUnit::Microseconds: return "microseconds";
        case MetricUnit::Bytes:        return "bytes";
        case MetricUnit::Rows:         return "rows";
        case MetricUnit::None:         break;
    }
    return {};
}

// One label, and the CLOSED set of values it may take.
struct LabelSpec final {
    std::string_view                  name;    // 16
    std::span<const std::string_view> values;  // 16
};

static_assert(sizeof(LabelSpec) == 32, "LabelSpec must not grow padding");

// Ordered largest-alignment-first so the table packs.
struct MetricSpec final {
    // [a-zA-Z_:][a-zA-Z0-9_:]* and, for a counter, WITHOUT the `_total` the
    // writer appends. Carrying it here would emit `x_total_total`, which every
    // collector reads as a different series from the one the dashboard names.
    std::string_view              name;     // 16

    // Shown in the scrape and never empty. Never translated either: it is read
    // by an operator against a server, in the same place the metric name is, and
    // a localised one makes two scrapes from two processes impossible to line
    // up.
    std::string_view              help;     // 16

    std::span<const LabelSpec>    labels;   // 16

    // Histogram only, and required there. Strictly increasing, at most
    // kMaxBuckets, in the declared unit.
    std::span<const std::int64_t> buckets;  // 16

    MetricKind                    kind;     //  1
    MetricUnit                    unit;     //  1
};

static_assert(sizeof(MetricSpec) == 72, "MetricSpec must not grow padding");

// --- derived sizes ----------------------------------------------------------
//
// These are the arithmetic the conformance check and the registry both need, so
// they live here rather than being spelled twice. A registry sized by one
// formula and bounded by another is a registry whose ceiling does not bound it.

// Distinct label-value combinations. One for an unlabelled metric.
[[nodiscard]] constexpr std::size_t series_count(const MetricSpec& spec) noexcept {
    std::size_t series = 1;
    for (const LabelSpec& label : spec.labels) { series *= label.values.size(); }
    return series;
}

// Cells per series: one for a counter or gauge, `buckets + 3` for a histogram —
// one per boundary, one overflow, a _sum and a _count.
[[nodiscard]] constexpr std::size_t cells_per_series(const MetricSpec& spec) noexcept {
    return spec.kind == MetricKind::Histogram ? spec.buckets.size() + 3 : 1;
}

[[nodiscard]] constexpr std::size_t cell_count(const MetricSpec& spec) noexcept {
    return series_count(spec) * cells_per_series(spec);
}

[[nodiscard]] constexpr std::size_t table_cell_count(std::span<const MetricSpec> table) noexcept {
    std::size_t cells = 0;
    for (const MetricSpec& spec : table) { cells += cell_count(spec); }
    return cells;
}

// --- table conformance ------------------------------------------------------

namespace detail {

// The OpenMetrics name grammar. Hand-written and linear, because std::regex is
// banned everywhere in this library and would not be constexpr in any case.
[[nodiscard]] constexpr bool is_metric_name(std::string_view name) noexcept {
    if (name.empty()) { return false; }
    const char first = name.front();
    const bool head = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
                      first == '_' || first == ':';
    if (!head) { return false; }
    for (const char c : name) {
        const bool body = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == ':';
        if (!body) { return false; }
    }
    return true;
}

// A label name may not carry a colon: the colon is reserved for names produced
// by a recording rule, and a label carrying one is rejected by the collector
// rather than by us, at scrape time, on a running system.
[[nodiscard]] constexpr bool is_label_name(std::string_view name) noexcept {
    if (name.empty()) { return false; }
    const char first = name.front();
    const bool head = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
                      first == '_';
    if (!head) { return false; }
    for (const char c : name) {
        const bool body = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_';
        if (!body) { return false; }
    }
    return true;
}

[[nodiscard]] constexpr bool ends_with(std::string_view text, std::string_view tail) noexcept {
    return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

[[nodiscard]] constexpr bool has_anvil_prefix(std::string_view name) noexcept {
    return name.substr(0, 6) == "anvil_";
}

[[nodiscard]] constexpr bool spec_is_well_formed(const MetricSpec& spec) noexcept {
    if (!is_metric_name(spec.name)) { return false; }
    // Never empty. A series with no help is a series nobody on call can
    // interpret at the moment they most need to.
    if (!ct::is_non_empty_utf8(spec.help)) { return false; }
    // The writer appends `_total` to a counter, so a name carrying it would emit
    // `_total_total` — a different series from the one every dashboard names.
    if (spec.kind == MetricKind::Counter && ends_with(spec.name, "_total")) { return false; }
    if (spec.unit != MetricUnit::None) {
        if (!ends_with(spec.name, unit_suffix(spec.unit))) { return false; }
        // `_microseconds` and not `microseconds`: the suffix is a name segment.
        const std::size_t boundary = spec.name.size() - unit_suffix(spec.unit).size();
        if (boundary == 0 || spec.name[boundary - 1] != '_') { return false; }
    }

    if (spec.kind == MetricKind::Histogram) {
        if (spec.buckets.empty() || spec.buckets.size() > kMaxBuckets) { return false; }
        for (std::size_t i = 1; i < spec.buckets.size(); ++i) {
            // Strictly increasing. Two equal boundaries make one bucket
            // unreachable, and the cumulative form at snapshot then reports a
            // count that never moves.
            if (spec.buckets[i] <= spec.buckets[i - 1]) { return false; }
        }
    } else if (!spec.buckets.empty()) {
        return false;
    }

    if (spec.labels.size() > kMaxLabels) { return false; }
    for (std::size_t i = 0; i < spec.labels.size(); ++i) {
        const LabelSpec& label = spec.labels[i];
        if (!is_label_name(label.name)) { return false; }
        // A label with no declared values contributes a zero to the product, so
        // the metric would have no series at all and every observe() against it
        // would silently do nothing.
        if (label.values.empty()) { return false; }
        for (std::size_t v = 0; v < label.values.size(); ++v) {
            if (!ct::is_non_empty_utf8(label.values[v])) { return false; }
            for (std::size_t w = 0; w < v; ++w) {
                if (label.values[w] == label.values[v]) { return false; }
            }
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (spec.labels[j].name == label.name) { return false; }
        }
    }

    return cell_count(spec) <= kMaxCellsPerMetric;
}

[[nodiscard]] constexpr bool table_is_well_formed(std::span<const MetricSpec> table,
                                                  bool internal) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        const MetricSpec& spec = table[i];
        if (!spec_is_well_formed(spec)) { return false; }
        // Enforced in BOTH directions, which is what keeps the two tables from
        // ever colliding and what lets a reader of a scrape tell at a glance
        // which side a series came from.
        if (has_anvil_prefix(spec.name) != internal) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (table[j].name == spec.name) { return false; }
        }
    }
    return table_cell_count(table) <= kMaxCells;
}

}  // namespace detail

// The application's table. A malformed one is a build error, not a runtime
// surprise: every condition here is a mistake that would otherwise ship as a
// scrape nothing can read or as resident memory nobody budgeted.
[[nodiscard]] constexpr bool metric_table_is_well_formed(
    std::span<const MetricSpec> table) noexcept {
    return detail::table_is_well_formed(table, false);
}

// anvil's own table, which must carry the prefix the application's may not.
[[nodiscard]] constexpr bool internal_metric_table_is_well_formed(
    std::span<const MetricSpec> table) noexcept {
    return detail::table_is_well_formed(table, true);
}

// The two tables share one ceiling, because they share one registry. Checking
// each against kMaxCells and neither against the sum is a bound that does not
// bound the thing it is written on.
[[nodiscard]] constexpr bool metric_tables_fit_together(
    std::span<const MetricSpec> internal, std::span<const MetricSpec> application) noexcept {
    return table_cell_count(internal) + table_cell_count(application) <= kMaxCells;
}

}  // namespace anvil::analytics
