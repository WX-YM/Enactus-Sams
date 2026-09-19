#pragma once

// A reading of every cell, taken on demand.
//
// A PULL SCRAPE NEEDS NO PERIODIC TASK AT ALL: each counter's shards are summed
// on read and each gauge is read directly, into one buffer the caller reserves
// once and reuses. Summing on read rather than maintaining a running total is
// what keeps the write path to a single uncontended add
// (docs/17-analytics.md §7).
//
// The buffer carries the values and NOTHING ELSE — no metric name, no series
// index, no per-row header. The order IS the structure: metrics in table order,
// anvil's own first, then for each metric its series in label-major order, then
// for each series its slots. A reader walks the same tables the registry was
// built from and consumes the values in step, which is what makes two snapshots
// of an unchanged registry byte-identical without anything having to sort.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/metric_spec.h"

namespace anvil::analytics {

// --- gauge sampling ---------------------------------------------------------
//
// A gauge is a SAMPLED CURRENT VALUE, so something has to sample it, and the
// only moment that produces a coherent scrape is immediately before the cells
// are read. A sampler installed at boot cannot be forgotten by the route that
// scrapes, which a "remember to call sample_pool_depths() first" comment
// eventually is.
//
// Samplers run on the scraping thread. They must be cheap and must not block:
// this is a request path, and a sampler that queries the database turns a scrape
// into a database load test that fires every fifteen seconds.

using GaugeSampler = std::function<void(Registry&)>;

void install_gauge_sampler(GaugeSampler sampler);
void clear_gauge_samplers() noexcept;

class Snapshot final {
public:
    // Reserves from the registry's two tables. Nothing here grows afterwards:
    // collect() is a request path, and a scrape that reallocates is a scrape
    // whose cost depends on how many times it has run.
    explicit Snapshot(Registry& registry);

    // Runs every installed gauge sampler, then reads every cell. Allocates
    // nothing.
    void collect();

    [[nodiscard]] std::span<const std::uint64_t> values() const noexcept { return values_; }
    [[nodiscard]] const Registry& registry() const noexcept { return *registry_; }

    // The offset of (metric, series, slot) in values(). Linear over the tables,
    // and deliberately NOT cached: the writer walks the snapshot sequentially
    // and never needs it, so caching would be a second index maintained for a
    // diagnostic path.
    [[nodiscard]] std::size_t offset_of(std::size_t metric, std::size_t series,
                                        std::size_t slot) const noexcept;

    [[nodiscard]] std::uint64_t value_of(std::size_t metric, std::size_t series,
                                         std::size_t slot) const noexcept;

    // The spec of the metric at `index` in the concatenated table.
    [[nodiscard]] const MetricSpec& spec_at(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t metric_count() const noexcept;

private:
    // Declaration order is construction order: values_ is sized from *registry_.
    Registry*                  registry_;
    std::vector<std::uint64_t> values_;
};

// Total values a snapshot of these two tables holds: SUM over metrics of
// series x slots. Not the CELL count — that multiplies by shards, and a shard is
// exactly what a snapshot removes.
[[nodiscard]] constexpr std::size_t snapshot_value_count(
    std::span<const MetricSpec> internal, std::span<const MetricSpec> application) noexcept {
    std::size_t values = 0;
    for (const MetricSpec& spec : internal) { values += cell_count(spec); }
    for (const MetricSpec& spec : application) { values += cell_count(spec); }
    return values;
}

}  // namespace anvil::analytics
