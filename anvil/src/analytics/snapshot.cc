#include "anvil/analytics/snapshot.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace anvil::analytics {
namespace {

// Immutable shared state swapped atomically rather than a mutex-guarded vector:
// readers are on a request path and then need no lock at all, and the writer
// runs once at boot (ENGINEERING_RULES.md §4).
using SamplerList = std::shared_ptr<const std::vector<GaugeSampler>>;

std::atomic<SamplerList> g_samplers;
std::mutex               g_sampler_mutex;

[[nodiscard]] SamplerList samplers() noexcept {
    return g_samplers.load(std::memory_order_acquire);
}

}  // namespace

void install_gauge_sampler(GaugeSampler sampler) {
    const std::lock_guard<std::mutex> held{g_sampler_mutex};
    auto next = std::make_shared<std::vector<GaugeSampler>>();
    if (const SamplerList current = samplers(); current) { *next = *current; }
    next->push_back(std::move(sampler));
    g_samplers.store(SamplerList{std::move(next)}, std::memory_order_release);
}

void clear_gauge_samplers() noexcept {
    const std::lock_guard<std::mutex> held{g_sampler_mutex};
    g_samplers.store(SamplerList{}, std::memory_order_release);
}

Snapshot::Snapshot(Registry& reg)
    : registry_{&reg},
      values_(snapshot_value_count(reg.internal_table(), reg.application_table()), 0) {}

std::size_t Snapshot::metric_count() const noexcept { return registry_->metric_count(); }

const MetricSpec& Snapshot::spec_at(std::size_t index) const noexcept {
    const std::span<const MetricSpec> internal = registry_->internal_table();
    return index < internal.size() ? internal[index]
                                   : registry_->application_table()[index - internal.size()];
}

void Snapshot::collect() {
    if (const auto installed = samplers(); installed) {
        // Sampled BEFORE the read, so every gauge in this scrape describes the
        // same instant as every counter in it. A sampler that ran after would
        // report a depth the emitted bytes do not contain.
        for (const GaugeSampler& sampler : *installed) {
            if (sampler) { sampler(*registry_); }
        }
    }

    std::size_t next = 0;
    for (std::size_t metric = 0; metric < metric_count(); ++metric) {
        const MetricSpec& spec = spec_at(metric);
        const std::size_t series = series_count(spec);
        const std::size_t slots = cells_per_series(spec);
        const MetricId id{static_cast<std::uint16_t>(metric)};
        for (std::size_t s = 0; s < series; ++s) {
            for (std::size_t slot = 0; slot < slots; ++slot) {
                values_[next++] = registry_->value_at(id, s, slot);
            }
        }
    }
}

std::size_t Snapshot::offset_of(std::size_t metric, std::size_t series,
                                std::size_t slot) const noexcept {
    std::size_t offset = 0;
    for (std::size_t i = 0; i < metric && i < metric_count(); ++i) {
        offset += cell_count(spec_at(i));
    }
    if (metric >= metric_count()) { return values_.size(); }
    const MetricSpec& spec = spec_at(metric);
    return offset + (series * cells_per_series(spec)) + slot;
}

std::uint64_t Snapshot::value_of(std::size_t metric, std::size_t series,
                                 std::size_t slot) const noexcept {
    const std::size_t offset = offset_of(metric, series, slot);
    return offset < values_.size() ? values_[offset] : 0;
}

}  // namespace anvil::analytics
