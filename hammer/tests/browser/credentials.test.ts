// Phase 7's two-tab credential run. The one row `docs/15-tasks.md` calls not
// optional.
//
// Run it with `tools/run-live.sh --browser`. It fails rather than skips without
// `HAMMER_LIVE_ORIGIN`, because a credential suite whose green means "no server
// was present" is the most expensive kind of green there is.
//
// --- why every other suite passes while this is broken -----------------------
//
// anvil rotates the refresh token as a compare-and-swap. Two tabs refreshing
// concurrently is a rotation race whose LOSER is signed out — not an error a
// user sees as an error, but a session that ends in the middle of something.
// hammer's answer is one leader per session: `navigator.locks` elects it and
// `BroadcastChannel` fans the result out.
//
// Every part of that is faked in the unit suites. `tests/state/` runs two store
// instances in one process over a channel the test wrote and a lock manager the
// test wrote, so it asserts that the algorithm is right GIVEN a lock manager
// that behaves as the test believes one does. It cannot assert that Chrome's
// lock manager is shared between two tabs of one origin, that the cookie the
// server rotated reached the second tab, or that `__Host-` scoping did what it
// says. Those are the three things that actually break.
//
// --- how the refresh is made to happen ---------------------------------------
//
// The refresh case needs the access credential to expire DURING the run, and
// anvil offers no way to ask for that: there is no route that invalidates the
// credential it is holding, and the reference application issues a fifteen-minute
// access cookie.
//
// The first version of this case gave up on that and asserted "at most one
// refresh", which held because there were none — a green that meant the two tabs
// had both made an ordinary call. The cookie is DELETED from the browser's own
// jar instead, leaving the refresh cookie alone, which is exactly the state a tab
// is in when an access token expires: a valid session, an unusable access
// credential, and a refresh token that can rotate once. The assertion is then
// `exactly one`, which is a thing that can fail.

import { afterAll, beforeAll, describe, expect, it } from "vitest";

import type { Browser, Page } from "playwright-core";

import type { Tabs } from "./harness.js";
import { launch, liveTabs } from "./harness.js";

let browser: Browser;
let tabs: Tabs;
let first: Page;
let second: Page;

function credentials(): { readonly email: string; readonly password: string } {
    const email = process.env["HAMMER_LIVE_USER"];
    const password = process.env["HAMMER_LIVE_SECRET"];
    if (email === undefined || password === undefined) {
        throw new Error("HAMMER_LIVE_USER and HAMMER_LIVE_SECRET are not set.");
    }
    return { email, password };
}

beforeAll(async () => {
    browser = await launch();
    tabs = await liveTabs(browser);
    const opened = await tabs.open(2);
    first = opened[0] as Page;
    second = opened[1] as Page;
}, 120_000);

afterAll(async () => {
    await tabs?.close();
    await browser?.close();
});

describe("one session, two real tabs", () => {
    it("signs in, and neither tab can read the credential it is using", async () => {
        const who = credentials();
        expect(await first.evaluate((it) => window.hammerTab.login(it.email, it.password), who)).toBe(
            true,
        );

        // The property every unit test passes without. anvil's tokens are
        // `__Host-` cookies with `HttpOnly`; the browser is what enforces that,
        // and `document.cookie` is where the failure would be visible.
        for (const tab of [first, second]) {
            const report = await tab.evaluate(() => window.hammerTab.report());
            expect(report.cookiesVisibleToScript).not.toContain("__Host-");
        }
    });

    it("reaches a protected route from the tab that did not sign in", async () => {
        // The second tab never called login. It has the session because the
        // cookie is the browser's, which is the whole shape of the design.
        expect(await second.evaluate(() => window.hammerTab.loadSession())).toBe(true);

        const report = await second.evaluate(() => window.hammerTab.report());
        expect(report.routes).toBeGreaterThan(0);
        expect(report.identity).not.toBeNull();

        // A 404 here is not a permission error and must never be reported as
        // one (`ENGINEERING_RULES.md` §5) — it means the object is missing OR forbidden and
        // the client is not entitled to know which.
        // `NOT_FOUND` is the server's own code. It is not a permission error
        // and must never be rendered as one: it means the object is missing OR
        // forbidden, and the client is not entitled to know which.
        const outcome = await second.evaluate(() => window.hammerTab.call());
        expect(["ok", "NOT_FOUND"]).toContain(outcome);

        // Every permission name the server sent mapped to a bit in this bundle.
        // One here would mean the server is newer than the client, which must
        // never end a session and must always be visible.
        expect(report.unknownPermissions).toEqual([]);
    });

    it("refreshes once across both tabs, with no rotation race", async () => {
        // The access credential is EXPIRED deliberately, and this is the part of
        // the row that could not be driven by waiting.
        //
        // anvil issues the access cookie with a fifteen-minute lifetime and
        // offers no route that invalidates the credential it is holding, so a
        // run that waited for a real expiry would take fifteen minutes and a run
        // that did not wait would assert nothing — which is what the first
        // version of this case did: it drove both tabs into a call, neither
        // refreshed, and "at most one refresh" held vacuously.
        //
        // Deleting the cookie from the browser's own jar is the real event. The
        // refresh cookie is left alone, which is exactly the state a tab is in
        // when the access token expires: a valid session, an unusable access
        // credential, and a refresh token that can rotate once.
        const before = await tabs.context.cookies();
        expect(before.some((cookie) => cookie.name === "__Host-at")).toBe(true);
        expect(before.some((cookie) => cookie.name === "__Host-rt")).toBe(true);

        await tabs.context.clearCookies({ name: "__Host-at" });

        // Both tabs into a call at the same instant. anvil rotates the refresh
        // token as a compare-and-swap, so two refreshes is a rotation race whose
        // LOSER is signed out — not an error a user sees as an error, but a
        // session that ends in the middle of something.
        const outcomes = await Promise.all([
            first.evaluate(() => window.hammerTab.call()),
            second.evaluate(() => window.hammerTab.call()),
        ]);

        // Both calls were answered. A tab that lost the race would have been
        // signed out and would answer with a credential failure instead.
        for (const outcome of outcomes) {
            expect(["ok", "NOT_FOUND"]).toContain(outcome);
        }

        const reports = await Promise.all(
            [first, second].map((tab) => tab.evaluate(() => window.hammerTab.report())),
        );

        // `unelected` is `hammer_refresh_races_total`. It is never non-zero on a
        // platform that has a lock manager, and a non-zero value means people
        // are being signed out by their own second tab.
        for (const report of reports) {
            expect(report.unelected).toBe(0);
        }

        // EXACTLY one, now that the expiry is real. One tab took
        // `navigator.locks`, refreshed, and broadcast; the other waited for that
        // news rather than queueing on the lock, and replayed its own request
        // afterwards. Two is the rotation race.
        const performed = reports.reduce((total, report) => total + report.refreshes, 0);
        expect(performed).toBe(1);

        // And the second tab did not conclude the leader had died. A takeover
        // here would mean the fan-out did not arrive, which on a slower machine
        // is the same defect arriving later.
        expect(reports.reduce((total, report) => total + report.takeovers, 0)).toBe(0);

        // The server really did rotate: a new access cookie is in the jar, and
        // it is not the one that was deleted. This is the half no unit suite can
        // see — the cookie the server set reached BOTH tabs, because there is
        // one jar and it is the browser's.
        const after = await tabs.context.cookies();
        expect(after.some((cookie) => cookie.name === "__Host-at")).toBe(true);

        // Still unreadable from script, after a rotation as before one.
        for (const tab of [first, second]) {
            const report = await tab.evaluate(() => window.hammerTab.report());
            expect(report.cookiesVisibleToScript).not.toContain("__Host-");
        }
    });

    it("empties the other tab when one signs out", async () => {
        await first.evaluate(() => window.hammerTab.logout());

        // The fan-out is a real `BroadcastChannel` between two real tabs. What
        // it prevents needs two contexts to exist at all: a tab left rendering a
        // signed-in shell after another tab signed out is one shared device away
        // from being a disclosure.
        await second.waitForFunction(() => window.hammerTab.report().identity === null, undefined, {
            timeout: 10_000,
        });

        const report = await second.evaluate(() => window.hammerTab.report());
        expect(report.identity).toBeNull();
        expect(report.routes).toBe(0);
    });
});
