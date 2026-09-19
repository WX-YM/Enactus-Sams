// The page the Trusted Types run loads: every component this library ships,
// mounted under a policy Chromium is actually enforcing.
//
// The order below is the whole design of the file.
//
//   1. The listeners go on FIRST. A violation raised while the reporter was
//      still being constructed is a violation nobody counts, and this run's
//      entire output is a count.
//   2. The default Trusted Types policy is installed, because that is what an
//      application does (`docs/03-deployment.md` §3) and because a run that
//      skipped it would be asserting the page rather than the library: the
//      policy REFUSES, so any sink hammer reached would throw here.
//   3. Everything mounts.
//   4. `probe()` is left for the run to call LAST, after the clean assertion.
//      It commits two deliberate violations, which is what makes the empty list
//      above an absence rather than a reporter that never worked. A check that
//      cannot fail reports clean for the wrong reason — the lesson
//      `tests/dom/csp.test.ts` learned when a trap on `Document.prototype`
//      turned out to be shadowed.

import type { TrustedTypesScope } from "hammer/dom";
import { installTrustedTypes } from "hammer/dom";

import { mountEach } from "../../dom/registry.js";
import type { PageReport } from "../harness.js";

declare global {
    interface Window {
        hammerRun: {
            report: () => PageReport;
            probe: () => void;
        };
    }
}

const violations: string[] = [];
const errors: string[] = [];

document.addEventListener("securitypolicyviolation", (event: SecurityPolicyViolationEvent) => {
    violations.push(`${event.effectiveDirective || event.violatedDirective}:${event.blockedURI}`);
});

// A Trusted Types refusal arrives as a thrown TypeError at the sink as well as
// as a violation report, and the two are not redundant: a sink reached inside a
// `try` would be reported and swallowed, and a sink reached in a page with no
// reporting would throw and be seen here.
window.addEventListener("error", (event: ErrorEvent) => {
    errors.push(String(event.message));
});
window.addEventListener("unhandledrejection", (event: PromiseRejectionEvent) => {
    errors.push(String(event.reason));
});

// `window` is the scope the platform puts `trustedTypes` on. It is passed as a
// parameter rather than read off a global inside the library, which is the same
// injection every other seam here uses — and the cast is because TypeScript's
// DOM library does not declare Trusted Types at all yet.
const installed = installTrustedTypes(window as unknown as TrustedTypesScope);

const mounted = mountEach(document);

window.hammerRun = {
    report: () => ({
        violations: [...violations],
        errors: [...errors],
        mounted: mounted.map((one) => one.name),
        trustedTypes: installed.ok ? "installed" : installed.error,
    }),

    probe: () => {
        const target = document.createElement("div");
        document.body.append(target);

        // A markup sink under `require-trusted-types-for 'script'`. The default
        // policy installed above refuses, so this throws as well as reporting —
        // caught, because an uncaught one would land in `errors` and the run
        // could not tell a deliberate probe from a real defect.
        try {
            target.innerHTML = "<b>x</b>"; // ban-exempt: driving the tripwire
        } catch {
            violations.push("trusted-types:threw");
        }

        // An inline style attribute under `style-src 'none'`. Not blocked by
        // Trusted Types at all — a second directive, reported through the same
        // listener, so the run knows the reporter sees more than one kind.
        target.setAttribute("style", "color:red");

        target.remove();
    },
};
