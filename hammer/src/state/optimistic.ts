// Showing somebody the result before the server has agreed to it.
//
// `ENGINEERING_RULES.md` §6 draws the line in one sentence: an optimistic update is
// reconcilable or it is absent. Three things follow from that, and all three are
// here rather than at a call site, because a call site gets the third one wrong.
//
//   APPLY LOCALLY. The provisional value reaches the STORE and never the cache
//   (`state/resource.ts`), so a second component opening the same address does
//   not inherit a value the server has not seen.
//
//   CONFIRM AGAINST THE DOCUMENT THE SERVER RETURNS. Not against the one this
//   client computed. A write that succeeded may have been normalised, clamped,
//   renumbered or merged on the way through, and publishing the local guess
//   because the status was 200 is how an interface shows a value that exists
//   nowhere. Where the route answers with no document, the value is not
//   confirmed and the resource is re-read rather than left standing.
//
//   NEVER LET AN UNCONFIRMED VALUE BECOME AN INPUT. A second optimistic update
//   over an outstanding one applies a function to a value nobody agreed to, and
//   a versioned write reads a version off a document that does not exist. Both
//   are refused, and refused by asking the resource rather than by a flag a
//   caller has to remember to pass.
//
// A rollback is counted, because it is the count of times the interface told
// somebody something had happened that had not
// (`hammer_optimistic_rollbacks_total`, `docs/00-architecture.md` §9).

import type { Result } from "../core/result.js";
import type { ApiTypes } from "../wire/client.js";

import type { CountSink } from "./counts.js";
import { kNoCounts } from "./counts.js";
import type { Resource, ResourceFailure } from "./resource.js";

// One shape, every property declared.
export type OptimisticOutcome<T, E> =
    | {
          // Applied, and the server's own document is what is now on screen.
          readonly kind: "confirmed";
          readonly document: T | null;
          readonly error: null;
      }
    | {
          // Applied, then withdrawn. The value on screen is the last one the
          // server confirmed.
          readonly kind: "rolled-back";
          readonly document: null;
          readonly error: E;
      }
    | {
          // Not applied at all: there was nothing to apply to, or a previous
          // optimistic value is still outstanding.
          readonly kind: "refused";
          readonly document: null;
          readonly error: null;
      };

export type OptimisticUpdate<A extends ApiTypes, T> = {
    readonly resource: Resource<T, ResourceFailure<A>>;

    // What the document looks like once this has happened. Pure, and it is
    // handed the last CONFIRMED value rather than whatever is on screen.
    readonly apply: (current: T) => T;

    // The write. It answers with the document the server now holds, or with null
    // for a route that returns none — which is honest rather than convenient,
    // because a null is what makes the re-read below happen.
    readonly commit: (signal: AbortSignal) => Promise<Result<T | null, ResourceFailure<A>>>;

    readonly signal: AbortSignal;
    readonly count?: CountSink;
};

export async function optimistic<A extends ApiTypes, T>(
    update: OptimisticUpdate<A, T>,
): Promise<OptimisticOutcome<T, ResourceFailure<A>>> {
    const count: CountSink = update.count ?? kNoCounts;
    const { resource } = update;
    const held = resource.state.get();

    if (held.value === null || resource.hasProvisional()) {
        return kRefused;
    }

    resource.provisional(update.apply(held.value));

    const answered = await update.commit(update.signal);

    if (!answered.ok) {
        // Back to the last value the server confirmed. `restore` re-reads the
        // cache rather than writing the old value back, so the rollback cannot
        // itself publish something unconfirmed.
        resource.restore();
        count("optimistic-rollback");
        return { kind: "rolled-back", document: null, error: answered.error };
    }

    if (answered.value === null) {
        // The write succeeded and said nothing about the document. The local
        // guess is therefore still a guess: it is withdrawn and the resource is
        // read again, so what ends up on screen came from the server.
        resource.restore();
        await resource.refresh();
        return { kind: "confirmed", document: resource.state.get().value, error: null };
    }

    resource.overwrite(answered.value);
    return { kind: "confirmed", document: answered.value, error: null };
}

const kRefused = {
    kind: "refused",
    document: null,
    error: null,
} as const;
