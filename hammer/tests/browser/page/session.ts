// The reference consumer, running in a real tab against a real anvil.
//
// This is the same construction every other suite drives — `appState` from
// `tests/testapp/app/state.ts` — handed the platform a browser actually has
// rather than the stand-ins a Node run has to supply:
//
//   fetch            the browser's, with `credentials: "include"`, so the cookie
//                    jar is the browser's and hammer still cannot read it. That
//                    absence is the property; a Node run holds the jar in the
//                    harness and therefore asserts a jar it wrote itself.
//   navigator.locks  a real lock manager, shared between every tab on this
//                    origin. It is what elects the one tab that refreshes.
//   BroadcastChannel a real channel, so the logout fan-out crosses tabs.
//
// Two pages loading this module are two tabs of one session. Nothing here
// coordinates them: the coordination is the library's, and observing it is the
// entire point of loading this twice.

import { channelFanOut, locksFrom } from "hammer/wire";

import { routeContentGet } from "../../testapp/api/hammer.generated.js";
import type { AppState } from "../../testapp/app/state.js";
import { appState, openSession, signIn, signOut } from "../../testapp/app/state.js";

export type TabReport = {
    // `refreshes` is how many this tab performed; `unelected` is
    // `hammer_refresh_races_total` and must be zero, because a non-zero value
    // means people are being signed out by their own second tab
    // (`docs/00-architecture.md` §9).
    readonly refreshes: number;
    readonly unelected: number;
    readonly takeovers: number;

    readonly identity: string | null;
    readonly routes: number;

    // Permission names the server sent that this bundle has no bit for. Zero
    // against the reference application, and a run that saw one would be a run
    // against a server newer than the client — which must never end a session
    // and must always be visible.
    readonly unknownPermissions: readonly string[];
    readonly superadmin: boolean;

    readonly cookiesVisibleToScript: string;
    readonly calls: readonly string[];
};

declare global {
    interface Window {
        hammerTab: {
            login: (email: string, password: string) => Promise<boolean>;
            loadSession: () => Promise<boolean>;
            call: () => Promise<string>;
            logout: () => Promise<void>;
            report: () => TabReport;
        };
    }
}

const origin = window.location.origin;
const calls: string[] = [];

// The identity the cache is keyed by. It comes from `identity.me` rather than
// from the session body, because anvil's session response carries none.
let who: string | null = null;

const state: AppState = appState({
    // The browser's own `fetch`. hammer adds `credentials: "include"`; what
    // comes back is never read for a cookie by anything in the library.
    fetch: (url, init) => fetch(url, init),
    locks: locksFrom(navigator.locks),
    fanOut: channelFanOut(new BroadcastChannel("hammer-session")),
    pageOrigin: origin,
    apiOrigin: origin,
    imageWorker: () => {
        throw new Error("this run decodes no images");
    },
    // A real dedicated worker, built from the reference consumer's own worker
    // entry and served beside this page by the harness.
    prehashWorker: () => new Worker(new URL("./prehash_worker.js", import.meta.url), { type: "module" }),
    beaconTo: navigator,
    count: () => undefined,
});

function signal(): AbortSignal {
    return AbortSignal.timeout(20_000);
}

window.hammerTab = {
    login: async (email, password) => {
        const answered = await signIn(state, { email, password }, signal());
        return answered.ok;
    },

    // Through the reference consumer's `openSession`, which reads the session
    // AND the identity and adopts it into the cache. A run that only called
    // `session.load` would be driving half of what an application has to do and
    // asserting that the half worked.
    loadSession: async () => {
        who = await openSession(state, signal());
        return state.session.current() !== null;
    },

    // One protected call, whose address arrived with the session. Recorded
    // rather than asserted here: what each run wants from it differs, and a page
    // that decided would be a page the run could not ask a new question of.
    call: async () => {
        const answered = await state.api.call(routeContentGet, {
            params: { id: "1" },
            signal: signal(),
        });
        // The server's own CODE for a server error, rather than the kind. A run
        // that saw only `"server"` could not tell a 404 from a 503, and the two
        // mean opposite things about whether the request happened.
        const outcome = answered.ok
            ? "ok"
            : answered.error.kind === "server"
              ? answered.error.code
              : answered.error.kind;
        calls.push(outcome);
        return outcome;
    },

    // The route and then the local clear, through the reference consumer's own
    // `signOut` — because a sign-out that only clears locally leaves a session
    // anvil is still holding, and the fan-out this run is here to observe is the
    // SECOND half of it.
    logout: async () => {
        await signOut(state, signal());
        who = null;
    },

    report: () => {
        const counts = state.api.counts();
        const view = state.session.current();
        return {
            refreshes: counts.refreshes,
            unelected: counts.unelected,
            takeovers: counts.takeovers,
            identity: state.session.current() === null ? null : who,
            routes: view?.routes.size ?? 0,
            unknownPermissions: view === null ? [] : [...view.unknownPermissions],
            superadmin: view?.superadmin ?? false,
            // The assertion a Node run cannot make. anvil's credentials are
            // `__Host-` cookies with `HttpOnly`, so this string must contain
            // none of them — and it is the browser enforcing that, not a jar
            // this repository wrote.
            cookiesVisibleToScript: document.cookie, // ban-exempt: asserting the absence
            calls: [...calls],
        };
    },
};
