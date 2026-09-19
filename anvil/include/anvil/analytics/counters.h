#pragma once

// The cells, and the only thing on an increment path.
//
// --- the rule this exists to satisfy ----------------------------------------
//
// docs/00-architecture.md §9: a log line per occurrence is not a metric.
// Anything that can happen once per request must be counted and reported
// periodically, never logged per event — under the load that makes it fire, a
// per-event line is itself the outage. That is a statement about COST, and its
// corollary is the whole of this file: an increment must cost no allocation, no
// lock and no contended cache line. A counter that is cheap at ten requests per
// second and expensive at ten thousand switches itself off exactly when it is
// being read.
//
// Three properties carry it, and each is load-bearing:
//
//   * THE INDEX IS COMPUTED, NOT LOOKED UP. There is no map, because a lookup by
//     name on an increment path is a hash of a string that was already known at
//     compile time. A metric's base offset is fixed at construction and the
//     label values arrive as indices, so an increment is an add, a multiply and
//     a fetch_add(relaxed).
//   * SHARDS ARE PADDED TO A CACHE LINE, so two threads incrementing the same
//     series never write the same line. False sharing on a hot counter costs
//     more than the thing being counted, which is how a metric becomes the
//     reason the number it reports is bad.
//   * THE SHARD IS THE THREAD'S, not the caller's choice — assigned at first
//     touch and never again. Relaxed ordering is correct because nothing depends
//     on a counter's value ordering against anything else: a snapshot reads a
//     value between the first and last observation, which is exactly what a
//     monotonically increasing counter means.
//
// --- a counter is sharded; a GAUGE IS NOT -----------------------------------
//
// Conflating the two is the easiest way to make this whole subsystem report
// nonsense. A counter accumulates, so N shards summed on read IS the counter —
// that is what sharding is for. A gauge is a sampled current value: queue depth,
// resident rows, pool wait. Summing sixteen shards of a gauge reports up to
// sixteen times the depth that exists, silently, in the one direction an
// operator would act on.
//
// So a gauge is ONE cell, stored and loaded relaxed, and it is SET-ONLY.
// Anything you would want to increment and decrement is two counters whose
// difference is the answer, which also survives a process restart honestly where
// a decremented gauge does not.
//
// --- the memory bound -------------------------------------------------------
//
//   bytes = SUM over cells  (gauge ? 1 : shards) x 64
//         <= kMaxCells x kMaxShards x 64  =  1 MiB
//
// A cardinality explosion is therefore a build failure rather than an OOM at
// 3am, and the ceiling is enforced by metric_table_is_well_formed rather than
// here (anvil/analytics/metric_spec.h, docs/17-analytics.md §6).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

#include "anvil/analytics/internal_metrics.h"
#include "anvil/analytics/metric_spec.h"

namespace anvil::analytics {

// A metric, as an index into the CONCATENATED table: anvil's own first, then the
// application's. Concatenated rather than kept apart because the scrape emits one
// document and two registries would be two allocations, two snapshots and two
// chances for a series order to differ between them.
struct MetricId final {
    std::uint16_t index;

    [[nodiscard]] constexpr bool operator==(const MetricId&) const noexcept = default;
};

inline constexpr std::size_t kInternalMetricCount = kInternalMetrics.size();

[[nodiscard]] constexpr MetricId metric_of(Internal metric) noexcept {
    return MetricId{static_cast<std::uint16_t>(metric)};
}

// The application's metric i. Resolved through a function rather than by adding
// a constant at the call site, because the offset is anvil's business and a call
// site that spells it is a call site that breaks when anvil declares a ninth.
[[nodiscard]] constexpr MetricId application_metric(std::size_t table_index) noexcept {
    return MetricId{static_cast<std::uint16_t>(kInternalMetricCount + table_index)};
}

// A label VALUE, as an INDEX into the label's declared value space.
//
// There is no std::string_view overload anywhere in this file. Not as a
// convenience and not as an escape hatch: a function that cannot be handed a
// request byte cannot be made to accept one by a refactor that was not thinking
// about metrics (docs/17-analytics.md §6).
//
// Implicit from an enum, because an enum is the intended way to name a value and
// a cast at every call site is a cast somebody eventually gets wrong. Explicit
// from an integer, because a bare number at a call site should have to say what
// it is.
struct LabelIndex final {
    std::uint16_t value;

    template <typename Enum, typename = std::enable_if_t<std::is_enum_v<Enum>>>
    constexpr LabelIndex(Enum enumerator) noexcept   // NOLINT(google-explicit-constructor)
        : value{static_cast<std::uint16_t>(enumerator)} {}

    constexpr explicit LabelIndex(std::uint16_t index) noexcept : value{index} {}
};

class Registry final {
public:
    static constexpr std::size_t kCacheLineBytes = 64;
    // The memory half of the memory-versus-contention trade. Sixteen shards of a
    // counter is 1 KiB per series; the contention half saturates well before
    // that on any machine this library targets.
    static constexpr std::size_t kMaxShards = 16;

    // One cell, on a line no other cell shares.
    struct alignas(kCacheLineBytes) Cell final {
        std::atomic<std::uint64_t> value;
    };

    static_assert(sizeof(Cell) == kCacheLineBytes);

    // `internal` is normally kInternalMetrics; it is a parameter so a test can
    // build a registry over a table it controls without also getting anvil's.
    Registry(std::span<const MetricSpec> internal, std::span<const MetricSpec> application);
    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // --- increment paths ----------------------------------------------------
    //
    // Every one of these is a no-op on a metric id, label index or kind that
    // does not match the table. Refusing to write is the only safe direction: an
    // out-of-range label index is the one thing between here and a write past
    // the end of the arena, and a metrics call must never be the reason a
    // process dies.

    void increment(MetricId id) noexcept { add_at(id, 0, 1); }
    void increment(MetricId id, LabelIndex a) noexcept { add_at(id, series_of(id, a), 1); }
    void increment(MetricId id, LabelIndex a, LabelIndex b) noexcept {
        add_at(id, series_of(id, a, b), 1);
    }
    void increment(MetricId id, LabelIndex a, LabelIndex b, LabelIndex c) noexcept {
        add_at(id, series_of(id, a, b, c), 1);
    }

    void add(MetricId id, std::uint64_t delta) noexcept { add_at(id, 0, delta); }
    void add(MetricId id, LabelIndex a, std::uint64_t delta) noexcept {
        add_at(id, series_of(id, a), delta);
    }
    void add(MetricId id, LabelIndex a, LabelIndex b, std::uint64_t delta) noexcept {
        add_at(id, series_of(id, a, b), delta);
    }

    // A gauge is SET, never added to. See the header comment.
    void set(MetricId id, std::uint64_t value) noexcept { set_at(id, 0, value); }
    void set(MetricId id, LabelIndex a, std::uint64_t value) noexcept {
        set_at(id, series_of(id, a), value);
    }

    // THREE ADDS, NOT TWELVE: the one bucket the value falls in, plus _sum and
    // _count. Incrementing every bucket at or above the value is the obvious
    // implementation and it is the wrong one — twelve atomic writes across twelve
    // cache lines on the hot path, to save an addition on a path that runs once
    // per scrape. The cumulative form OpenMetrics wants is computed at snapshot,
    // over cells that are already being read (docs/17-analytics.md §5).
    void observe(MetricId id, std::int64_t value) noexcept { observe_at(id, 0, value); }
    void observe(MetricId id, LabelIndex a, std::int64_t value) noexcept {
        observe_at(id, series_of(id, a), value);
    }

    // --- read paths ---------------------------------------------------------

    [[nodiscard]] std::span<const MetricSpec> internal_table() const noexcept {
        return internal_;
    }
    [[nodiscard]] std::span<const MetricSpec> application_table() const noexcept {
        return application_;
    }
    [[nodiscard]] std::size_t metric_count() const noexcept { return metric_count_; }
    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_; }
    [[nodiscard]] std::size_t cell_capacity() const noexcept { return cell_capacity_; }
    [[nodiscard]] std::size_t arena_bytes() const noexcept { return arena_bytes_; }

    // The spec behind an id, or nullptr when the id names no metric in either
    // table. A real state during a rolling deploy of a library consumer.
    [[nodiscard]] const MetricSpec* spec(MetricId id) const noexcept;

    // One series' slot, summed across shards for a counter or histogram and read
    // directly for a gauge. Summing on READ is what keeps the write path to a
    // single uncontended add and what lets a pull scrape need no periodic task
    // at all.
    [[nodiscard]] std::uint64_t value_at(MetricId id, std::size_t series,
                                         std::size_t slot) const noexcept;

    // Zeroes every cell. For a test that needs a registry it did not construct;
    // never called on a running process, where a reset would make a counter
    // non-monotonic and every rate computed across the reset wrong.
    void reset() noexcept;

private:
    // Per-metric layout, computed once at construction from the two tables. Held
    // INSIDE the single arena allocation, ahead of the cells.
    struct Layout final {
        std::uint32_t cell_base;        // first Cell of this metric in the arena
        std::uint32_t series;           // distinct label-value combinations
        std::array<std::uint32_t, kMaxLabels> stride;
        std::uint16_t slots;            // cells per series
        std::uint16_t buckets;          // histogram boundaries; 0 otherwise
        std::uint8_t  shards;           // 1 for a gauge, shards_ otherwise
        std::uint8_t  labels;
        MetricKind    kind;
    };

    [[nodiscard]] const Layout* layout_of(MetricId id) const noexcept {
        return id.index < metric_count_ ? &layouts_[id.index] : nullptr;
    }

    // kNoSeries when any index is outside its declared value space. Checked
    // rather than trusted: an index is the one thing between the public API and
    // a write past the end of the arena.
    static constexpr std::uint32_t kNoSeries = 0xFFFFFFFFU;

    [[nodiscard]] std::uint32_t series_of(MetricId id, LabelIndex a) const noexcept;
    [[nodiscard]] std::uint32_t series_of(MetricId id, LabelIndex a,
                                          LabelIndex b) const noexcept;
    [[nodiscard]] std::uint32_t series_of(MetricId id, LabelIndex a, LabelIndex b,
                                          LabelIndex c) const noexcept;

    void add_at(MetricId id, std::uint32_t series, std::uint64_t delta) noexcept;
    void set_at(MetricId id, std::uint32_t series, std::uint64_t value) noexcept;
    void observe_at(MetricId id, std::uint32_t series, std::int64_t value) noexcept;

    // Declaration order is construction order: the arena is sized from the
    // tables, and everything below it points into the arena.
    std::span<const MetricSpec> internal_;
    std::span<const MetricSpec> application_;
    // ONE allocation, over-aligned to a cache line and holding the layouts and
    // then the cells. Two allocations would be two chances for a cell array to
    // outlive or be outlived by the description of what is in it.
    void*         arena_;
    Layout*       layouts_;
    Cell*         cells_;
    std::size_t   arena_bytes_;
    std::size_t   metric_count_;
    std::size_t   cell_capacity_;
    std::size_t   shards_;
    std::uint32_t shard_mask_;
};

// --- the process-wide registry ----------------------------------------------
//
// anvil increments counters from inside every layer, so the cell array cannot
// live above the layer that increments it (docs/00-architecture.md §2) and a
// dependency-injected registry would have to be threaded through every function
// in the library. It is installed once, from main(), beside the thread pools.
//
// NULL UNTIL INSTALLED, and an increment before then is a NO-OP rather than a
// crash. A library whose counter call kills an application that has not called
// install_registry is a library with a worse failure mode than the missing
// number it was reporting.

void install_registry(std::shared_ptr<Registry> registry) noexcept;
void uninstall_registry() noexcept;

// Relaxed, because the pointer is written once at boot and read on every
// increment: an acquire here would be a fence on the hottest path in the
// library, ordering nothing a relaxed load does not already order for a pointer
// whose target is fully constructed before it is published.
[[nodiscard]] Registry* registry() noexcept;

namespace detail {

[[nodiscard]] std::uint32_t next_thread_shard() noexcept;

// Assigned at FIRST TOUCH and never again. The shard belongs to the thread, so
// two threads incrementing the same series never write the same cache line —
// and a caller cannot choose one, which is what would reintroduce the sharing
// this exists to remove.
[[nodiscard]] inline std::uint32_t thread_shard() noexcept {
    static thread_local const std::uint32_t assigned = next_thread_shard();
    return assigned;
}

}  // namespace detail

// --- anvil's own increment sites --------------------------------------------
//
// Free functions rather than `registry()->increment(...)` at ninety call sites:
// the null check belongs in one place, and an instrumentation call that reads as
// one line is an instrumentation call that survives a refactor.

inline void count(Internal metric) noexcept {
    if (Registry* reg = registry(); reg != nullptr) { reg->increment(metric_of(metric)); }
}

inline void count(Internal metric, LabelIndex a) noexcept {
    if (Registry* reg = registry(); reg != nullptr) { reg->increment(metric_of(metric), a); }
}

inline void count(Internal metric, LabelIndex a, LabelIndex b) noexcept {
    if (Registry* reg = registry(); reg != nullptr) {
        reg->increment(metric_of(metric), a, b);
    }
}

inline void count_many(Internal metric, LabelIndex a, std::uint64_t delta) noexcept {
    if (Registry* reg = registry(); reg != nullptr) { reg->add(metric_of(metric), a, delta); }
}

inline void count_many(Internal metric, LabelIndex a, LabelIndex b,
                       std::uint64_t delta) noexcept {
    if (Registry* reg = registry(); reg != nullptr) {
        reg->add(metric_of(metric), a, b, delta);
    }
}

inline void sample(Internal metric, LabelIndex a, std::uint64_t value) noexcept {
    if (Registry* reg = registry(); reg != nullptr) { reg->set(metric_of(metric), a, value); }
}

inline void sample(Internal metric, std::uint64_t value) noexcept {
    if (Registry* reg = registry(); reg != nullptr) { reg->set(metric_of(metric), value); }
}

inline void observe(Internal metric, std::int64_t value) noexcept {
    if (Registry* reg = registry(); reg != nullptr) {
        reg->observe(metric_of(metric), value);
    }
}

}  // namespace anvil::analytics
