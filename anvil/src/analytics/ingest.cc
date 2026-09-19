#include "anvil/analytics/ingest.h"

#include <exception>
#include <utility>

#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include "anvil/analytics/counters.h"
#include "anvil/core/thread_pools.h"
#include "anvil/db/mongo_pool.h"

namespace anvil::analytics {

std::size_t EventSink::VisitorHash::operator()(const VisitorId& id) const noexcept {
    std::size_t hashed = 0;
    for (std::size_t i = 0; i < sizeof(std::size_t) && i < id.size(); ++i) {
        hashed |= static_cast<std::size_t>(id[i]) << (i * 8);
    }
    return hashed;
}

EventSink::EventSink(const db::DatabaseNames& databases,
                     const AnalyticsCollections& collections,
                     std::span<const EventSpec> events, IngestConfig config)
    : config_{config},
      events_{events},
      // Each repository takes the database ITS OWN collection was declared in.
      events_repo_{std::string{databases.for_collection(collections.events)},
                   collections.events},
      sessions_repo_{std::string{databases.for_collection(collections.sessions)},
                     collections.sessions},
      mutex_{},
      buffered_{events_, config_.capacity_rows},
      pending_sessions_{} {}

EventSink::~EventSink() { stop(); }

EventSink::Outcome EventSink::offer(const Offer& offered) noexcept {
    // 1. CONSENT, at the door and before anything is derived. An unknown code is
    //    treated as requiring consent (anvil/analytics/event_spec.h), so a
    //    rolling deploy fails towards recording less.
    if (!offered.consented && event_requires_consent(events_, offered.code)) {
        return Outcome::RefusedConsent;
    }

    const db::TimeMs at = db::now_ms();
    const DayNumber day = day_of(at);
    const VisitorId session = visitor_id(offered.address, day);
    // 2. A VISITOR. All-zero means no pepper is installed, and an unkeyed digest
    //    of a 32-bit address space is reversible from a database dump — so this
    //    records nothing rather than recording something that looks fine.
    if (is_anonymous_visitor(session)) { return Outcome::NoVisitor; }

    std::vector<EventRow> batch;
    std::vector<VisitorId> sessions;
    {
        const std::lock_guard<std::mutex> held{mutex_};
        if (stopped_) { return Outcome::Stopped; }

        // 3. SAMPLING, and only above the high-water mark. Below it the sink is
        //    keeping up and there is nothing to trade away.
        if (buffered_.size() >= config_.high_water_rows &&
            !session_is_sampled_in(session, config_.sample_denominator)) {
            ++sampled_out_;
            return Outcome::SampledOut;
        }

        const Event event{offered.code, offered.dimensions, session, offered.subject};
        // 4. The buffer's classify-and-shed policy.
        const EventBuffer::Admission admission = buffered_.offer(event, at);
        if (admission == EventBuffer::Admission::Refused) { return Outcome::Dropped; }

        // Bounded by the buffer's own capacity, which is what keeps this set
        // from becoming the unbounded thing the buffer is not.
        if (pending_sessions_.size() < config_.capacity_rows) {
            pending_sessions_.insert(session);
        }

        if (buffered_.size() < kBatchRows) { return Outcome::Recorded; }
        // analytics_pool refused the last batch. Rebuilding the same 512-row
        // task per event to be refused again is the storm deferred_flushes()
        // describes; the rows wait in a buffer that already sheds. Reachable
        // only after a refusal, so Pools::analytics() here cannot be the throw
        // a noexcept function must not make.
        if (flush_deferred_) {
            if (Pools::analytics().saturated()) {
                ++deferred_flushes_;
                return Outcome::Recorded;
            }
            flush_deferred_ = false;
        }
        flush_locked(batch, sessions);
    }
    post(std::move(batch), std::move(sessions));
    return Outcome::Recorded;
}

std::size_t EventSink::flush_locked(std::vector<EventRow>& out,
                                    std::vector<VisitorId>& sessions) {
    if (buffered_.size() == 0 && pending_sessions_.empty()) { return 0; }
    buffered_.drain(out);
    sessions.reserve(pending_sessions_.size());
    for (const VisitorId& visitor : pending_sessions_) { sessions.push_back(visitor); }
    pending_sessions_.clear();
    return out.size();
}

Status EventSink::write_batch(mongocxx::client& client, const EventRepository& events,
                              const SessionRepository& sessions_repo,
                              std::chrono::seconds retention,
                              std::span<const EventRow> batch,
                              std::span<const VisitorId> sessions) {
    const db::TimeMs now = db::now_ms();
    // Sessions FIRST. A session row that exists without its events reports a
    // visitor who did nothing, which is harmless; events that exist without
    // their session make a "sessions per day" number smaller than the funnel
    // computed from the same window, which is a discrepancy nobody can explain.
    for (const VisitorId& visitor : sessions) {
        const Result<bool> created =
            sessions_repo.touch(client, visitor, day_of(now), now, retention);
        if (!created) { return created.error(); }
    }
    return events.append_many(client, batch, now, retention);
}

void EventSink::post(std::vector<EventRow> batch, std::vector<VisitorId> sessions) noexcept {
    if (batch.empty() && sessions.empty()) { return; }
    const std::size_t rows = batch.size();

    // EVERYTHING the task needs, by value. Not `this`: a task analytics_pool has
    // already accepted runs after post() returns, and nothing keeps this sink
    // alive until then.
    auto task = [events = events_repo_, sessions_repo = sessions_repo_,
                 retention = config_.retention, batch, sessions]() {
        auto client = db::MongoPool::instance().acquire();
        if (const Status written =
                write_batch(*client, events, sessions_repo, retention, batch, sessions);
            !written) {
            // Logged rather than propagated. There is no caller left to fail:
            // the requests these rows describe have already been answered.
            LOG_ERROR << "analytics: batch of " << batch.size() << " row(s) failed to write";
        }
    };

    // analytics_pool, NOT audit_pool. audit_pool exists so a saturated request
    // path cannot starve the record of what saturated it; a second writer on it
    // re-creates exactly that problem, and this is the writer that floods
    // (docs/17-analytics.md §10).
    if (Pools::analytics().try_post(anvil::guarded("analytics", std::move(task)))) { return; }

    // RE-ADMITTED, not discarded. This was the correction the audit sink's shape
    // needed rather than a copy of it: rows that the classify-and-shed policy
    // just protected would otherwise be lost wholesale, because the policy
    // governs admission to the buffer and stops at its boundary. A load run
    // measured 8,485 refusals against a live, busy mongod — the case the audit
    // sink's comment reasoned could not happen. That sink now does the same
    // thing this one does.
    std::size_t returned = 0;
    {
        const std::lock_guard<std::mutex> held{mutex_};
        ++refused_flushes_;
        // Armed here, taken down by the first attempt that finds the pool with
        // room. Without it this whole function repeats per event for as long as
        // the queue stays full — the drain, the copy, the refusal, the
        // re-admission and the log line below.
        flush_deferred_ = true;
        returned = buffered_.readmit(batch);
        // The session upserts go back too. They are idempotent by construction —
        // the write is $setOnInsert on a primary key — so re-admitting them
        // cannot double-count anything.
        for (const VisitorId& visitor : sessions) {
            if (pending_sessions_.size() >= config_.capacity_rows) { break; }
            pending_sessions_.insert(visitor);
        }
    }
    LOG_WARN << "analytics: analytics_pool refused a batch of " << rows << " row(s); "
             << returned << " conversion(s) re-admitted";
}

void EventSink::start() {
    // A timer rather than size alone. Size alone starves the tail: on a quiet
    // deployment the last few rows of the day would sit in memory until the next
    // request, and a row that exists only in one process's memory is a row a
    // crash erases. It is also the coalescer's window, so the interval bounds
    // how stale a COUNT can be as well as how stale a row can be.
    timer_id_ = static_cast<std::int64_t>(drogon::app().getLoop()->runEvery(
        std::chrono::duration<double>{kFlushInterval}.count(), [this]() {
            std::vector<EventRow> batch;
            std::vector<VisitorId> sessions;
            {
                const std::lock_guard<std::mutex> held{mutex_};
                // Deliberately NOT gated on the deferral latch: this runs at
                // 1 Hz, and it is what retries when no new event arrives to
                // retry for.
                if (flush_locked(batch, sessions) == 0 && sessions.empty()) { return; }
            }
            post(std::move(batch), std::move(sessions));
        }));
}

void EventSink::stop() noexcept {
    std::vector<EventRow> batch;
    std::vector<VisitorId> sessions;
    std::int64_t  installed_timer = 0;
    std::uint64_t lost_behaviour = 0;
    std::uint64_t lost_conversions = 0;
    {
        const std::lock_guard<std::mutex> held{mutex_};
        if (stopped_) { return; }
        stopped_ = true;
        installed_timer = timer_id_;
        timer_id_ = 0;
        // drain() takes the coalescer's open window with it. That window is the
        // NEWEST row in the process, so a stop that flushed only the closed rows
        // would discard the one row nothing else has a copy of.
        static_cast<void>(flush_locked(batch, sessions));
        lost_behaviour = buffered_.dropped_behaviour();
        lost_conversions = buffered_.dropped_conversions();
    }

    // The flush timer holds `this`, and nothing in this class keeps the sink
    // alive for it — the same defect as a pool task capturing `this`, with a
    // different owner. A timer left installed by a destroyed sink fires on freed
    // memory every second for the life of the loop.
    //
    // Taken out BEFORE the final flush so a tick cannot arrive during it and
    // drain the buffer this is about to write.
    if (installed_timer != 0) {
        drogon::app().getLoop()->invalidateTimer(
            static_cast<trantor::TimerId>(installed_timer));
    }

    if (!batch.empty() || !sessions.empty()) {
        // Written HERE, on the calling thread, not posted. stop() runs during
        // shutdown, and a task posted to a pool that is about to drain is a task
        // that may never run. Blocking is acceptable exactly once, on the way
        // out.
        try {
            auto client = db::MongoPool::instance().acquire();
            if (const Status written = write_batch(*client, events_repo_, sessions_repo_,
                                                   config_.retention, batch, sessions);
                !written) {
                LOG_ERROR << "analytics: final flush of " << batch.size()
                          << " row(s) failed";
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "analytics: final flush of " << batch.size()
                      << " row(s) threw: " << e.what();
        }
    }

    if (lost_conversions > 0) {
        // Said first and said separately. A conversion is refused only when the
        // buffer holds nothing compressible at all, so every one of these is the
        // state worth alerting on rather than a load signal.
        LOG_ERROR << "analytics: " << lost_conversions
                  << " CONVERSION(s) dropped over this process's lifetime — the buffer held "
                     "nothing compressible";
    }
    if (lost_behaviour > 0) {
        LOG_WARN << "analytics: " << lost_behaviour
                 << " behaviour row(s) dropped over this process's lifetime";
    }
}

Status EventSink::flush_now(mongocxx::client& client) {
    std::vector<EventRow> batch;
    std::vector<VisitorId> sessions;
    {
        const std::lock_guard<std::mutex> held{mutex_};
        if (flush_locked(batch, sessions) == 0 && sessions.empty()) { return ok(); }
    }
    return write_batch(client, events_repo_, sessions_repo_, config_.retention, batch,
                       sessions);
}

std::size_t EventSink::buffered() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return buffered_.size();
}

std::uint64_t EventSink::dropped_behaviour() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return buffered_.dropped_behaviour();
}

std::uint64_t EventSink::dropped_conversions() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return buffered_.dropped_conversions();
}

std::uint64_t EventSink::sampled_out() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return sampled_out_;
}

std::uint64_t EventSink::refused_flushes() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return refused_flushes_;
}

std::uint64_t EventSink::deferred_flushes() const noexcept {
    const std::lock_guard<std::mutex> held{mutex_};
    return deferred_flushes_;
}

}  // namespace anvil::analytics
