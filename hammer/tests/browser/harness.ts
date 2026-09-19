// A real browser, and the four properties that need one.
//
// Every other suite in this repository asserts hammer against a stand-in: a
// document happy-dom implements, a channel the test wrote, a clock the test
// advances. Four of phase 7's rows cannot be written that way, and the reason is
// the same each time — the mechanism under test IS the platform:
//
//   TRUSTED TYPES. A policy is enforced by the browser or it is not enforced.
//              happy-dom has none, so `tests/dom/csp.test.ts` installs tripwires
//              on every route to a sink and drives them; that asserts the code
//              takes no such route, which is the strongest thing a fake document
//              can say. Whether Chromium AGREES is a different claim.
//   TWO TABS.  `navigator.locks` and `BroadcastChannel` are per-origin and
//              shared between real tabs. Two store instances in one Node process
//              share whatever the test handed them.
//   FREEZE.    A frozen or discarded tab is a browser lifecycle event. It cannot
//              be simulated meaningfully, because what is being asserted is that
//              the timers really stopped.
//   3G.        Concurrency under real latency is not the shape it has under a
//              fake clock, and the queue's whole job is a shape.
//
// --- how to run it ----------------------------------------------------------
//
//     tools/run-live.sh --browser
//
// which starts `anvil_reference_server`, reads its URL and its two passwords off
// the server's own stdout, and runs the live suite and this one. The three rows
// that need a server fail rather than skip without `HAMMER_LIVE_ORIGIN`, for the
// reason `tests/live/harness.ts` gives: a suite that quietly passes with no
// server is a suite whose green means nothing. The Trusted Types run needs no
// server and is driven by `npm run test:browser` alone.
//
// One case is SKIPPED rather than failed, and the distinction matters: the
// `Retry-After` case needs a route whose rate limit can be reached deliberately,
// and the reference application installs no limiter. The server is there and
// cannot produce the answer, which is a different thing from the server being
// absent — it is a row in `docs/15-tasks.md` §Cross-repo.
//
// --- why this is not in `npm run check` -------------------------------------
//
// It needs a browser binary on the machine, which CI may not have and a
// contributor certainly may not. `npm run check` is what every commit passes;
// this is what a person runs against a browser they have.

import { existsSync, readFileSync } from "node:fs";
import { createServer } from "node:http";
import type { AddressInfo } from "node:net";
import { fileURLToPath } from "node:url";

import { build } from "esbuild";
import type { Browser, BrowserContext, CDPSession, Page } from "playwright-core";
import { chromium } from "playwright-core";

const kRoot = fileURLToPath(new URL("../../", import.meta.url));

// Where a Chromium lives on a machine that did not download one through
// playwright. `playwright-core` is the dependency rather than `playwright`
// precisely so nothing here installs a browser: a postinstall step that fetches
// a hundred megabytes is a dev dependency with opinions about the network.
const kCandidates = [
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
];

export function browserExecutable(): string {
    const named = process.env["HAMMER_BROWSER"];
    if (named !== undefined && named !== "") {
        if (!existsSync(named)) {
            throw new Error(`HAMMER_BROWSER points at ${named}, which does not exist.`);
        }
        return named;
    }
    for (const candidate of kCandidates) {
        if (existsSync(candidate)) {
            return candidate;
        }
    }
    throw new Error(
        "No Chromium was found. Set HAMMER_BROWSER to one, or install chromium; " +
            "see docs/16-test-plan.md §Phase 7 — browser.",
    );
}

export async function launch(): Promise<Browser> {
    return chromium.launch({ executablePath: browserExecutable(), headless: true });
}

// A missing origin is a FAILED run rather than a skipped one, the same way
// `tests/live/harness.ts` treats it and for the same reason.
export function liveOrigin(): string {
    const origin = process.env["HAMMER_LIVE_ORIGIN"];
    if (origin === undefined || origin === "") {
        throw new Error(
            "HAMMER_LIVE_ORIGIN is not set. This run needs a live anvil AND a browser; " +
                "see docs/16-test-plan.md §Phase 7 — browser.",
        );
    }
    return origin;
}

// The entry point specifiers, mapped to source, read out of `exports` rather
// than written down. It is the third place in this repository that would
// otherwise hold a copy of this map — `tsconfig.json`, `vitest.config.ts` — and
// a copy is a thing to forget when an entry point is added.
function entryAliases(): Record<string, string> {
    const pkg: unknown = JSON.parse(readFileSync(`${kRoot}package.json`, "utf8"));
    const exported = (pkg as { readonly exports?: Readonly<Record<string, string>> }).exports ?? {};
    const name = (pkg as { readonly name: string }).name;

    const alias: Record<string, string> = {};
    for (const [subpath, target] of Object.entries(exported)) {
        if (!target.startsWith("./dist/") || !target.endsWith(".js")) {
            continue;
        }
        const source = `${kRoot}src/${target.slice("./dist/".length, -".js".length)}.ts`;
        alias[subpath === "." ? name : `${name}/${subpath.slice("./".length)}`] = source;
    }
    return alias;
}

// The policy the application serves, as a HEADER rather than a `<meta>` tag.
//
// `require-trusted-types-for` is ignored in a meta tag by some engines and
// honoured in others, and a run whose enforcement depends on which one would be
// asserting the delivery mechanism. It is the policy `docs/03-deployment.md` §3
// publishes, narrowed to what one page needs.
//
// `style-src 'none'` is not decoration: hammer sets no inline style and ships no
// stylesheet, so anything that reaches for one is a violation the browser
// reports rather than a defect somebody notices in production.
const kPolicy = [
    "default-src 'none'",
    "script-src 'self'",
    "style-src 'none'",
    // The reference consumer's media origin. The request will not resolve, and
    // that is fine — a load failure is not a policy violation, and the claim
    // here is about the policy.
    "img-src 'self' https://media.test",
    "connect-src 'self'",
    "base-uri 'none'",
    "object-src 'none'",
    "form-action 'none'",
    "frame-ancestors 'none'",
    "require-trusted-types-for 'script'",
    // `hammer` is the policy `dom/sanitized.ts` creates for its one parse;
    // `default` is the refusing one `installTrustedTypes` installs. A page that
    // named only the second is the deployment that broke the section renderer,
    // and it is why this run exists.
    "trusted-types hammer default",
].join("; ");

export type Origin = {
    readonly url: string;
    readonly close: () => Promise<void>;
};

// A real origin over HTTP on the loopback address, which Chromium treats as a
// secure context — so Trusted Types is active, which it is not on a `file:` URL
// or on `about:blank`.
//
// The page carries no inline script and no inline style, because the policy it
// is serving would refuse both. That is the point: the harness has to satisfy
// the same rule the library does, or the run is measuring the harness.
export async function serveBundle(entry: string): Promise<Origin> {
    const built = await build({
        entryPoints: [fileURLToPath(new URL(entry, import.meta.url))],
        bundle: true,
        format: "esm",
        target: "es2022",
        platform: "browser",
        write: false,
        outdir: `${kRoot}dist/never-written`,
        alias: entryAliases(),
        charset: "utf8",
        logLevel: "silent",
    });
    const script = built.outputFiles.map((file) => file.text).join("");

    const page = [
        "<!doctype html>",
        '<html lang="en"><head><meta charset="utf-8"><title>hammer</title></head>',
        '<body><div id="root"></div><script type="module" src="/app.js"></script></body></html>',
    ].join("");

    const server = createServer((request, response) => {
        if (request.url === "/app.js") {
            response.writeHead(200, {
                "Content-Type": "text/javascript; charset=utf-8",
                "Content-Security-Policy": kPolicy,
            });
            response.end(script);
            return;
        }
        response.writeHead(200, {
            "Content-Type": "text/html; charset=utf-8",
            "Content-Security-Policy": kPolicy,
        });
        response.end(page);
    });

    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const port = (server.address() as AddressInfo).port;

    return {
        url: `http://127.0.0.1:${port}/`,
        close: () =>
            new Promise<void>((resolve, reject) => {
                server.close((error) => (error === undefined ? resolve() : reject(error)));
            }),
    };
}

// What the page publishes back to the run. Declared here rather than in the page
// so both halves are compiled against one shape.
export type PageReport = {
    readonly violations: readonly string[];
    readonly errors: readonly string[];
    readonly mounted: readonly string[];
    readonly trustedTypes: string;
};

// --- the runs that need anvil as well ---------------------------------------
//
// The three remaining phase 7 rows need a live server, and they need the page to
// be SAME-ORIGIN with it. That is not a convenience: hammer refuses to be
// constructed against a cross-site API because `SameSite=Lax` cookies are not
// sent there, so a run whose page came off a second port would be asserting an
// anonymous session.
//
// The bundle is fulfilled by the browser itself at a path ON the live origin —
// a real page on a real origin, holding real cookies, where the only part that
// is not the server's is the bytes of the harness page. Every request the
// library makes goes to anvil untouched: the interception is scoped to one
// prefix, and a run that intercepted an API call would be a stub, which is the
// thing this suite exists not to be.
//
// `anvil_reference_server` can serve a static directory at `/app` instead, and
// the choice between the two is deliberate. Interception needs no build step and
// no file written anywhere, and the property being bought — same-origin — is
// identical either way, because the browser decides what "origin" means from the
// URL and not from who produced the bytes.

const kMountPath = "/__hammer-browser-run/";

export type Tabs = {
    readonly origin: string;
    readonly context: BrowserContext;

    // N tabs of ONE session: one cookie jar, one lock manager, one broadcast
    // channel. Two contexts would be two browsers, which is the mistake this
    // function exists to make impossible to write by accident.
    readonly open: (count: number) => Promise<readonly Page[]>;

    readonly cdp: (page: Page) => Promise<CDPSession>;
    readonly close: () => Promise<void>;
};

export async function liveTabs(browser: Browser, entry = "./page/session.ts"): Promise<Tabs> {
    const origin = liveOrigin();

    const built = await build({
        entryPoints: [fileURLToPath(new URL(entry, import.meta.url))],
        bundle: true,
        format: "esm",
        target: "es2022",
        platform: "browser",
        write: false,
        outdir: `${kRoot}dist/never-written`,
        alias: entryAliases(),
        charset: "utf8",
        logLevel: "silent",
    });
    const script = built.outputFiles.map((file) => file.text).join("");

    const document = [
        "<!doctype html>",
        '<html lang="en"><head><meta charset="utf-8"><title>hammer</title></head>',
        `<body><script type="module" src="${kMountPath}app.js"></script></body></html>`,
    ].join("");

    // A self-signed certificate is what a local anvil serves. Accepting it here
    // is accepting the DEVELOPMENT server's certificate, and it changes nothing
    // about cookie scoping: `__Host-` still requires `Secure`, which https over
    // a local certificate still is.
    const context = await browser.newContext({ ignoreHTTPSErrors: true });

    await context.route(`${origin}${kMountPath}**`, async (route) => {
        const isScript = route.request().url().endsWith("app.js");
        await route.fulfill({
            status: 200,
            contentType: isScript ? "text/javascript; charset=utf-8" : "text/html; charset=utf-8",
            body: isScript ? script : document,
        });
    });

    return {
        origin,
        context,
        open: async (count) => {
            const pages: Page[] = [];
            for (let at = 0; at < count; at++) {
                const page = await context.newPage();
                await page.goto(`${origin}${kMountPath}`, { waitUntil: "load" });
                pages.push(page);
            }
            return pages;
        },
        cdp: (page) => context.newCDPSession(page),
        close: () => context.close(),
    };
}
