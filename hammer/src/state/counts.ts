// What this layer counts, and why it is counted rather than logged.
//
// `docs/00-architecture.md` §9 gives the reason in one line: a log line per
// occurrence is not a metric, and under the load that makes one of these fire, a
// line per occurrence is itself the problem. So each of these is a name and a
// tally, reported through the application's own analytics seam — this library
// declares no metric name in an application's namespace and ships no reporter.
//
// The two worth watching are the two that are invisible from outside. A rollback
// is the count of times the interface told somebody something had happened that
// had not. A consent refusal that is not zero means a screen is reporting events
// it was never given permission to report, and finding that from the server side
// means noticing an absence.

export type StateCount =
    // An entry left a bounded cache because the ceiling was reached. Steady
    // growth here is a class whose ceiling is too small for the screen using it,
    // which reads to a user as a list that reloads every time they scroll back.
    | "cache-evicted"

    // The identity changed and everything held was dropped. Expected once per
    // login and once per logout; more than that is a session store that is
    // re-reading an identity it already had.
    | "identity-dropped"

    // A mutation's declared invalidations were applied, locally or from another
    // tab (`state/invalidate.ts`).
    | "invalidated"

    // A stale entry was served while a revalidation ran. Only ever where the
    // server granted `stale-while-revalidate`.
    | "served-stale"

    // An optimistic value was withdrawn because the write failed
    // (`hammer_optimistic_rollbacks_total`).
    | "optimistic-rollback"

    // A write was refused because the version it read is no longer current. It
    // is a reconciliation and never a retry (`state/versioned.ts`).
    | "version-mismatch"

    // An analytics event was dropped at the door for want of consent. It was not
    // queued (`docs/01-seams.md` §10).
    | "consent-refused"

    // A worker pool refused a task because its queue was full. On `imagePool`
    // this is the memory cap doing its job, and the alternative to the refusal
    // is a killed tab (`docs/00-architecture.md` §3).
    | "pool-rejected"

    // The session says the server was built from a different descriptor than
    // this bundle was generated from (`docs/00-architecture.md` §7.1). Counted
    // rather than acted on: hammer never reloads a page by itself.
    | "stale-client"

    // A stream event arrived that had already been handled. Normal operation on
    // a reconnect, because the stream is at-least-once; a rate that does not
    // track reconnects is a dedupe that is not working.
    | "duplicate-event";

// Where the tallies go. A function rather than an object so that an application
// with no reporter passes nothing and pays nothing.
export type CountSink = (name: StateCount) => void;

export const kNoCounts: CountSink = () => {};
