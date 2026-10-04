// A section's published document and its draft, which are two documents.
//
// anvil stores them separately and answers them separately, and the thing that
// goes wrong in a client is not the fetch — it is the CACHE. A draft read into
// the published entry is a draft rendered to every visitor, from one editor's
// preview, with no request having gone wrong anywhere. That is the whole reason
// this module exists rather than the call site passing a flag
// (`docs/01-seams.md` §8).
//
// So the stage is not optional and there is no default. `open` takes one, the
// stage reaches the request through a query the application shapes, and the
// query is part of the cache key by construction (`state/cache.ts`) — which
// makes "a draft is never served from the published key" a property of the key
// rather than of anybody's care.
//
// --- the labels ---------------------------------------------------------------
//
// A section field's labels are the one place the descriptor carries words a
// person reads, and they are not an exception to §13: they are the
// application's own labels for its own editors, declared once in its own C++
// table and generated into the client with the rest of the schema. What this
// module ships is the INDEXING, because the index is the part that is easy to
// get wrong — the array is ordered by the locale table, which is persisted
// server-side and append-only, so the client never renumbers it and never
// assumes index 0 means anything (`docs/01-seams.md` §5).

import type { ApiTypes, CallableRoute } from "../wire/client.js";
import type { PathParams, RouteQuery } from "../wire/route.js";

import type { Resource, ResourceBody, ResourceFailure, ResourceStore } from "./resource.js";

// Two documents, named. A boolean would make `open(key, true)` a call site
// nobody can read, and the one that matters is the one somebody wrote `false`
// into by accident.
export type SectionStage = "published" | "draft";

const kStages: readonly SectionStage[] = ["published", "draft"];

export type SectionsConfig<A extends ApiTypes, R extends CallableRoute & { readonly capability: null }> = {
    readonly resources: ResourceStore<A>;
    readonly route: R;

    // Whatever the route's own path takes, which is fixed for the whole surface:
    // a section is addressed by its key, and the key travels in the query
    // alongside the stage.
    readonly params?: PathParams;

    // How the stage and the section key reach the request. The names are the
    // server's vocabulary, so the application shapes the query and hammer names
    // nothing (`CLAUDE.md` §1) — the same seam the pager takes for its limit and
    // its cursor.
    readonly query: (where: { readonly key: string; readonly stage: SectionStage }) => RouteQuery;

    // Which bounded class these live in. A section's rows carry words in every
    // declared locale, so the entries are large and the ceiling is the
    // application's to choose (`state/cache.ts`).
    readonly class?: string;
};

export class Sections<A extends ApiTypes, R extends CallableRoute & { readonly capability: null }> {
    private readonly config: SectionsConfig<A, R>;

    constructor(config: SectionsConfig<A, R>) {
        this.config = config;
    }

    // One section at one stage. The handle is the resource's own, so the caller
    // releases it the way it releases every other one — an entry nobody releases
    // is a request that outlives the screen that wanted it.
    open(
        key: string,
        stage: SectionStage,
    ): Resource<ResourceBody<A, R>, ResourceFailure<A>> {
        return this.config.resources.open(this.config.route, {
            query: this.config.query({ key, stage }),
            ...(this.config.params === undefined ? {} : { params: this.config.params }),
            ...(this.config.class === undefined ? {} : { class: this.config.class }),
            // One cast, at the one boundary where a route typed per call meets a
            // class generic over it — the same place and the same reason as
            // `state/resource.ts`. The parameters are checked where the
            // application spells them, in `params` above.
        } as never);
    }

    // A draft was published. Both entries are now wrong — the published one
    // because it has changed, and the draft one because it no longer differs —
    // and dropping only the first is how an editor keeps seeing the draft they
    // just published as though it were still pending.
    //
    // Opened and released around the `forget`, which looks like a detour and is
    // not: `open` is the only thing that knows this stage's cache key, and a
    // handle taken without being given back is a watcher that never leaves —
    // which would keep the entry alive, let the re-read land, and put the value
    // straight back into the cache this was clearing.
    published(key: string): void {
        for (const stage of kStages) {
            const held = this.open(key, stage);
            held.forget();
            held.release();
        }
    }
}

// A label for one locale, by the index the locale table gives it.
//
// The index is the server's and the array is one entry per declared locale, so a
// short array is a generation failure rather than something to fall back from
// (`docs/01-seams.md` §8). It is still read defensively, because this runs on a
// row that arrived over the wire: what it will not do is silently substitute
// another locale's words, which is the failure that reads as a translation
// nobody wrote.
export function labelAt(labels: readonly string[], localeIndex: number): string | null {
    if (!Number.isInteger(localeIndex) || localeIndex < 0) {
        return null;
    }
    return labels[localeIndex] ?? null;
}
