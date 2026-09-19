// Phase 7's Trusted Types row: a page enforcing Trusted Types renders every
// component and no policy violation is reported.
//
// This is the one row in phase 7's browser set that needs no server, and it is
// therefore the only one with evidence behind it. What it adds over
// `tests/dom/csp.test.ts` is the participant that suite cannot have: happy-dom
// enforces no policy, so that suite asserts the code takes no route to a sink by
// trapping every route. This asserts that Chromium, with the policy actually
// served, agrees — and it asserts it over the same registry, which is the
// reference consumer's own mounts rather than a list written for a test.
//
// The claim is narrow and worth stating precisely. It is NOT that hammer is
// immune to XSS; the library's markup path goes through `DOMParser` and
// `importNode`, which is not a Trusted Types sink, so the policy has nothing of
// hammer's to refuse. What the run buys is that this stays true: a component
// that reached `innerHTML` next year would fail here, in the environment that
// decides, rather than in production on the one deployment that enforces a CSP.

import { afterAll, beforeAll, describe, expect, it } from "vitest";

import type { Browser } from "playwright-core";

import { published } from "../dom/registry.js";
import type { Origin, PageReport } from "./harness.js";
import { launch, serveBundle } from "./harness.js";

let browser: Browser;
let origin: Origin;
let report: PageReport;

beforeAll(async () => {
    browser = await launch();
    origin = await serveBundle("./page/mount_all.ts");

    const page = await browser.newPage();
    await page.goto(origin.url, { waitUntil: "networkidle" });
    report = await page.evaluate(() => window.hammerRun.report());

    // Kept open: the probe below runs in this same page, after the assertions
    // that have to see a clean one.
    probed = async () => {
        await page.evaluate(() => window.hammerRun.probe());
        return page.evaluate(() => window.hammerRun.report());
    };
}, 60_000);

let probed: () => Promise<PageReport>;

afterAll(async () => {
    await origin?.close();
    await browser?.close();
});

describe("every component, in a browser enforcing the policy", () => {
    it("installs the default policy the application's CSP permits", () => {
        // `unavailable` would mean the browser has no Trusted Types and the run
        // asserted nothing; `refused` would mean the CSP did not name the
        // policy, which is the deployment mistake `docs/03-deployment.md` §3
        // exists to prevent. Either is a failed run rather than a passed one.
        expect(report.trustedTypes).toBe("installed");
    });

    it("mounts all of them", () => {
        // The registry is the reference consumer's own screen, and `published()`
        // is what the entry points export. A component nobody mounted would be
        // uncovered here the same way it is uncovered in the DOM suite.
        expect(report.mounted.length).toBe(published().length);
    });

    it("reports no policy violation at all", () => {
        expect(report.violations).toEqual([]);
    });

    it("throws nothing while rendering", () => {
        // A Trusted Types refusal is a TypeError at the sink as well as a
        // report. A page that swallowed the report would still land here.
        expect(report.errors).toEqual([]);
    });

    // Without this, the two above report clean for the wrong reason: a listener
    // that was never wired, a policy that was never enforced, a page that never
    // loaded the bundle at all.
    it("reports a violation when one is committed, so the absence is an absence", async () => {
        const after = await probed();

        expect(after.violations).toContain("trusted-types:threw");
        expect(after.violations.some((one) => one.startsWith("style-src"))).toBe(true);
    });
});
