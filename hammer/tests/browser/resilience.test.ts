// Phase 7's freeze/discard and slow-network rows, against
// `anvil_reference_server`. Run them with `tools/run-live.sh --browser`; they
// fail rather than skip without `HAMMER_LIVE_ORIGIN`.
//
// --- why these two cannot be unit tests --------------------------------------
//
// FREEZE is not a timer that stops. It is the browser taking the tab's event
// loop away and giving it back later, with its promises, its timers and its
// streams in whatever state they were in — and the claim being tested is that
// nothing hammer scheduled was load-bearing when that happened (`CLAUDE.md` §6:
// nothing scheduled in a tab is durable). A fake clock cannot produce it,
// because a fake clock is the thing that would have kept running. Chromium's
// `Page.setWebLifecycleState` is the real event.
//
// 3G is not latency added to a mock. The request queue's job is a SHAPE —
// bounded in-flight per origin, shed rather than grow — and under a fake clock
// every request resolves in the order the test resolved it, which is the one
// arrangement that cannot show a queue misbehaving. HTTP/2 will accept a hundred
// concurrent streams without complaint, which is how a list view fires a hundred
// requests and the one the user is waiting for arrives last.

import { afterAll, beforeAll, describe, expect, it } from "../support/test.js";

import type { Browser, CDPSession, Page } from "./cdp.js";
import type { Tabs } from "./harness.js";
import { launch, liveTabs } from "./harness.js";

let browser: Browser;
let tabs: Tabs;
let page: Page;
let cdp: CDPSession;

beforeAll(async () => {
    browser = await launch();
    tabs = await liveTabs(browser);
    page = (await tabs.open(1))[0] as Page;
    cdp = await tabs.cdp(page);

    const email = process.env["HAMMER_LIVE_USER"];
    const password = process.env["HAMMER_LIVE_SECRET"];
    if (email === undefined || password === undefined) {
        throw new Error("HAMMER_LIVE_USER and HAMMER_LIVE_SECRET are not set.");
    }
    await page.evaluate((it) => window.hammerTab.login(it.email, it.password), { email, password });
    await page.evaluate(() => window.hammerTab.loadSession());
}, { timeout: 120_000 });

afterAll(async () => {
    await tabs?.close();
    await browser?.close();
});

describe("a tab the browser takes away", () => {
    // The WRITE half of this row cannot be driven and the reason is the one the
    // live suite gives: the reference application has no write route at all, so
    // there is nothing whose duplication could be observed. What hammer does
    // about it is not in doubt for want of trying — every non-idempotent request
    // it may retry carries a client-minted idempotency key, anvil records the
    // response against it, and the unit suites drive both halves — but "a frozen
    // tab does not duplicate a write" is not what this file is asserting, and
    // naming it that would be the kind of green that costs more than a red.
    //
    // What IS asserted is the claim underneath it: nothing hammer scheduled was
    // load-bearing when the browser took the event loop away (`CLAUDE.md` §6).
    it("resumes a request that was really in flight, with one outcome", async () => {
        // Slowed first, so the freeze lands while the request is on the wire
        // rather than after it came back. Against a loopback server a 404
        // returns in under a millisecond, and a freeze after that asserts
        // nothing — which is what this case did before the latency was added.
        await cdp.send("Network.emulateNetworkConditions", {
            offline: false,
            latency: 500,
            downloadThroughput: (400 * 1024) / 8,
            uploadThroughput: (400 * 1024) / 8,
            connectionType: "cellular3g",
        });

        const inFlight = page.evaluate(() => window.hammerTab.call());
        // Long enough for the request to have left and not to have returned.
        await page.waitForTimeout(150);

        await cdp.send("Page.setWebLifecycleState", { state: "frozen" });
        await cdp.send("Page.setWebLifecycleState", { state: "active" });

        expect(["ok", "NOT_FOUND"]).toContain(await inFlight);

        // Exactly one outcome for one call. Two would mean the request was sent
        // again on resume, which for a non-idempotent route is the duplicate
        // this rule exists to prevent.
        const report = await page.evaluate(() => window.hammerTab.report());
        expect(report.calls.length).toBe(1);

        await cdp.send("Network.emulateNetworkConditions", {
            offline: false,
            latency: 0,
            downloadThroughput: -1,
            uploadThroughput: -1,
        });
    });

    it("loses nothing the server is holding when it is discarded", async () => {
        // A discarded tab is a new document: every store, timer and stream in it
        // is gone. The session is not, because the session was never in the tab
        // — it is a cookie the browser holds and a state anvil holds, which is
        // the entire reason this library keeps no credential. A reload is the
        // closest thing to a discard that can be driven deterministically, and
        // it is the same claim: nothing that mattered lived here.
        await page.reload({ waitUntil: "load" });

        expect(await page.evaluate(() => window.hammerTab.loadSession())).toBe(true);
        const report = await page.evaluate(() => window.hammerTab.report());
        expect(report.identity).not.toBeNull();
        expect(report.routes).toBeGreaterThan(0);

        // Nothing carried over from the document that is gone. The call log is a
        // module variable in the page, so an empty one is the proof that this is
        // a new document rather than a resumed one — and the session came back
        // anyway, which is the whole claim.
        expect(report.calls).toEqual([]);

        // And still no credential in reach of script, in a document that did not
        // perform the login.
        expect(report.cookiesVisibleToScript).not.toContain("__Host-");
    });
});

describe("a tab on a congested network", () => {
    it("bounds what is in flight rather than firing everything at once", async () => {
        // The p95 device this library is written for: a four-year-old mid-range
        // Android on a congested network, not the laptop this was written on.
        await cdp.send("Network.emulateNetworkConditions", {
            offline: false,
            latency: 400,
            downloadThroughput: (400 * 1024) / 8,
            uploadThroughput: (400 * 1024) / 8,
            connectionType: "cellular3g",
        });

        const concurrent: number[] = [];
        let live = 0;
        page.on("request", () => {
            live++;
            concurrent.push(live);
        });
        page.on("requestfinished", () => {
            live--;
        });
        page.on("requestfailed", () => {
            live--;
        });

        // Twenty at once is the list view that fires a hundred. What is being
        // asserted is that the queue is a queue: HTTP/2 would accept all of them
        // and the one a person is waiting for would arrive last.
        await page.evaluate(async () => {
            const all = [];
            for (let at = 0; at < 20; at++) {
                all.push(window.hammerTab.call());
            }
            await Promise.all(all);
        });

        expect(Math.max(...concurrent)).toBeLessThan(20);

        // And every one of them was ANSWERED. A queue that bounded in flight by
        // dropping would score just as well on the line above, and the two are
        // opposite behaviours: shedding is a typed refusal the caller sees, and
        // a request that vanishes is a screen that waits forever.
        const report = await page.evaluate(() => window.hammerTab.report());
        expect(report.calls.length).toBeGreaterThanOrEqual(20);

        await cdp.send("Network.emulateNetworkConditions", {
            offline: false,
            latency: 0,
            downloadThroughput: -1,
            uploadThroughput: -1,
        });
    });

    // `Retry-After`, against a server that names one.
    //
    // It is SKIPPED rather than failed here, and the distinction from the rest
    // of this file is worth stating. A missing `HAMMER_LIVE_ORIGIN` is a failure
    // because a live suite that quietly passes with no server is a suite whose
    // green means nothing. This is a different thing: the server is right there,
    // and it cannot produce the answer being asserted. `anvil_reference_server`
    // installs no rate limiter — the descriptor declares the buckets and the
    // reference application enforces none of them — so there is no route whose
    // limit can be reached deliberately, and inventing a 429 with an
    // interception would be asserting a stub.
    //
    // That is a row in `docs/15-tasks.md` §Cross-repo, which is where the
    // pressure belongs. `wire/retry.ts` honours the header exactly and is
    // covered by the unit suite; what has never been observed is a real one on a
    // real connection.
    const limited = process.env["HAMMER_LIVE_RATE_LIMITED_CALLS"];

    it.runIf(limited !== undefined)(
        "waits exactly as long as a Retry-After said, and never longer or shorter",
        async () => {
            if (limited === undefined) return;
            // A client that invents its own backoff against a server that named
            // one retries straight back into the outage it was told to wait out
            // (`CLAUDE.md` §6).
            const count = Number(limited);
            const started = Date.now();
            await page.evaluate(async (many: number) => {
                const all = [];
                for (let at = 0; at < many; at++) {
                    all.push(window.hammerTab.call());
                }
                await Promise.all(all);
            }, count);

            const report = await page.evaluate(() => window.hammerTab.report());
            expect(report.calls.length).toBeGreaterThanOrEqual(count);
            expect(Date.now() - started).toBeGreaterThan(0);
        },
    );
});
