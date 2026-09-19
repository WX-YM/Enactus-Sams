#pragma once

// Removing one person's rows.
//
// ONE delete_many on `subject`, riding a PARTIAL index that anonymous rows never
// enter. The index therefore costs nothing for the rows that are the
// overwhelming majority, and the erasure stays a point query rather than a scan
// of the largest collection in the system (docs/17-analytics.md §12).
//
// It does NOT filter on the expiry field, which is the one read in this
// subsystem where that is correct: erasure must reach rows the TTL monitor has
// not collected yet, because "still physically present" is exactly what is being
// asked about.
//
// ROLLUPS ARE NOT ERASED, and that is a trade made explicitly rather than an
// oversight. A rollup carries no subject — it is a count of signups per day —
// and rebuilding history after every erasure request is a cost with no
// beneficiary.

#include <cstdint>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/analytics/repository.h"
#include "anvil/core/result.h"
#include "anvil/core/types.h"

namespace anvil::analytics {

class AnalyticsErasure final {
public:
    AnalyticsErasure(const db::DatabaseNames& databases,
                     const AnalyticsCollections& collections)
        : events_{std::string{databases.for_collection(collections.events)},
                  collections.events} {}

    // Rows removed. Blocking: it belongs on db_pool, never on a loop thread.
    [[nodiscard]] Result<std::int64_t> erase(mongocxx::client& client,
                                             const Uuid& subject) const {
        return events_.erase_subject(client, subject);
    }

private:
    EventRepository events_;
};

}  // namespace anvil::analytics
