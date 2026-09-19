#include "anvil/analytics/counters.h"

#include <atomic>
#include <bit>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace anvil::analytics {
namespace {

// Published once at boot and read on every increment. A raw atomic pointer
// beside the shared_ptr that owns the object: a shared_ptr load on the hot path
// is an atomic increment AND a later atomic decrement, both contended cache-line
// writes, which is precisely the cost this subsystem exists not to pay.
std::atomic<Registry*>    g_registry{nullptr};
std::shared_ptr<Registry> g_owned;

std::atomic<std::uint32_t> g_next_shard{0};

[[nodiscard]] std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Hardware concurrency rounded UP to a power of two and capped, so the shard
// index is a mask rather than a modulo — a division on the increment path would
// cost more than the add it guards.
[[nodiscard]] std::size_t choose_shards() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    const std::size_t wanted = hardware == 0 ? 4U : hardware;
    std::size_t shards = std::bit_ceil(wanted);
    if (shards > Registry::kMaxShards) { shards = Registry::kMaxShards; }
    if (shards == 0) { shards = 1; }
    return shards;
}

}  // namespace

std::uint32_t detail::next_thread_shard() noexcept {
    return g_next_shard.fetch_add(1, std::memory_order_relaxed);
}

Registry::Registry(std::span<const MetricSpec> internal, std::span<const MetricSpec> application)
    : internal_{internal},
      application_{application},
      arena_{nullptr},
      layouts_{nullptr},
      cells_{nullptr},
      arena_bytes_{0},
      metric_count_{internal.size() + application.size()},
      cell_capacity_{0},
      shards_{choose_shards()},
      // Declared after shards_, so this reads a member that is already built.
      // A power of two, so the shard index is a mask rather than a division on
      // the increment path.
      shard_mask_{static_cast<std::uint32_t>(shards_ - 1)} {
    // Defence in depth behind the static_assert the seam already carries. The
    // ceiling is a COMPILE-TIME bound (anvil/analytics/metric_spec.h) and this is
    // the reading of it that a table smuggled in through a span cannot dodge.
    if (table_cell_count(internal) + table_cell_count(application) > kMaxCells) {
        throw std::length_error("analytics: the two metric tables exceed kMaxCells");
    }

    const std::size_t layout_bytes =
        align_up(sizeof(Layout) * metric_count_, kCacheLineBytes);

    // Sized from the two tables, once, and never grown. There is no rehash and
    // no reallocation anywhere in this class: a registry that could move its
    // cells is a registry that could move them out from under a concurrent
    // fetch_add.
    std::size_t physical_cells = 0;
    for (std::size_t i = 0; i < metric_count_; ++i) {
        const MetricSpec& spec = i < internal_.size() ? internal_[i]
                                                      : application_[i - internal_.size()];
        const std::size_t shards = spec.kind == MetricKind::Gauge ? 1 : shards_;
        physical_cells += cell_count(spec) * shards;
    }

    arena_bytes_ = layout_bytes + (physical_cells * kCacheLineBytes);
    cell_capacity_ = physical_cells;

    arena_ = ::operator new(arena_bytes_, std::align_val_t{kCacheLineBytes});
    layouts_ = static_cast<Layout*>(arena_);
    cells_ = reinterpret_cast<Cell*>(static_cast<std::byte*>(arena_) + layout_bytes);

    for (std::size_t i = 0; i < metric_count_; ++i) { new (&layouts_[i]) Layout{}; }
    for (std::size_t i = 0; i < physical_cells; ++i) { new (&cells_[i]) Cell{}; }

    std::uint32_t next_cell = 0;
    for (std::size_t i = 0; i < metric_count_; ++i) {
        const MetricSpec& spec = i < internal_.size() ? internal_[i]
                                                      : application_[i - internal_.size()];
        Layout& layout = layouts_[i];
        layout.kind = spec.kind;
        layout.labels = static_cast<std::uint8_t>(spec.labels.size());
        layout.slots = static_cast<std::uint16_t>(cells_per_series(spec));
        layout.buckets = static_cast<std::uint16_t>(spec.buckets.size());
        layout.series = static_cast<std::uint32_t>(series_count(spec));
        layout.shards = static_cast<std::uint8_t>(spec.kind == MetricKind::Gauge ? 1 : shards_);
        layout.cell_base = next_cell;

        // stride[last] is 1 and each earlier one multiplies by the value spaces
        // to its right, so the series index is a dot product the compiler folds
        // into a multiply-add per label.
        std::uint32_t running = 1;
        for (std::size_t label = spec.labels.size(); label-- > 0;) {
            layout.stride[label] = running;
            running *= static_cast<std::uint32_t>(spec.labels[label].values.size());
        }

        next_cell += layout.series * layout.slots * layout.shards;
    }
}

Registry::~Registry() {
    // Cell and Layout are trivially destructible, so there is nothing to run —
    // the arena is one buffer and freeing it is the whole of the teardown.
    ::operator delete(arena_, std::align_val_t{kCacheLineBytes});
}

const MetricSpec* Registry::spec(MetricId id) const noexcept {
    if (id.index < internal_.size()) { return &internal_[id.index]; }
    const std::size_t offset = id.index - internal_.size();
    if (offset < application_.size()) { return &application_[offset]; }
    return nullptr;
}

std::uint32_t Registry::series_of(MetricId id, LabelIndex a) const noexcept {
    const Layout* layout = layout_of(id);
    if (layout == nullptr || layout->labels != 1) { return kNoSeries; }
    const std::uint32_t series = static_cast<std::uint32_t>(a.value) * layout->stride[0];
    return series < layout->series ? series : kNoSeries;
}

std::uint32_t Registry::series_of(MetricId id, LabelIndex a, LabelIndex b) const noexcept {
    const Layout* layout = layout_of(id);
    if (layout == nullptr || layout->labels != 2) { return kNoSeries; }
    const std::uint32_t series = (static_cast<std::uint32_t>(a.value) * layout->stride[0]) +
                                 (static_cast<std::uint32_t>(b.value) * layout->stride[1]);
    // One bound check covers both indices: an out-of-range b lands inside a's
    // block and would be caught only by the per-label check, so both are
    // verified rather than the product alone.
    if (b.value >= layout->stride[0]) { return kNoSeries; }
    return series < layout->series ? series : kNoSeries;
}

std::uint32_t Registry::series_of(MetricId id, LabelIndex a, LabelIndex b,
                                  LabelIndex c) const noexcept {
    const Layout* layout = layout_of(id);
    if (layout == nullptr || layout->labels != 3) { return kNoSeries; }
    if (c.value >= layout->stride[1]) { return kNoSeries; }
    if (b.value >= layout->stride[0] / layout->stride[1]) { return kNoSeries; }
    const std::uint32_t series = (static_cast<std::uint32_t>(a.value) * layout->stride[0]) +
                                 (static_cast<std::uint32_t>(b.value) * layout->stride[1]) +
                                 (static_cast<std::uint32_t>(c.value) * layout->stride[2]);
    return series < layout->series ? series : kNoSeries;
}

void Registry::add_at(MetricId id, std::uint32_t series, std::uint64_t delta) noexcept {
    const Layout* layout = layout_of(id);
    if (layout == nullptr || series == kNoSeries || series >= layout->series) { return; }
    // A gauge is not a counter and adding to one is the confusion this whole
    // file is written against, so it is refused rather than silently accepted.
    if (layout->kind != MetricKind::Counter) { return; }

    const std::uint32_t shard = detail::thread_shard() & shard_mask_;
    const std::size_t index =
        layout->cell_base + (static_cast<std::size_t>(series) * layout->shards) + shard;
    cells_[index].value.fetch_add(delta, std::memory_order_relaxed);
}

void Registry::set_at(MetricId id, std::uint32_t series, std::uint64_t value) noexcept {
    const Layout* layout = layout_of(id);
    if (layout == nullptr || series == kNoSeries || series >= layout->series) { return; }
    if (layout->kind != MetricKind::Gauge) { return; }
    // Shard 0, always: a gauge has exactly one cell, because a summed gauge
    // reports up to kMaxShards times the value that exists.
    cells_[layout->cell_base + series].value.store(value, std::memory_order_relaxed);
}

void Registry::observe_at(MetricId id, std::uint32_t series, std::int64_t value) noexcept {
    const Layout* layout = layout_of(id);
    if (layout == nullptr || series == kNoSeries || series >= layout->series) { return; }
    if (layout->kind != MetricKind::Histogram) { return; }
    const MetricSpec* metric = spec(id);
    if (metric == nullptr) { return; }

    // A negative observation is a bug at the call site — every declared unit is
    // a duration, a size or a count — so it is charged to the first bucket and
    // contributes nothing to the sum rather than wrapping the unsigned cell,
    // which would turn one mistake into a number no operator could interpret.
    const std::uint64_t magnitude = value < 0 ? 0U : static_cast<std::uint64_t>(value);

    // A linear scan of at most twelve int64 in one or two cache lines. It beats
    // a binary search at this size and has no branch misprediction worth naming.
    std::size_t slot = layout->buckets;
    for (std::size_t i = 0; i < layout->buckets; ++i) {
        if (value <= metric->buckets[i]) {
            slot = i;
            break;
        }
    }

    const std::uint32_t shard = detail::thread_shard() & shard_mask_;
    const std::size_t series_base =
        layout->cell_base +
        (static_cast<std::size_t>(series) * layout->slots * layout->shards);
    const auto cell_of = [&](std::size_t which) {
        return series_base + (which * layout->shards) + shard;
    };

    cells_[cell_of(slot)].value.fetch_add(1, std::memory_order_relaxed);
    cells_[cell_of(layout->buckets + 1U)].value.fetch_add(magnitude,
                                                          std::memory_order_relaxed);
    cells_[cell_of(layout->buckets + 2U)].value.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Registry::value_at(MetricId id, std::size_t series,
                                 std::size_t slot) const noexcept {
    const Layout* layout = layout_of(id);
    if (layout == nullptr || series >= layout->series || slot >= layout->slots) { return 0; }
    const std::size_t base = layout->cell_base +
                             (((series * layout->slots) + slot) * layout->shards);
    std::uint64_t total = 0;
    for (std::size_t shard = 0; shard < layout->shards; ++shard) {
        total += cells_[base + shard].value.load(std::memory_order_relaxed);
    }
    return total;
}

void Registry::reset() noexcept {
    for (std::size_t i = 0; i < cell_capacity_; ++i) {
        cells_[i].value.store(0, std::memory_order_relaxed);
    }
}

void install_registry(std::shared_ptr<Registry> reg) noexcept {
    g_owned = std::move(reg);
    g_registry.store(g_owned.get(), std::memory_order_release);
}

void uninstall_registry() noexcept {
    g_registry.store(nullptr, std::memory_order_release);
    g_owned.reset();
}

Registry* registry() noexcept { return g_registry.load(std::memory_order_relaxed); }

}  // namespace anvil::analytics
