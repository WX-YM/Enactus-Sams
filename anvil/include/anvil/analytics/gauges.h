#pragma once

// The two internal gauges that cannot sample themselves.
//
// A gauge is a SAMPLED CURRENT VALUE, and the natural moment to sample one is
// immediately before a scrape reads the cells — which is what
// install_gauge_sampler exists for (anvil/analytics/snapshot.h). Two of anvil's
// own gauges do not fit that shape for opposite reasons, and both are here.
//
//   anvil_pool_queue_depth   fits it exactly, and is installed by Pools::init
//                            so that a deployment cannot have bounded queues and
//                            no view of how full they are. The declaration is
//                            here rather than in core/thread_pools.h because
//                            that header is included by everything and this one
//                            is not.
//
//   anvil_ttl_collection_rows does NOT fit it. Reading it costs a round trip per
//                            collection, and a sampler that queries the database
//                            turns every scrape into a database load test that
//                            fires every fifteen seconds. It is sampled from a
//                            RECURRING JOB instead, on db_pool, at whatever
//                            interval the application declares — and the number
//                            it reports is a count of resident rows including
//                            the ones the TTL monitor has not reached yet, which
//                            is precisely the signal. A count that filtered out
//                            expired rows would report a stalled monitor as
//                            healthy (docs/09-mongodb.md §6).

#include <mongocxx/client.hpp>

#include "anvil/db/collections.h"

namespace anvil::analytics {

// Installed once, from Pools::init. Idempotent only in the sense that Pools::init
// is: a second call would add a second identical sampler.
void install_pool_gauge_sampler();

// Blocking: one estimated count per lifetime-bounded collection. Call it from a
// job handler on db_pool, never from a scrape and never from a loop thread.
//
// estimated_document_count reads collection metadata rather than counting, so
// this is O(1) per collection rather than a scan of the highest-churn
// collections in the system — which is the only way a gauge over them can be
// cheap enough to sample at all.
void sample_ttl_collection_rows(mongocxx::client& client, const db::DatabaseNames& databases);

}  // namespace anvil::analytics
