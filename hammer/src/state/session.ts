// Who this tab belongs to, what they reach, and whether this bundle is older
// than the server it is talking to.
//
// It holds no credential and cannot: anvil's tokens are `__Host-` cookies with
// `HttpOnly`, so every screen hammer ships around them is a screen driving a
// credential it is unable to read (`ENGINEERING_RULES.md` §9). What it holds is the
// decoded session VIEW — a 128-bit permission set, the holder-scoped route
// table, and the descriptor hash the server was built from — and all of it is
// memory-only and dies with the tab (`docs/00-architecture.md` §7).
//
// --- the bootstrap, and the gap that used to be here -------------------------
//
// Reading a session needs an address, and this store does not own the read: the
// application hands it one, the way it supplies `refreshRoute`
// (`docs/01-seams.md` §16).
//
// That indirection existed because the address of the session route arrived
// WITH the session — anvil described it as `authenticated`, the generator
// withheld its path the way it withholds every holder route's, and on a cold
// load there was nothing to resolve it from, so the read could not be spelled at
// all and an application wrote the path out by hand. anvil describes
// `session.current` as public now, for the reason `auth.refresh` already was: a
// route gated on the credential it exists to establish works only while it is
// unnecessary. So the closure is handed a generated route `const` and the second
// copy is gone.
//
// The indirection stays, because it was never only about the address: this store
// is the client's `SessionSource` and the client is what performs the read, so
// one of the two has to be told about the other after both exist.
//
// --- what a failure here must not do -----------------------------------------
//
// A session read that fails is not a logout. A dropped connection, a 503 and a
// timeout are all failures of the READ, and a store that cleared its identity on
// one would sign somebody out of a valid session every time a train went into a
// tunnel. Only an answer from the server — an authenticated route replying that
// there is nobody there — ends a session, and the refresh machinery in
// `wire/credentials.ts` has already had its turn before that answer arrives.

import type { HammerError } from "../core/errors.js";
import type { StaleClientError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import type { AffordableRoute } from "../wire/affordance.js";
import { affordsRoute, holdsAll } from "../wire/affordance.js";
import type { SessionSource } from "../wire/resolve.js";
import type { PermissionBits, SessionDecodeError, SessionView } from "../wire/session_view.js";
import { decodeSessionView, staleClient } from "../wire/session_view.js";

import type { Identity } from "./cache.js";
import type { CountSink } from "./counts.js";
import { kNoCounts } from "./counts.js";
import type { Readable } from "./store.js";
import { Store } from "./store.js";

// How the session is read. Supplied by the application after the client exists,
// because the client is constructed with this store as its `SessionSource` and
// the two would otherwise each need the other first.
export type SessionRead = (signal: AbortSignal) => Promise<Result<unknown, HammerError>>;

// One shape, every property declared (`ENGINEERING_RULES.md` §2.3). `status` discriminates
// so that a screen can narrow; `view` is null in two of the three members and
// the union says which.
export type SessionState =
    | {
          // Nothing has been read yet. Distinguished from `anonymous` because
          // rendering a signed-out surface before the first read has answered is
          // a login form that flashes at every person who is already signed in.
          readonly status: "unknown";
          readonly view: null;
          readonly identity: null;
          readonly stale: null;
          readonly error: null;
      }
    | {
          readonly status: "anonymous";
          readonly view: null;
          readonly identity: null;
          readonly stale: null;
          // Why there is no session, where the server said. Null when the
          // application signed out on purpose.
          readonly error: HammerError | SessionDecodeError | null;
      }
    | {
          readonly status: "active";
          readonly view: SessionView;
          readonly identity: Identity;

          // The bundle is older than the API (`docs/00-architecture.md` §7.1).
          // Surfaced and never acted on: an automatic reload discards whatever
          // the user had typed, on the deploy most likely to be happening during
          // working hours.
          readonly stale: StaleClientError | null;
          readonly error: null;
      };

const kUnknown = {
    status: "unknown",
    view: null,
    identity: null,
    stale: null,
    error: null,
} as const;

export type SessionStoreConfig = {
    // The descriptor hash this bundle was generated from, so a server built from
    // a different one can be reported. The generated module exports it; hammer
    // cannot know it.
    readonly clientHash: string;

    // Every permission name to the bit anvil stores it in, from the generated
    // module. anvil sends the holder's permissions as NAMES in bit order, so
    // nothing can turn them back into the 16 bytes this library holds without
    // this table (`wire/session_view.ts`).
    //
    // Required rather than optional, although an empty one would "work": a
    // store with no table decodes every session to no permissions, and the
    // symptom is a screen with every non-route affordance missing and nothing
    // to explain it. A thing an application must supply fails at type-check
    // time or it fails at 3am (`ENGINEERING_RULES.md` §1).
    readonly permissionBits: PermissionBits;

    // Where the identity comes from. The session payload's shape past the three
    // fields `decodeSessionView` reads is the application's (`docs/01-seams.md`
    // §4), so the application is what points at the id — and the cache is keyed
    // by whatever comes back, which is why it returns null rather than throwing
    // on a body it does not recognise.
    readonly identityOf?: (body: unknown) => Identity;

    readonly count?: CountSink;
};

export class SessionStore {
    private readonly config: SessionStoreConfig;
    private readonly state: Store<SessionState>;
    private readonly count: CountSink;

    private read: SessionRead | null;
    private inFlight: Promise<SessionView | null> | null;

    constructor(config: SessionStoreConfig) {
        this.config = config;
        this.count = config.count ?? kNoCounts;
        this.state = new Store<SessionState>(kUnknown);
        this.read = null;
        this.inFlight = null;
    }

    // Told once, after the client that will do the reading exists. A second call
    // is programmer error rather than a reconfiguration: two readers is two
    // opinions about who is signed in, which is the thing this store exists to
    // be the single one of (`ENGINEERING_RULES.md` §3.3).
    readsFrom(read: SessionRead): void {
        if (this.read !== null) {
            throw new Error("the session store already has a reader");
        }
        this.read = read;
    }

    get store(): Readable<SessionState> {
        return this.state;
    }

    current(): SessionView | null {
        return this.state.get().view;
    }

    identity(): Identity {
        return this.state.get().identity;
    }

    // Whether a control should exist, answered from the session's own table
    // rather than from the bits — anvil's `satisfies()` short-circuits on user
    // type, and a superadmin's permission set is deliberately not all-ones, so a
    // client counting bits would hide every guarded control from the one account
    // that reaches all of them (`wire/affordance.ts`).
    affords(route: AffordableRoute): boolean {
        return affordsRoute(this.current(), route);
    }

    // For an affordance that is NOT a route, where there is no server-built
    // table to consult and the bits are all there is.
    holds(bits: readonly number[]): boolean {
        return holdsAll(this.current(), bits);
    }

    // What `createClient` takes. A separate object rather than this class,
    // because the client needs exactly two functions and handing it the store
    // would hand it the right to clear the session it is reading.
    get source(): SessionSource {
        return {
            current: () => this.current(),
            refetch: (signal) => this.load(signal),
        };
    }

    // Read the session. Concurrent callers join the one in flight: the 403 path,
    // a holder route that resolved to nothing and the application's own first
    // load all reach here, and three reads of one session is three rotations of
    // nothing and one answer.
    load(signal: AbortSignal): Promise<SessionView | null> {
        const running = this.inFlight;
        if (running !== null) {
            return running;
        }

        const started = this.perform(signal);
        this.inFlight = started;
        // Both arms rather than `finally`, for the reason `wire/client.ts` gives:
        // a `finally` builds a derived promise that rejects when the original
        // does, and nothing is attached to that one.
        const settle = (): void => {
            if (this.inFlight === started) {
                this.inFlight = null;
            }
        };
        void started.then(settle, settle);
        return started;
    }

    // The session is over. Called from the application's `onLogout`, which is
    // reached by this tab signing out and by another tab's broadcast alike
    // (`wire/credentials.ts`).
    clear(): void {
        this.publish({
            status: "anonymous",
            view: null,
            identity: null,
            stale: null,
            error: null,
        });
    }

    close(): void {
        this.state.close();
    }

    private async perform(signal: AbortSignal): Promise<SessionView | null> {
        const read = this.read;
        if (read === null) {
            // Nothing has told this store how to read a session, so there is
            // nothing to report but the absence. Not a throw: a resolution that
            // reaches here during construction would otherwise take down the
            // first call an application makes.
            return null;
        }

        const answered = await read(signal);
        if (signal.aborted) {
            return this.current();
        }

        if (!answered.ok) {
            // A failure of the READ is not a logout (see the header). The only
            // answer that ends a session is the server saying there is nobody
            // there, and by the time a 401 reaches here the refresh has already
            // had its turn.
            if (answered.error.kind === "server" && answered.error.status === 401) {
                this.publish({
                    status: "anonymous",
                    view: null,
                    identity: null,
                    stale: null,
                    error: answered.error,
                });
                return null;
            }
            return this.current();
        }

        const decoded = decodeSessionView(answered.value, this.config.permissionBits);
        if (!decoded.ok) {
            // A malformed payload is refused whole rather than kept in part:
            // every path in that table becomes the path of an authenticated
            // request, and half a table is a table nothing checked
            // (`wire/session_view.ts`).
            this.publish({
                status: "anonymous",
                view: null,
                identity: null,
                stale: null,
                error: decoded.error,
            });
            return null;
        }

        const view = decoded.value;
        const identity = this.config.identityOf?.(answered.value) ?? null;
        this.publish({
            status: "active",
            view,
            identity,
            stale: staleClient(view, this.config.clientHash),
            error: null,
        });
        return view;
    }

    private publish(next: SessionState): void {
        // Counted here and the drop counted where it happens: this store knows
        // the bundle and the server disagree, and `state/resource.ts` knows what
        // was thrown away because of it (`docs/00-architecture.md` §9).
        if (next.stale !== null && this.state.get().stale === null) {
            this.count("stale-client");
        }
        this.state.set(next);
    }
}
