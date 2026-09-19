// A cursor, and the absence of an offset.
//
// `skip(n)` is O(n) server-side: the database walks and discards n documents to
// answer page five hundred, and it does that for every reader on every refresh.
// anvil has no route that accepts one (anvil `ENGINEERING_RULES.md` §7), so the only thing
// left to decide is whether a client can ASK for one — and the answer here is
// that it cannot spell one. There is no `offset` field on any type in this
// module and no function that takes a number as a position.
//
// The brand carries the route id because a cursor is only meaningful to the
// route that issued it: it encodes that route's sort key, and handing it to
// another list is either a decode error at the server or, worse, a page of the
// wrong rows. A plain `string` cursor type would make that a run-time discovery.
//
// It is opaque. Nothing in this library parses one, and nothing should: its
// contents are the server's business, and a client that reads them is a client
// that breaks when the server changes its sort.

import type { Brand } from "./brand.js";
import { brand } from "./brand.js";
import type { Result } from "./result.js";
import { fail, ok } from "./result.js";

export type Cursor<RouteId extends string> = Brand<string, `cursor:${RouteId}`>;

// Minting is deliberately not re-exported from the entry point, the way
// `brand` is not: a consumer that can mint a cursor can mint one that never came
// from a server, and the type stops being evidence of anything. The envelope
// decode is the one caller.
export function cursorFromServer<RouteId extends string>(
    _routeId: RouteId,
    value: string,
): Cursor<RouteId> {
    return brand<string, `cursor:${RouteId}`>(value);
}

// `after: null` is the first page. It is null rather than absent so that a
// caller has to say which it means: an omitted key and a key holding the cursor
// look identical at a call site, and the one that silently means "start again"
// is the one that shows up as a list that never advances.
export type PageRequest<RouteId extends string> = {
    readonly limit: number;
    readonly after: Cursor<RouteId> | null;
};

export type PageRequestError = "limit-not-positive" | "limit-above-maximum";

// The maximum comes from the descriptor's entry for that route. It is a failure
// rather than a clamp: a caller that asked for two hundred and silently received
// fifty renders a list that looks complete and is not, and the bug is in the
// screen rather than in the request.
export function pageRequest<RouteId extends string>(request: {
    readonly limit: number;
    readonly maxLimit: number;
    readonly after: Cursor<RouteId> | null;
}): Result<PageRequest<RouteId>, PageRequestError> {
    if (!Number.isInteger(request.limit) || request.limit <= 0) {
        return fail("limit-not-positive");
    }
    if (request.limit > request.maxLimit) {
        return fail("limit-above-maximum");
    }
    return ok({ limit: request.limit, after: request.after });
}
