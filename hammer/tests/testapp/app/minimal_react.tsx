// The React half of `docs/02-getting-started.md`, compiled.
//
// `react_screen.tsx` is the exhaustive proof — every hook the adapter ships, in
// one tree, over one application state. This is the narrow version a person
// reads first, and it exists for the reason `minimal.ts` does: the guide's React
// example described `useResource("inbox.list", { query })` and `useSession()`
// for its whole life, and neither has ever been a signature this library has.

import type { ReactElement } from "react";
import { useCallback } from "react";

import { affordsRoute, holdsAll } from "hammer/wire";
import type { ResourceFailure } from "hammer/state";
import { useResource, useSession } from "hammer/react";

import { kPermFormPii, routeContentGet } from "../api/hammer.generated.js";
import type { Api } from "../api/hammer.generated.js";
import type { AppState } from "./state.js";
import { openContent } from "./state.js";

type Document = { readonly title: string; readonly starred: boolean; readonly version: number };

// #region react-a-read
function Content({ state, id }: { readonly state: AppState; readonly id: string }): ReactElement {
    // `useSession` takes the STORE. The hook subscribes to it; it does not find
    // one for you, because a library that reached for an ambient singleton would
    // be deciding your application's composition.
    const session = useSession(state.session);

    // `useResource` takes a function that OPENS a resource, not a route and a
    // query. Opening is yours — it is where the cache key, the invalidation set
    // and the decode live — and the hook supplies only the part React can: when
    // the subscription starts and when it stops.
    //
    // The factory is the identity that decides the lifetime, exactly as
    // `useSyncExternalStore`'s own `subscribe` argument is, and it carries the
    // same obligation: memoise it over the address. An inline arrow releases and
    // re-opens on every render, which is an aborted request per keystroke of
    // whatever else is on the screen.
    const open = useCallback(() => openContent(state, id), [state, id]);
    const view = useResource<Document, ResourceFailure<Api>>(open);

    // An affordance, never a control. `affordsRoute` reads the session's own
    // route table — the server built it with the same `satisfies()` its request
    // filter calls — so it cannot disagree with what the server will do.
    if (!affordsRoute(session.view, routeContentGet)) {
        return <></>;
    }

    // Where the affordance is NOT a route — a column of personal data, a bulk
    // action, a tab — the bits answer instead. `holdsAll` short-circuits on
    // superadmin, because anvil's held set is deliberately not all-ones and a
    // client that only counted bits would hide the whole application from the
    // one account that reaches all of it.
    const showPii = holdsAll(session.view, [kPermFormPii]);

    // `status` is "loading" | "ready" | "failed", and `value` is null until it
    // is ready.
    if (view.state.status !== "ready" || view.state.value === null) {
        return <p aria-busy={view.state.status === "loading"} />;
    }

    return (
        <p dir="auto" data-pii={showPii}>
            {view.state.value.title}
        </p>
    );
}
// #endregion

export { Content };
