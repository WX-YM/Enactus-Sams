#include "anvil/audit/service.h"

#include <exception>
#include <utility>

#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include "anvil/analytics/counters.h"
#include "anvil/core/thread_pools.h"
#include "anvil/db/mongo_pool.h"

namespace anvil::audit {

AuditService::AuditService(std::string database, std::string_view collection,
                           std::span<const AuditActionSpec> actions, AuditAction denial_action,
                           std::size_t capacity_rows)
    : database_{std::move(database)},
      actions_{actions},
      denial_action_{denial_action},
      log_{database_, collection},
      mutex_{},
      buffered_{actions_, capacity_rows} {}

AuditService::~AuditService() { stop(); }

Status AuditService::write(mongocxx::client& client, const AuditEntry& entry) const {
    return log_.append(client, entry, db::now_ms());
}

Result<AuditPage> AuditService::read(mongocxx::client& client, const AuditQuery& query) const {
    return log_.listing(client, query);
}

std::uint64_t AuditService::dropped_traffic() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return buffered_.dropped_traffic() + refused_traffic_;
}

std::uint64_t AuditService::dropped_changes() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return buffered_.dropped_changes() + refused_changes_;
}

std::uint64_t AuditService::refused_flushes() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return refused_flushes_;
}

std::uint64_t AuditService::deferred_flushes() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return deferred_flushes_;
}

std::size_t AuditService::buffered() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return buffered_.size();
}

void AuditService::start() {
    // A timer rather than size alone. Size alone starves the tail: on a quiet
    // deployment the last few rows of the day would sit in memory until the next
    // request, and a row that exists only in one process's memory is a row a
    // crash erases.
    //
    // It is also the coalescer's window. A fold that outlived this interval
    // would be a repeat added to a row already on disk, so the interval bounds
    // how stale a COUNT can be as well as how stale a row can be.
    timer_id_ = static_cast<std::int64_t>(drogon::app().getLoop()->runEvery(
        std::chrono::duration<double>{kFlushInterval}.count(), [this]() {
            std::vector<AuditRow> batch;
            {
                const std::lock_guard<std::mutex> held{mutex_};
                // Deliberately NOT gated on the deferral latch. This runs at
                // 1 Hz, so its cost is nothing beside a per-request path, and it
                // is what retries when no new row arrives to retry for — a sink
                // that only tried again on the next audited event would sit on a
                // full buffer through a quiet minute after the pool drained.
                if (flush_locked(batch) == 0) { return; }
            }
            post(std::move(batch));
        }));
}

void AuditService::stop() noexcept {
    std::vector<AuditRow> batch;
    std::int64_t  installed_timer = 0;
    std::uint64_t lost_traffic = 0;
    std::uint64_t lost_changes = 0;
    {
        const std::lock_guard<std::mutex> held{mutex_};
        if (stopped_) { return; }
        stopped_ = true;
        installed_timer = timer_id_;
        timer_id_ = 0;
        // drain() takes the coalescer's open window with it. That window is the
        // NEWEST row in the process, so a stop that flushed only the closed rows
        // would discard the one row nothing else has a copy of.
        buffered_.drain(batch);
        lost_traffic = buffered_.dropped_traffic() + refused_traffic_;
        lost_changes = buffered_.dropped_changes() + refused_changes_;
    }

    // The flush timer holds `this`, and nothing in this class keeps the service
    // alive for it — the same defect as a pool task capturing `this`, with a
    // different owner. A timer left installed by a destroyed service fires on
    // freed memory every second for the life of the loop.
    //
    // Taken out BEFORE the final flush so a tick cannot arrive during it and
    // drain the buffer this is about to write.
    if (installed_timer != 0) {
        drogon::app().getLoop()->invalidateTimer(
            static_cast<trantor::TimerId>(installed_timer));
    }

    if (!batch.empty()) {
        // Written HERE, on the calling thread, not posted. stop() runs during
        // shutdown, and a task posted to a pool that is about to drain is a task
        // that may never run — which would discard the very rows this exists to
        // save. Blocking is acceptable exactly once, on the way out.
        try {
            auto client = db::MongoPool::instance().acquire();
            if (const Status written = log_.append_many(*client, batch); !written) {
                LOG_ERROR << "audit: final flush of " << batch.size() << " row(s) failed";
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "audit: final flush of " << batch.size()
                      << " row(s) threw: " << e.what();
        }
    }

    if (lost_changes > 0) {
        // Said first and said separately. Every one of these is the only record
        // that somebody did something, and losing one means the buffer held
        // nothing compressible at all — a database outage, not a load problem.
        LOG_ERROR << "audit: " << lost_changes
                  << " row(s) dropped over this process's lifetime that were CHANGES — the "
                     "only record of what somebody did";
    }
    if (lost_traffic > 0) {
        LOG_ERROR << "audit: " << lost_traffic
                  << " traffic row(s) dropped over this process's lifetime";
    }
}

std::size_t AuditService::flush_locked(std::vector<AuditRow>& out) {
    if (buffered_.size() == 0) { return 0; }
    buffered_.drain(out);

    // Reported HERE rather than at the drop site: one line per flush at most,
    // instead of one per lost row. A million ERROR lines is not a louder alert
    // than one carrying the number, it is a second outage
    // (docs/00-architecture.md §9).
    const std::uint64_t lost = buffered_.dropped_traffic() + buffered_.dropped_changes() +
                               refused_traffic_ + refused_changes_;
    if (lost > reported_drops_) {
        LOG_ERROR << "audit: " << (lost - reported_drops_)
                  << " row(s) dropped, buffer full — the database is not keeping up and "
                     "forensic signal is being lost";
        reported_drops_ = lost;
    }
    return out.size();
}

void AuditService::post(std::vector<AuditRow> batch) noexcept {
    const std::size_t rows = batch.size();

    // EVERYTHING the task needs, by value. Not `this`: a task audit_pool has
    // accepted runs after post() returns, and nothing in this class keeps the
    // service alive until then. It happens to survive today because an
    // AuditService is a main()-level object that outlives Pools::shutdown() —
    // but that is an ordering rule living in an application's main(), not a
    // property of this class, and a test that builds a service on the heap
    // breaks it. The load gate found the identical shape in EventSink::post as
    // a stack-use-after-return on its first run.
    //
    // The batch is COPIED and not moved, because the refusal path below is what
    // needs it: try_post takes the task by value and destroys it when it refuses,
    // so a moved batch is already gone by the time there is anything to do about
    // it. The copy is bounded by kBatchRows of a trivially copyable row — about
    // 16 KB once per flush — which is the price of not discarding them.
    //
    // The repository copy is a database name and a collection name.
    auto task = [log = log_, batch]() {
        auto client = db::MongoPool::instance().acquire();
        if (const Status written = log.append_many(*client, batch); !written) {
            // Logged rather than propagated. There is no caller left to fail:
            // the responses these rows describe have already been sent.
            LOG_ERROR << "audit: batch of " << batch.size() << " row(s) failed to write";
        }
    };

    // audit_pool, NOT db_pool. A batch posted to the queue every request uses is
    // refused precisely when that queue is full, which is precisely the flood
    // these rows describe. This pool is small, has its own bound, and no request
    // path can spend it (docs/00-architecture.md §3).
    if (Pools::audit().try_post(anvil::guarded("audit", std::move(task)))) { return; }

    // RE-ADMITTED, not discarded. This used to count the whole batch as lost and
    // drop it, on the reasoning that a refusal means the database has been
    // unreachable for minutes rather than that the server is busy. A load run
    // against a live, busy mongod refused 8,485 batches, which is exactly the
    // case that reasoning excludes — so the classify-and-shed policy governed
    // admission to the buffer and then threw away the rows it had protected.
    //
    // The buffer decides what comes back, because the buffer is where the
    // classification lives: changes return, traffic is charged as the drop at
    // the FLUSH stage, and the bound it already has is what makes returning a
    // batch to it safe.
    std::size_t returned = 0;
    {
        const std::lock_guard<std::mutex> held{mutex_};
        ++refused_flushes_;
        // Armed here and taken down by the first attempt that finds the pool
        // with room. Without it, a buffer holding kBatchRows of re-admitted
        // CHANGES makes the next row repeat this whole function — the drain, the
        // copy, the refusal, the re-admission and this log line — once per row,
        // for as long as the queue stays full. See the header comment.
        flush_deferred_ = true;
        returned = buffered_.readmit(batch);
    }
    // WARN, not ERROR: nothing is lost yet at this point. The rows that were
    // lost are counted in the buffer, and flush_locked reports them once per
    // flush with the number — one line per lost row is a second outage
    // (docs/00-architecture.md §9). The latch above is what keeps this line
    // to one per saturation episode plus one per timer tick, rather than one
    // per audited request, which is the same rule applied to this line itself.
    LOG_WARN << "audit: audit_pool refused a batch of " << rows << " row(s); " << returned
             << " change(s) re-admitted";
}

Status AuditService::flush_now(mongocxx::client& client) {
    std::vector<AuditRow> batch;
    {
        const std::lock_guard<std::mutex> held{mutex_};
        if (flush_locked(batch) == 0) { return ok(); }
    }
    return log_.append_many(client, batch);
}

void AuditService::write_async(const AuditEntry& entry) noexcept {
    std::vector<AuditRow> batch;
    {
        const std::lock_guard<std::mutex> held{mutex_};
        if (stopped_) {
            if (audit_class_of(actions_, entry.action) == AuditClass::Traffic) {
                ++refused_traffic_;
                analytics::count(analytics::Internal::AuditRowsDropped,
                                 analytics::AuditDropClass::Traffic,
                                 analytics::AuditDropStage::Flush);
            } else {
                ++refused_changes_;
                analytics::count(analytics::Internal::AuditRowsDropped,
                                 analytics::AuditDropClass::Change,
                                 analytics::AuditDropStage::Flush);
            }
            return;
        }
        // Stamped NOW, not at flush time. The row's instant is when the event
        // happened; the flush is an implementation detail of getting it stored.
        // A fold keeps the FIRST such instant, which is where the burst began.
        (void)buffered_.offer(entry, db::now_ms());
        if (buffered_.size() < kBatchRows) { return; }
        // The pool refused the last batch. Reachable only after a refusal, which
        // is itself only reachable once Pools::init has run, so Pools::audit()
        // here cannot be the throw that a noexcept function must not make.
        if (flush_deferred_) {
            if (Pools::audit().saturated()) {
                ++deferred_flushes_;
                return;
            }
            flush_deferred_ = false;
        }
        flush_locked(batch);
    }
    post(std::move(batch));
}

void AuditService::record(const accesscontrol::DenialRecord& denial) noexcept {
    write_async(AuditEntry{
        .actor = denial.actor,
        // The subject is the resource, and a denial happens BEFORE any resource
        // is identified. Leaving it absent is honest; the route is recovered
        // from the request log line, which carries a matching request id.
        .subject = std::nullopt,
        .from_state = std::nullopt,
        .to_state = std::nullopt,
        .ip = denial.ip,
        .action = denial_action_,
        // The TRUE code, even though the client saw a 404 on a stealth route.
        // This field is the reason stealth is an acceptable trade at all.
        .code = denial.code,
        .succeeded = false,
        // Carried so two denials the client experienced differently are not
        // folded into one row. Not stored (anvil/audit/record.h).
        .stealthed = denial.stealthed,
    });
}

}  // namespace anvil::audit
