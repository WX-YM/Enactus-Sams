// A list, one page at a time, and no way to ask for page five hundred.
//
// `skip(n)` is O(n) server-side: the database walks and discards n documents to
// answer that page, for every reader on every refresh. anvil has no route that
// accepts one, and `core/cursor.ts` makes sure a client cannot spell one — there
// is no offset on any type it exports and no function that takes a number as a
// position. This module is the thing built on top of that, and it inherits the
// property rather than restating it.
//
// --- `hasMore` comes from the cursor, never from a count ---------------------
//
// The tempting version is `items.length === limit`, and it is wrong in both
// directions. A page that happens to come back exactly full at the end of the
// list shows a "load more" that loads nothing; a server that filtered rows after
// the fetch returns a short page in the middle of the list and the reader is
// told there is nothing further. The server knows where the list ends and says
// so by handing back a cursor or not handing one back, and that is the only
// thing this module reads.
//
// --- the page's shape is the application's -----------------------------------
//
// Response shapes are not in the descriptor (`docs/01-seams.md` §4), so a reader
// function says where the rows and the next cursor are. That reader only ever
// runs on a body this client received, which is what keeps `cursorFromServer`
// honest: a `Cursor` still cannot be produced without the server having issued
// one.

import type { Cursor, PageRequestError } from "../core/cursor.js";
import { cursorFromServer, pageRequest } from "../core/cursor.js";
import type { HammerError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import type { ApiTypes, CallableRoute, Client } from "../wire/client.js";
import type { PathParams, RouteQuery } from "../wire/route.js";

import type { ResourceBody, ResourceFailure } from "./resource.js";
import type { Readable } from "./store.js";
import { Store } from "./store.js";

// A route the descriptor describes as paged. A route without a `page` block
// cannot be handed to this class at all, which is the compiler saying what a
// comment would otherwise have to: there is no cursor field to read and no
// maximum to bound the limit by.
export type PagedRoute = CallableRoute & {
    readonly page: { readonly cursor: string; readonly limitMax: number };
};

// What one response carried. `next` is the server's opaque cursor, or null where
// the server said the list ends.
export type Page<Row> = {
    readonly items: readonly Row[];
    readonly next: string | null;
};

// One shape, every property declared (`ENGINEERING_RULES.md` §2.3).
export type PagerState<Row, E> =
    | {
          readonly status: "idle";
          readonly items: readonly Row[];
          readonly hasMore: boolean;
          readonly error: null;
      }
    | {
          readonly status: "loading";
          readonly items: readonly Row[];
          readonly hasMore: boolean;
          readonly error: null;
      }
    | {
          readonly status: "ready";
          readonly items: readonly Row[];
          readonly hasMore: boolean;
          readonly error: null;
      }
    | {
          readonly status: "failed";
          // The pages already loaded stay. A failure at page four does not empty
          // the three the person has been reading.
          readonly items: readonly Row[];
          readonly hasMore: boolean;
          readonly error: E;
      };

export type PagerConfig<A extends ApiTypes, R extends PagedRoute, Row> = {
    readonly client: Client<A>;
    readonly route: R;
    readonly params?: PathParams;

    // How many rows per page. Checked against the descriptor's maximum for this
    // route, and a request above it FAILS rather than being clamped: a caller
    // that asked for two hundred and silently received fifty renders a list that
    // looks complete and is not (`core/cursor.ts`).
    readonly limit: number;

    // Where the rows and the next cursor are in this route's response, and what
    // else the request carries besides the page. Both are the application's,
    // because the response shape and the server's query vocabulary are.
    readonly readPage: (body: ResourceBody<A, R>) => Page<Row> | null;
    readonly query: (page: {
        readonly limit: number;
        readonly after: Cursor<R["id"]> | null;
    }) => RouteQuery;
};

// The pager's own refusals, and the page-request ones it passes through
// unchanged. A limit above the descriptor's maximum and a limit that is not a
// positive integer are two different mistakes, and collapsing them would hand a
// caller a reason that does not describe what they did.
export type PagerError = PageRequestError | "unreadable-page";

export class Pager<A extends ApiTypes, R extends PagedRoute, Row> {
    private readonly config: PagerConfig<A, R, Row>;
    private readonly state: Store<PagerState<Row, ResourceFailure<A> | PagerError>>;
    private after: Cursor<R["id"]> | null;
    private inFlight: Promise<void> | null;

    // Replaced rather than reused on a reset. An `AbortController` cannot be
    // un-aborted, so a pager that kept one would abort its first page and then
    // silently refuse every page after it.
    private lifetime: AbortController;
    private closed: boolean;

    constructor(config: PagerConfig<A, R, Row>) {
        this.config = config;
        this.state = new Store<PagerState<Row, ResourceFailure<A> | PagerError>>({
            status: "idle",
            items: [],
            hasMore: true,
            error: null,
        });
        this.lifetime = new AbortController();
        this.closed = false;
        this.after = null;
        this.inFlight = null;
    }

    get store(): Readable<PagerState<Row, ResourceFailure<A> | PagerError>> {
        return this.state;
    }

    // The next page, or nothing. Concurrent callers join the one in flight: a
    // list that fires a second request because the person scrolled twice is a
    // list that will show a page twice.
    more(): Promise<void> {
        const running = this.inFlight;
        if (running !== null) {
            return running;
        }
        const held = this.state.get();
        if (!held.hasMore || this.closed) {
            return Promise.resolve();
        }

        const started = this.load();
        this.inFlight = started;
        const settle = (): void => {
            if (this.inFlight === started) {
                this.inFlight = null;
            }
        };
        void started.then(settle, settle);
        return started;
    }

    // Back to the first page. What an invalidation does to a list: the rows
    // below page one were selected relative to a cursor that described a list
    // that has since changed, so keeping them and re-reading page one would
    // show a row twice or skip one.
    reset(): void {
        this.lifetime.abort();
        this.lifetime = new AbortController();
        this.inFlight = null;
        this.after = null;
        this.state.set({ status: "idle", items: [], hasMore: true, error: null });
    }

    close(): void {
        this.closed = true;
        this.lifetime.abort();
        this.state.close();
    }

    private async load(): Promise<void> {
        // Captured before the await. `reset` replaces the controller, so a load
        // that read `this.lifetime` afterwards would ask the new one whether the
        // old request was cancelled and be told no.
        const { signal } = this.lifetime;
        const held = this.state.get();
        this.state.set({
            status: "loading",
            items: held.items,
            hasMore: held.hasMore,
            error: null,
        });

        const request = pageRequest<R["id"]>({
            limit: this.config.limit,
            maxLimit: this.config.route.page.limitMax,
            after: this.after,
        });
        if (!request.ok) {
            // Reported rather than thrown: a bad limit is a call site's mistake,
            // and a list that crashes the tab over one is worse than a list that
            // says why it is empty.
            this.settle(held.items, held.hasMore, request.error);
            return;
        }

        // One cast, at the one boundary where a route typed per call meets a
        // class generic over it — the same place and the same reason as
        // `state/resource.ts`.
        const call = this.config.client.call.bind(this.config.client) as unknown as (
            route: CallableRoute,
            options: {
                readonly params: PathParams;
                readonly query: RouteQuery;
                readonly signal: AbortSignal;
            },
        ) => Promise<Result<unknown, HammerError>>;

        const answered = await call(this.config.route, {
            params: this.config.params ?? {},
            query: this.config.query({ limit: request.value.limit, after: request.value.after }),
            signal,
        });

        if (signal.aborted) {
            return;
        }

        if (!answered.ok) {
            this.settle(held.items, held.hasMore, answered.error as ResourceFailure<A>);
            return;
        }

        const page = this.config.readPage(answered.value as ResourceBody<A, R>);
        if (page === null) {
            // A body this route's reader does not recognise. Reported rather
            // than treated as an empty page: an empty page ends the list, and
            // ending a list because a response changed shape is a list that
            // silently loses its tail.
            this.settle(held.items, held.hasMore, "unreadable-page");
            return;
        }

        // One loop and one new array, rather than a `concat` per page. A list
        // paged forty times through `concat` allocates forty arrays of
        // everything before it.
        const items = held.items.slice();
        for (const row of page.items) {
            items.push(row);
        }

        this.after = page.next === null ? null : cursorFromServer<R["id"]>(this.config.route.id, page.next);
        this.state.set({
            status: "ready",
            items,
            // The server said where the list ends. Never `items.length === limit`.
            hasMore: page.next !== null,
            error: null,
        });
    }

    private settle(
        items: readonly Row[],
        hasMore: boolean,
        error: ResourceFailure<A> | PagerError,
    ): void {
        this.state.set({ status: "failed", items, hasMore, error });
    }
}
