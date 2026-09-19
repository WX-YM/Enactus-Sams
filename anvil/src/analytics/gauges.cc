#include "anvil/analytics/gauges.h"

#include <cstdint>
#include <string>

#include <mongocxx/exception/exception.hpp>

#include "anvil/analytics/counters.h"
#include "anvil/analytics/internal_metrics.h"
#include "anvil/analytics/snapshot.h"
#include "anvil/core/thread_pools.h"

namespace anvil::analytics {

void install_pool_gauge_sampler() {
    install_gauge_sampler([](Registry& registry) {
        // Guarded, because a scrape may outlive Pools::init's caller in a test
        // and a sampler that throws would take the scrape with it. There is no
        // number to report when there are no pools, and reporting zero would be
        // a lie an operator would act on.
        if (!Pools::ready()) { return; }
        const MetricId depth = metric_of(Internal::PoolQueueDepth);
        registry.set(depth, PoolLabel::Db, Pools::db().queue_depth());
        registry.set(depth, PoolLabel::Cpu, Pools::cpu().queue_depth());
        registry.set(depth, PoolLabel::Hash, Pools::hash().queue_depth());
        registry.set(depth, PoolLabel::Audit, Pools::audit().queue_depth());
        registry.set(depth, PoolLabel::Analytics, Pools::analytics().queue_depth());
    });
}

void sample_ttl_collection_rows(mongocxx::client& client,
                                const db::DatabaseNames& databases) {
    Registry* reg = registry();
    if (reg == nullptr) { return; }
    const MetricId rows = metric_of(Internal::TtlCollectionRows);

    for (std::size_t i = 0; i < kTtlCollectionValues.size(); ++i) {
        const std::string_view name = kTtlCollectionValues[i];
        try {
            const std::string database{databases.for_collection(name)};
            const std::int64_t resident =
                client[database][std::string{name}].estimated_document_count();
            reg->set(rows, LabelIndex{static_cast<std::uint16_t>(i)},
                     resident < 0 ? 0U : static_cast<std::uint64_t>(resident));
        } catch (const mongocxx::exception&) {
            // Left at its previous reading rather than zeroed. A collection that
            // could not be counted has not become empty, and a gauge that drops
            // to zero on a transient driver error is an alert nobody can trust.
        }
    }
}

}  // namespace anvil::analytics
