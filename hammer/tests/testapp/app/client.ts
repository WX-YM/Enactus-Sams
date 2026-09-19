// The reference consumer's client, and the proof that the seam is satisfiable
// from outside hammer.
//
// Everything hammer needs that it may not know — the origins, the tables, the
// route a refresh goes to, the platform's own singletons — is handed over here,
// from the generated module and from the deployment (docs/01-seams.md §16). If
// any of it could not be supplied from an application, it would fail HERE, at
// type-check time, which is the only place it can fail cheaply.

import { createClient } from "hammer/wire";
import type {
    ApiOriginRequest,
    Capability,
    Client,
    ExclusiveLocks,
    FanOut,
    FetchLike,
    SessionSource,
} from "hammer/wire";

import type { Api } from "../api/hammer.generated.js";
import {
    kApiTables,
    kPageLimitMax,
    routeAuthRefresh,
    routeContentDelete,
    routeContentGet,
    routeMediaList,
} from "../api/hammer.generated.js";

// The type parameter is the generated module's. It was four imports and a
// hand-written mapped type here, which is what every application had to write
// and what the first one to consume this library copied a stale version of.
export type { Api } from "../api/hammer.generated.js";

export type AppClient = Client<Api>;

export function client(platform: {
    // Unvalidated. `createClient` checks the pairing and throws on one it cannot
    // drive: a cross-site API is not a degraded client but a session that never
    // exists, and the only alternative to failing here is every request going
    // out anonymously.
    readonly origin: ApiOriginRequest;
    readonly fetch: FetchLike;
    readonly session: SessionSource;
    readonly locks: ExclusiveLocks | null;
    readonly fanOut: FanOut;
    readonly onLogout: () => void;
}): AppClient {
    return createClient<Api>({
        // Every table this client decodes with, from the one generated module
        // whose hash the session response is checked against.
        api: kApiTables,

        // Which route a refresh goes to is not in the descriptor, so it is named
        // here rather than guessed at by the generator.
        refreshRoute: routeAuthRefresh,

        origin: platform.origin,
        fetch: platform.fetch,
        session: platform.session,
        locks: platform.locks,
        fanOut: platform.fanOut,
        onLogout: platform.onLogout,
    });
}

// A read. The parameters are the route's own, so a typo in `id` is a compile
// error and a missing one is not spellable.
export function readContent(api: AppClient, id: string, signal: AbortSignal) {
    return api.call(routeContentGet, { params: { id }, signal });
}

// A page. The limit is the descriptor's maximum for this route and there is no
// offset to pass — `after` is where the server said the last page ended.
export function listMedia(
    api: AppClient,
    where: { readonly ns: string; readonly id: string },
    after: string | null,
    signal: AbortSignal,
) {
    return api.call(routeMediaList, {
        params: where,
        query: { limit: kPageLimitMax, after },
        signal,
    });
}

// A destructive call. It cannot be written without a `Capability<"ContentDelete">`,
// and the only producer of one is the mint call — so "the user confirmed this"
// is discharged by the compiler rather than by a reviewer.
export function deleteContent(
    api: AppClient,
    id: string,
    capability: Capability<"ContentDelete">,
    signal: AbortSignal,
) {
    return api.call(routeContentDelete, { params: { id }, capability, signal });
}

// The response-shape seam, exercised.
//
// anvil validates requests through a schema binder and writes responses by hand,
// so there is nothing in the descriptor to emit that would not be a guess
// (`docs/01-seams.md` §4). The application declares what a route answers with by
// augmenting the generated interface, and a route with no declaration stays
// `unknown` — which forces its call sites to narrow loudly rather than inherit a
// lie.
declare module "../api/hammer.generated.js" {
    interface RouteResponses {
        "content.get": { readonly title: string; readonly starred: boolean; readonly version: number };
        "media.list": { readonly items: readonly { readonly id: string }[]; readonly next: string | null };
    }
}
