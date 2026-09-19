// The platform a live run supplies, and what it honestly stands in for.
//
// --- what this suite found the first time it ran ----------------------------
//
// It had never run: anvil was a library plus test executables and served the
// reference application on no port, which was a row in `docs/15-tasks.md`
// §Cross-repo. `anvil_reference_server` closed it, and the first execution
// found that `decodeSessionView` was written against a session payload anvil
// does not send — `perms` as a base64url mask at the root, where anvil writes
// an `authority` object carrying permission NAMES. Every one of this
// repository's 1,145 unit tests passed over it, because every one of them built
// its payload with the same builder the decode was written against.
//
// That is the row's whole justification in one defect. A shared fixture that
// agrees with the implementation is two copies of one belief, and nothing but a
// real server tells them apart.
//
// --- what a Node run can assert, and what it cannot -------------------------
//
// The jar below is the BROWSER's job, done by the test. A browser keeps cookies
// out of script's reach; Node's `fetch` keeps no cookies at all, so a live run
// here has to hold them somewhere, and holding them in the harness rather than
// in hammer is what keeps the library's own property true: the client never
// reads a cookie, and this file is not part of the library.
//
// What it therefore CANNOT assert, and what phase 7's browser run exists for:
//
//   that the cookie is `__Host-` scoped and `HttpOnly`, which is enforcement no
//   jar performs and the whole reason the token is unreadable;
//   that two real tabs share one lock manager and one jar, which is the rotation
//   race that has to be observed with two contexts;
//   that a frozen or discarded tab resumes without duplicating a write.
//
// Everything below is the half that is about REQUESTS: what is sent, what comes
// back, and what this client does with it.
//
// --- two holders, because one cannot show the interesting half --------------
//
// The reference application seeds a staff editor holding `ContentRead` and a
// superadmin holding NOTHING. Both are needed: the projection is only visible
// as a projection when two holders are handed two different tables, and
// `authority.superadmin` only means anything against an account whose permission
// set is deliberately not all-ones. A suite with one account asserts that a
// table arrived, which is the half that was never in doubt.

import type { ExclusiveLocks, FanOut, FetchLike } from "../../src/wire/index.js";
import { noFanOut } from "../../src/wire/index.js";
import type { AppState, Credentials } from "../testapp/app/state.js";
import { appState } from "../testapp/app/state.js";

// A missing origin is a FAILED run rather than a skipped one. A live suite that
// quietly passes when there is no server is a suite whose green means nothing,
// which is worse than one that does not exist.
export function liveOrigin(): string {
    const origin = process.env["HAMMER_LIVE_ORIGIN"];
    if (origin === undefined || origin === "") {
        throw new Error(
            "HAMMER_LIVE_ORIGIN is not set. The live suite needs a running anvil; " +
                "see docs/16-test-plan.md §Phase 6–7.",
        );
    }
    return origin;
}

// The reference application draws both at boot and prints them once, so nothing
// here is a constant and nothing is read out of a file. A run against a server
// whose credentials were not passed in is a run that would have signed in as
// whatever this repository happened to remember.
export function liveCredentials(): Credentials {
    const email = process.env["HAMMER_LIVE_USER"];
    const password = process.env["HAMMER_LIVE_SECRET"];
    if (email === undefined || password === undefined) {
        throw new Error("HAMMER_LIVE_USER and HAMMER_LIVE_SECRET are not set.");
    }
    return { email, password };
}

// One jar per run, and it is not shared between runs on purpose: a live suite
// that inherited a session from the last one would pass on a credential path
// that no longer works.
export class CookieJar {
    private readonly held: Map<string, string> = new Map();

    get count(): number {
        return this.held.size;
    }

    // Only the name and the value are kept. The attributes are the browser's
    // business and a jar that pretended to enforce `Secure`, `SameSite` or
    // `__Host-` would be asserting its own implementation rather than the
    // server's header — which is exactly the trap phase 7's browser run exists
    // to avoid.
    accept(header: string): void {
        const pair = header.split(";", 1)[0] ?? "";
        const split = pair.indexOf("=");
        if (split <= 0) {
            return;
        }
        const name = pair.slice(0, split).trim();
        const value = pair.slice(split + 1).trim();
        if (value === "" || value === "deleted") {
            this.held.delete(name);
            return;
        }
        this.held.set(name, value);
    }

    header(): string | null {
        if (this.held.size === 0) {
            return null;
        }
        return [...this.held].map(([name, value]) => `${name}=${value}`).join("; ");
    }

    names(): readonly string[] {
        return [...this.held.keys()];
    }
}

// The `fetch` hammer is handed. It adds what a browser would add for
// `credentials: "include"` and NOTHING else: no retry, no header of its own, no
// body inspection.
export function jarFetch(jar: CookieJar): FetchLike {
    return async (url, init) => {
        const headers = new Headers(init.headers as HeadersInit | undefined);
        const cookies = jar.header();
        if (cookies !== null && init.credentials !== "omit") {
            headers.set("Cookie", cookies);
        }

        const response = await fetch(url, { ...init, headers, redirect: init.redirect ?? "manual" });

        // `getSetCookie` keeps them apart; a joined header cannot be split on
        // commas, because an `Expires` attribute contains one.
        for (const header of response.headers.getSetCookie()) {
            jar.accept(header);
        }
        return response;
    };
}

export type LiveRun = {
    readonly state: AppState;
    readonly jar: CookieJar;
    readonly close: () => void;
};

// The second seeded account, whose password is passed in the same way and for
// the same reason. Absent means the superadmin cases do not run, and they say so
// rather than passing: there is no assertion about a superadmin that an editor's
// session can stand in for.
export function liveSuperadmin(): Credentials | null {
    const email = process.env["HAMMER_LIVE_SUPERADMIN"];
    const password = process.env["HAMMER_LIVE_SUPERADMIN_SECRET"];
    if (email === undefined || password === undefined) {
        return null;
    }
    return { email, password };
}

// The reference consumer, pointed at a real server. It is the SAME construction
// every other suite drives — a live run that built its own client would be
// asserting a client no application has.
export function liveRun(): LiveRun {
    const origin = liveOrigin();
    const jar = new CookieJar();

    const state = appState({
        fetch: jarFetch(jar),
        // Node has no lock manager. The degraded path is the one a browser
        // without `navigator.locks` takes as well, and it is documented rather
        // than emulated: a refresh with no leader is one tab refreshing alone,
        // which is correct for a run with one process in it.
        locks: null,
        fanOut: noFanOut,
        pageOrigin: origin,
        apiOrigin: origin,
        // Never called here. A live run uploads a `File` as a handle and never
        // decodes an image, so a factory that throws is more honest than a
        // worker that would never receive a message.
        imageWorker: () => {
            throw new Error("the live suite decodes no images");
        },
        beaconTo: { sendBeacon: () => false },
        count: () => undefined,
    });

    return { state, jar, close: () => state.close() };
}

export function signal(afterMs = 10_000): AbortSignal {
    return AbortSignal.timeout(afterMs);
}

// Satisfies the compiler's `ExclusiveLocks` import where a run wants to pass one
// explicitly. Exported rather than inlined so that the day a live run does have
// a lock manager — a browser context, phase 7 — it is a one-line change here.
export type LiveLocks = ExclusiveLocks | null;
export type LiveFanOut = FanOut;
