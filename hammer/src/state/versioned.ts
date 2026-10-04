// A read-modify-write that carries the version it read.
//
// anvil's optimistic concurrency answers `VERSION_MISMATCH` when the document
// moved under a writer. The client's job is to RECONCILE — re-read, re-apply,
// re-present — and never to retry the same body, which converts a detected
// conflict into a silent overwrite with extra steps (`CLAUDE.md` §6). That is
// the whole of this module, and the shape is what enforces it: this function
// returns, and there is no loop in it to hide a second attempt in.
//
// --- what "carrying the version" means here ---------------------------------
//
// The version lives somewhere in a body or a header the application chose, both
// of which are the application's (`docs/01-seams.md` §4, §17). So the version is
// READ from the document by a function the application supplies and PUT on the
// request by a function the application supplies, and what this module owns is
// the part that gets forgotten: that the version sent is the one belonging to
// the document currently on screen, and that a write with no version is refused
// rather than sent as an unconditional overwrite.
//
// A write with no version to carry is the lost update this mechanism exists to
// detect, sent deliberately. It is refused here.

import type { Result } from "../core/result.js";
import type { ServerError } from "../core/errors.js";
import { isServerError } from "../core/errors.js";
import type { ApiTypes } from "../wire/client.js";

import type { CountSink } from "./counts.js";
import { kNoCounts } from "./counts.js";
import type { Resource, ResourceFailure } from "./resource.js";

// anvil's code for a lost update. Named once, here, rather than compared at
// every call site: the vocabulary is the descriptor's, and a second spelling is
// a second thing to get wrong on the day a screen needs it most.
const kVersionMismatch = "VERSION_MISMATCH";

// One shape, every property declared.
export type VersionedOutcome<T, E> =
    | {
          // The server took the write and answered with the document it now
          // holds. That document is what is published, not the one this client
          // computed — confirming against the server's own answer is the
          // difference between a write that happened and a write that was sent.
          readonly kind: "written";
          readonly document: T;
          readonly current: null;
          readonly error: null;
      }
    | {
          // Somebody else wrote first. `current` is what the document looks like
          // now, re-read rather than guessed, so the caller can re-apply and
          // re-present. It is null where the re-read itself failed.
          readonly kind: "conflict";
          readonly document: null;
          readonly current: T | null;
          readonly error: null;
      }
    | {
          // There is no version to carry, because the resource has no document
          // to have read one from. Sending anyway is an unconditional overwrite.
          readonly kind: "no-version";
          readonly document: null;
          readonly current: null;
          readonly error: null;
      }
    | {
          readonly kind: "failed";
          readonly document: null;
          readonly current: null;
          readonly error: E;
      };

export type VersionedWrite<A extends ApiTypes, T, Version> = {
    // The resource holding the document being written. Its current value is what
    // the version is read from, and its value is what the server's answer
    // replaces.
    readonly resource: Resource<T, ResourceFailure<A>>;

    // Where the version is in this document. Null for a document that carries
    // none, which is what makes the refusal above reachable rather than
    // theoretical.
    readonly versionOf: (document: T) => Version | null;

    // The request, with the version put wherever this route wants it. It is a
    // parameter rather than a header this library sets, because anvil's
    // framework never reads one: the handler that consumes it is the
    // application's (`docs/01-seams.md` §17).
    readonly send: (
        version: Version,
        signal: AbortSignal,
    ) => Promise<Result<T, ResourceFailure<A>>>;

    readonly signal: AbortSignal;
    readonly count?: CountSink;
};

export async function writeVersioned<A extends ApiTypes, T, Version>(
    write: VersionedWrite<A, T, Version>,
): Promise<VersionedOutcome<T, ResourceFailure<A>>> {
    const count: CountSink = write.count ?? kNoCounts;
    const held = write.resource.state.get();

    // An unconfirmed value is not a document to write from. A version read off
    // one belongs to a document the server never agreed to, and the write it
    // produces is a fabrication built on a fabrication (`state/optimistic.ts`).
    if (held.value === null || write.resource.hasProvisional()) {
        return kNoVersion;
    }

    const version = write.versionOf(held.value);
    if (version === null) {
        return kNoVersion;
    }

    const answered = await write.send(version, write.signal);
    if (answered.ok) {
        write.resource.overwrite(answered.value);
        return { kind: "written", document: answered.value, current: null, error: null };
    }

    if (!isConflict(answered.error)) {
        return { kind: "failed", document: null, current: null, error: answered.error };
    }

    // The reconciliation, and the ONLY thing that follows a mismatch. There is
    // no branch here that sends the same body again: the document changed, so
    // the body this caller computed describes a document that no longer exists.
    count("version-mismatch");
    await write.resource.refresh();
    const reread = write.resource.state.get();
    return { kind: "conflict", document: null, current: reread.value, error: null };
}

// The code and not the status. `VERSION_MISMATCH` and `CONFLICT` are both 409
// in anvil's table, and they want opposite things: one is reconciled, the other
// is never resolved silently. A 409 from something that is not anvil decodes to
// `Unknown`, which is reported as a failure rather than reconciled — re-reading
// and re-presenting on a proxy's 409 would tell somebody their write conflicted
// when it never arrived.
function isConflict(error: ResourceFailure<ApiTypes>): error is ServerError<string, string> {
    return isServerError(error) && error.code === kVersionMismatch;
}

const kNoVersion = {
    kind: "no-version",
    document: null,
    current: null,
    error: null,
} as const;
