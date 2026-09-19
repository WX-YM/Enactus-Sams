// A screen only a holder reaches, built as its own bundle.
//
// It is the reference consumer's proof that `hammer/wire` is usable from outside
// hammer with nothing but a generated route `const` and a session — and it is
// what the built-output assertions read. What it proves is the narrow thing
// §4.1 buys: the route id and the call site are here, because they live in
// whichever chunk calls them, and the PATH is not — not in this bundle, not in
// any chunk, and not in a source map. It arrives with the session, scoped by the
// server to what these permissions reach.
//
// Path secrecy is defence in depth and never a boundary; anvil's stealth 404 is
// the control, and it is worth exactly as much after this file as before it.

import {
    affordsRoute,
    buildRoute,
    decodeEnvelope,
    errorVocabulary,
    holdsAll,
    requestPath,
    resolveRoute,
} from "hammer/wire";
import type {
    ErrorVocabulary,
    ResolveError,
    ResponseFacts,
    RouteBuildError,
    RouteTarget,
    SessionSource,
    SessionView,
} from "hammer/wire";
import type { Result } from "hammer";
import { ok } from "hammer";

import type { ErrorCode, ValidationReason } from "../api/hammer.generated.js";
import {
    kErrorCodeValues,
    kPageLimitMax,
    kPermFormPii,
    kValidationReasonValues,
    routeAuditList,
    topicStaffAssigned,
} from "../api/hammer.generated.js";
import { copy } from "./copy.js";

// Whether the audit entry is in the menu at all. Answered from the table the
// server scoped to this holder, so it agrees with what the server will do.
export function showsAuditEntry(session: SessionView | null): boolean {
    return affordsRoute(session, routeAuditList);
}

// An affordance that is not a route: whether this screen offers the column that
// holds personal data. The bit is a number, so no permission name reaches the
// bundle.
export function showsPiiColumn(session: SessionView | null): boolean {
    return holdsAll(session, [kPermFormPii]);
}

// Whether this screen offers to follow the topic at all — the same question
// `showsAuditEntry` asks of a route, asked of a topic, because subscribing is
// the disclosure the way calling is (docs/01-seams.md §4.1, §9).
export function offersAssignmentAlerts(session: SessionView | null): boolean {
    return holdsAll(session, topicStaffAssigned.perms);
}

// Which entry of a preferences response this topic is. By CODE, because a holder
// topic has no key compiled into this bundle to compare against: the key arrives
// from the server, filtered to what this holder may subscribe to, and the code is
// what re-attaches it to the const the screen holds.
export function assignmentAlertsEnabled(
    entries: readonly { readonly code: number; readonly enabled: boolean }[],
): boolean | null {
    for (const entry of entries) {
        if (entry.code === topicStaffAssigned.code) {
            return entry.enabled;
        }
    }
    return null;
}

// Where the request would go. The screen does not know and cannot be compiled
// knowing: it asks, and a missing address comes back as the shape everything
// absent comes back as.
export function auditTarget(
    source: SessionSource,
    signal: AbortSignal,
): Promise<Result<RouteTarget, ResolveError>> {
    return resolveRoute(routeAuditList, source, signal);
}

// The names the decode knows, under the types the call sites spell. This is the
// one place the two halves can meet: the names come off the generated value
// maps and the unions are erased by the time hammer runs, so hammer cannot
// build this and an application cannot get it wrong — both halves are the
// generated module's.
//
// Built on demand rather than at module scope: a `new Set` on import is a
// side effect, and a module with one is a module a bundler will not drop.
export function failureVocabulary(): ErrorVocabulary<ErrorCode, ValidationReason> {
    return errorVocabulary<ErrorCode, ValidationReason>({
        codes: kErrorCodeValues,
        reasons: kValidationReasonValues,
    });
}

// The address of one page of the audit list. The limit is the descriptor's
// maximum for the route, and there is no offset to pass: `after` is where the
// server said the last page ended.
export function auditPageAddress(
    target: RouteTarget,
    after: string | null,
): Result<string, RouteBuildError> {
    const built = buildRoute(target, {
        params: {},
        query: { limit: kPageLimitMax, after },
    });
    if (!built.ok) {
        return built;
    }
    return ok(requestPath(built.value));
}

// What a failed call says to this screen, in one locale.
//
// The lookup is the proof: `decoded.error.code` is a member of the generated
// union rather than a string, so the copy table is total over it and a code the
// server adds is a compile error here — not a blank line on a screen, which is
// what a fallback string would have bought.
export function auditFailureCopy(response: ResponseFacts): string | null {
    const decoded = decodeEnvelope(response, failureVocabulary());
    if (decoded.ok) {
        return null;
    }
    return copy.en.errors[decoded.error.code];
}
