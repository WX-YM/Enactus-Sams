// versioned-write-exempt: a step rewrites a document to what it SHOULD be, and
// a version filter would make every step fail against any document an
// application had edited since the step was written — which is every document
// the step exists to reach. The correctness argument here is idempotence, not
// version matching (docs/18-data-migrations.md §5): each write is a $set or a
// $unset addressed by `_id`, so applying it twice is a no-op.

#include "anvil/db/data_migrations.h"

#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/model/update_one.hpp>
#include <mongocxx/options/find.hpp>

#include "anvil/core/uuid.h"
#include "anvil/db/codec.h"

namespace anvil::db {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::sub_document;

// One batch's documents, OWNED.
//
// A bsoncxx::document::view borrows the cursor's internal buffer, and that
// buffer is reused the moment the cursor advances — so a span of views over a
// cursor is a use-after-free by the time the step reads the second document.
// The values are materialised first and the views point into those.
struct Batch final {
    std::vector<bsoncxx::document::value> documents;
    std::vector<bsoncxx::document::view>  views;

    void clear() noexcept {
        views.clear();
        documents.clear();
    }
};

// A 16-byte binary `_id`, which is what Cursor::IdRange declares.
[[nodiscard]] bool is_uuid_id(const bsoncxx::document::element& id) noexcept {
    if (!id || id.type() != bsoncxx::type::k_binary) { return false; }
    const bsoncxx::types::b_binary bytes = id.get_binary();
    return bytes.size == 16 &&
           (bytes.sub_type == bsoncxx::binary_sub_type::k_uuid ||
            bytes.sub_type == bsoncxx::binary_sub_type::k_binary);
}

// The millisecond a UUIDv7 `_id` encodes, for a ledger row an operator can read
// without knowing the collection. Absent for anything that is not a v7.
[[nodiscard]] std::optional<TimeMs> created_at_of(const bsoncxx::document::element& id) noexcept {
    if (!is_uuid_id(id)) { return std::nullopt; }
    const bsoncxx::types::b_binary bytes = id.get_binary();
    Uuid value{};
    for (std::size_t i = 0; i < value.size(); ++i) { value[i] = bytes.bytes[i]; }
    if ((value[6] >> 4) != 7) { return std::nullopt; }
    return TimeMs{std::chrono::milliseconds{uuid::v7_timestamp_ms(value)}};
}

// `{_id: {$gt: last}}`, in `_id` order, bounded.
//
// Never skip(n): it is O(n) server-side, so the last batch of a large collection
// costs the most at exactly the point the run is most likely to be interrupted
// (ENGINEERING_RULES.md §7). The whole document is read rather than a projection, because
// the transform is the step's and anvil cannot know which fields it needs.
[[nodiscard]] bool read_batch(mongocxx::client& client, std::string_view database,
                              const MigrationStep& step,
                              const std::optional<bsoncxx::types::bson_value::value>& after,
                              Batch& batch) {
    batch.clear();

    bsoncxx::builder::basic::document filter;
    if (after.has_value()) {
        filter.append(kvp("_id", [&after](sub_document sub) {
            sub.append(kvp(codec::key_of("$gt"), after->view()));
        }));
    }

    mongocxx::options::find options{};
    options.sort(make_document(kvp("_id", bsoncxx::types::b_int32{1})));
    options.limit(static_cast<std::int64_t>(step.batch_size));

    auto cursor = client[std::string{database}][std::string{step.collection}].find(
        filter.view(), options);

    batch.documents.reserve(step.batch_size);
    for (const bsoncxx::document::view& document : cursor) {
        batch.documents.emplace_back(document);
    }
    batch.views.reserve(batch.documents.size());
    for (const bsoncxx::document::value& document : batch.documents) {
        batch.views.push_back(document.view());
    }
    return !batch.documents.empty();
}

// One bulk write per batch, ordered, every model addressed by `_id`.
[[nodiscard]] std::int64_t execute(mongocxx::client& client, std::string_view database,
                                   const MigrationStep& step,
                                   std::span<const StepWrite> writes) {
    if (writes.empty()) { return 0; }

    auto bulk = client[std::string{database}][std::string{step.collection}].create_bulk_write();
    for (const StepWrite& write : writes) {
        const bsoncxx::document::value filter = make_document(kvp("_id", write.id.view()));
        const bsoncxx::document::value update = make_document(
            kvp(codec::key_of(write.kind == StepWriteKind::Set ? "$set" : "$unset"),
                write.fields.view()));
        bulk.append(mongocxx::model::update_one{filter.view(), update.view()});
    }
    (void)bulk.execute();
    return static_cast<std::int64_t>(writes.size());
}

// Everything that has to be true before a step is claimed. Reported as Refused
// — exit 3, the catalogue does not describe this cluster — rather than as a
// failure, because nothing was attempted and nothing is half-applied.
[[nodiscard]] std::string precondition_failure(mongocxx::client& client,
                                               const MigrationDeps& deps,
                                               const MigrationStep& step) {
    if (step.apply == nullptr) { return "the step declares no body"; }

    const std::int32_t applied = applied_schema_version(
        client, deps.databases.for_collection(step.collection));
    if (applied < step.min_schema_version) {
        // A step that reads through an index declares the version that created
        // it. Refusing is what stops a deploy whose order slipped from
        // collection-scanning production.
        return "the step needs schema version " + std::to_string(step.min_schema_version) +
               " and this database is at " + std::to_string(applied);
    }
    return {};
}

void run_one_step(mongocxx::client& client, const MigrationDeps& deps,
                  const MigrationStep& step, const RunOptions& options,
                  MigrationLedger& ledger, StepReport& report, RunOutcome& outcome) {
    const std::string_view database = deps.databases.for_collection(step.collection);

    if (const std::string refusal = precondition_failure(client, deps, step); !refusal.empty()) {
        report.state = StepState::Refused;
        report.detail = refusal;
        outcome = RunOutcome::Inconsistent;
        return;
    }

    // Read before claiming. Not a check-then-act: a step the ledger says is
    // finished needs no lock at all, and the claim below is still one atomic
    // operation for every step that does.
    const Result<std::optional<LedgerEntry>> existing = ledger.read(client, step.name);
    if (!existing) {
        report.state = StepState::Failed;
        report.detail = "the ledger could not be read";
        outcome = RunOutcome::StepFailed;
        return;
    }
    if (existing.value().has_value() && existing.value()->done) {
        // Zero work, because this invocation did none. The totals the previous
        // run accumulated are on the ledger row and belong to --status, which
        // reads it: a report that carried them forward would say a run moved
        // documents it never touched.
        report.state = StepState::AlreadyDone;
        return;
    }

    LedgerProgress progress{};
    if (!options.dry_run) {
        const Result<LedgerEntry> claimed = ledger.claim(client, step.name, options.lease);
        if (!claimed) {
            // Reported HELD and not waited on. A second operator watching a
            // migration "hang" is how two of them end up force-killing the one
            // that was working.
            report.state = StepState::Held;
            report.detail = "another runner holds the lease";
            outcome = RunOutcome::Held;
            return;
        }
        progress.cursor = claimed.value().cursor;
        progress.cursor_at = claimed.value().cursor_at;
        progress.documents = claimed.value().documents;
        progress.batches = claimed.value().batches;
    } else if (existing.value().has_value()) {
        // A dry run resumes from where the real one got to, so it reports the
        // work that is actually left rather than the work there once was.
        progress.cursor = existing.value()->cursor;
        progress.cursor_at = existing.value()->cursor_at;
    }

    const std::int64_t scanned_before = progress.documents;
    StepContext context{client, database, step.collection};
    Batch batch;

    for (;;) {
        if (!options.dry_run) {
            // Renews the lease AND detects a changed owner, in one operation, at
            // the TOP of the batch — so a fenced runner stops before it writes
            // rather than after. It is asserted by looking at what it wrote.
            if (const Status renewed = ledger.record(client, step.name, progress, options.lease);
                !renewed) {
                report.state = StepState::Failed;
                report.detail = "the lease was taken by another runner";
                outcome = RunOutcome::StepFailed;
                return;
            }
        }

        if (!read_batch(client, database, step, progress.cursor, batch)) { break; }

        const bsoncxx::document::element last_id = batch.views.back()["_id"];
        if (step.cursor == Cursor::IdRange && !is_uuid_id(last_id)) {
            // A cursor proved against one `_id` shape is a cursor that breaks on
            // the other, and it breaks by walking a prefix and reporting that it
            // finished. Refused loudly instead.
            report.state = StepState::Refused;
            report.detail = "the collection's _id is not a 16-byte uuid, which IdRange declares";
            outcome = RunOutcome::Inconsistent;
            if (!options.dry_run) {
                (void)ledger.record_failure(client, step.name, progress, report.detail);
            }
            return;
        }

        context.clear();
        const StepOutcome applied = step.apply(context, batch.views);
        if (applied != StepOutcome::Ok) {
            report.state = StepState::Failed;
            report.detail = "the step refused a batch";
            outcome = RunOutcome::StepFailed;
            if (!options.dry_run) {
                (void)ledger.record_failure(client, step.name, progress, report.detail);
            }
            return;
        }

        if (options.dry_run) {
            report.documents_written += static_cast<std::int64_t>(context.writes().size());
        } else {
            report.documents_written += execute(client, database, step, context.writes());
        }

        progress.cursor = bsoncxx::types::bson_value::value{last_id.get_value()};
        progress.cursor_at = created_at_of(last_id);
        progress.documents += static_cast<std::int64_t>(batch.views.size());
        ++progress.batches;
        ++report.batches;
    }

    report.documents_scanned = progress.documents - scanned_before;
    report.state = StepState::Applied;

    if (!options.dry_run) {
        if (const Status finished = ledger.finish(client, step.name, progress); !finished) {
            report.state = StepState::Failed;
            report.detail = "the lease was taken by another runner";
            outcome = RunOutcome::StepFailed;
        }
    }
}

}  // namespace

DataMigrationReport run_data_migrations(mongocxx::client& client, const MigrationDeps& deps,
                                        const RunOptions& options) {
    DataMigrationReport report{};
    report.outcome = RunOutcome::Applied;
    report.steps.reserve(deps.steps.size());

    MigrationLedger ledger{std::string{ledger_database(deps.databases)}, options.runner,
                           std::string{options.label}};

    for (const MigrationStep& step : deps.steps) {
        StepReport& entry = report.steps.emplace_back();
        entry.name = step.name;

        // A failed or held step blocks every step after it, because N+1 was
        // written by somebody who assumed N ran. Recorded per step rather than
        // implied by the run's outcome, so --status says which ones never
        // started.
        if (report.outcome != RunOutcome::Applied) {
            entry.state = StepState::Blocked;
            continue;
        }
        if (!options.only.empty() && options.only != step.name) {
            entry.state = StepState::Skipped;
            continue;
        }

        try {
            run_one_step(client, deps, step, options, ledger, entry, report.outcome);
        } catch (const mongocxx::exception&) {
            // No driver text reaches the report, and the run stops here: a step
            // that threw is a step in an unknown state, and the next one was
            // written assuming this one finished.
            entry.state = StepState::Failed;
            entry.detail = "the server refused an operation during this step";
            report.outcome = RunOutcome::StepFailed;
        } catch (const std::exception&) {
            entry.state = StepState::Failed;
            entry.detail = "the step could not be completed";
            report.outcome = RunOutcome::StepFailed;
        }
    }

    return report;
}

}  // namespace anvil::db
