// The node-side driver for every DOM and React test file that needs a real
// document (docs/15-tasks.md Phase 8 B2, CLAUDE.md §12), replacing the
// `happy-dom` stopgap.
//
// `grep -rl happy_dom_env tests` names twenty files. Nineteen of them import
// `tests/support/happy_dom_env.js` as their first line and are driven from
// here; the twentieth, `tests/core/environment.test.ts`, only MENTIONS the
// specifier in a comment warning against exactly this happening by accident,
// and stays a plain `node:test` file proving that warning still holds.
//
// --- design -------------------------------------------------------------
//
// One Chromium, launched once (`tests/browser/cdp.ts`, the same client
// `tests/browser/harness.ts` uses for the live and Trusted Types runs). One
// HTTP server for the whole run, serving each file's bundle at its own path
// (`/f<index>/`) so two files never share a URL a browser cache could confuse,
// and falling back to an on-demand TypeScript-to-ESM transpile of any other
// repository-relative path — the one thing `tests/dom/sanitized.test.ts` needs:
// its cache-busting `import(...?fresh=N)` re-import of its own module under
// test resolves, in a real browser exactly as in Node, to a fresh module
// instance per distinct URL, and that URL has to be servable.
//
// Each file gets its own browser CONTEXT (`CLAUDE.md` §3.3's "nothing outlives
// what created it", applied per file rather than per test): a leaked global, an
// installed Trusted Types policy, a mounted node left behind by a failing
// `close()` — none of it survives into the next file's page.
//
// The whole run happens through top-level await, BEFORE any `describe`/`it` is
// registered, because a browser result has to exist before node:test can be
// told about it — recorded here in one flat list, then replayed as nested
// `describe`/`it` calls below so a failure's node:test output names the exact
// file, the exact `describe` path, and the exact case, the same way it would if
// `node --test` had run the file directly.
//
// A file whose page throws before registering a single test, or that reports
// zero, is a failure rather than a silent pass: the same rule
// `tests/live/harness.ts` and `tests/browser/harness.ts` give for a missing
// server or a missing browser applies here to a missing DOCUMENT — a suite that
// can pass with nothing behind it is a suite whose green means nothing.

import { existsSync, readFileSync } from "node:fs";
import { createServer } from "node:http";
import type { IncomingMessage, ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import type { Plugin } from "esbuild";
import { build, transformSync } from "esbuild";

import { describe, it } from "../support/test.js";

import type { Browser, BrowserContext } from "../browser/cdp.js";
import { entryAliases, kContentSecurityPolicy, launch } from "../browser/harness.js";
import type { CaseResult } from "../support/browser_registry.js";

const kRoot = fileURLToPath(new URL("../../", import.meta.url));
const kSupportDir = fileURLToPath(new URL("../support/", import.meta.url));
const kTestTsPath = `${kSupportDir}test.ts`;
const kTestBrowserTsPath = `${kSupportDir}test_browser.ts`;
const kBrowserRegistryPath = `${kSupportDir}browser_registry.ts`;

// The twenty files. Named explicitly rather than discovered by grepping their
// contents at run time: this list IS the seam between "runs under node:test
// directly" and "runs through this driver", and a seam that greps for itself on
// every run is a seam that silently changes shape when a comment merely
// mentions the string it is looking for — which is exactly the false positive
// `tests/core/environment.test.ts` above is written to catch.
const kFiles = [
    "tests/dom/accessibility.test.ts",
    "tests/dom/auth.test.ts",
    "tests/dom/bell.test.ts",
    "tests/dom/consent.test.ts",
    "tests/dom/csp.test.ts",
    "tests/dom/direction.test.ts",
    "tests/dom/error.test.ts",
    "tests/dom/form.test.ts",
    "tests/dom/image.test.ts",
    "tests/dom/inbox.test.ts",
    "tests/dom/mount.test.ts",
    "tests/dom/pager.test.ts",
    "tests/dom/sanitized.test.ts",
    "tests/dom/section.test.ts",
    "tests/dom/testapp.test.ts",
    "tests/dom/upload.test.ts",
    "tests/react/hooks.test.tsx",
    "tests/react/testapp.test.tsx",
    "tests/chart/chart.test.ts",
    "tests/edit/editor.test.ts",
    "tests/chat-e2ee/idb.test.ts",
] as const;

type PageReport = { readonly results: readonly CaseResult[] } | { readonly crashed: string };

// --- the esbuild plugin: hammer/* and the .js→.ts/.tsx fallback -------------
//
// Two rules, both mirroring `tests/support/register.mjs`'s Node loader hook so
// the bundle resolves specifiers exactly as `node --test` already does:
//
//   1. a bare `hammer/*` specifier resolves to the entry point's SOURCE, the
//      same map `register.mjs` and `tests/browser/harness.ts` both derive from
//      `package.json`'s own `exports` rather than duplicating it a third time;
//   2. a relative specifier ending in `.js` whose literal file does not exist
//      resolves to the `.ts`/`.tsx` sibling that does — every source file in
//      this repository is imported by the extension its COMPILED output will
//      have, per `verbatimModuleSyntax`, not the one on disk.
//
// One case rule 2 would otherwise get right by accident and wrong on purpose:
// `"../support/test.js"` resolves to `test_browser.ts`, not `test.ts`. That is
// the whole of what makes a DOM test file written against the node backend run,
// unmodified, against this one instead.
function browserTestResolvePlugin(): Plugin {
    const bareAliases = entryAliases();

    return {
        name: "hammer-browser-test-resolve",
        setup(api) {
            api.onResolve({ filter: /.*/ }, (args) => {
                const aliased = bareAliases[args.path];
                if (aliased !== undefined) {
                    return { path: aliased };
                }

                if (!args.path.startsWith("./") && !args.path.startsWith("../")) {
                    return undefined;
                }
                if (!args.path.endsWith(".js")) {
                    return undefined;
                }

                const resolvedJs = join(args.resolveDir, args.path);
                const stem = resolvedJs.slice(0, -".js".length);

                if (`${stem}.ts` === kTestTsPath) {
                    return { path: kTestBrowserTsPath };
                }
                if (existsSync(resolvedJs)) {
                    return undefined;
                }
                for (const extension of [".ts", ".tsx"]) {
                    const candidate = `${stem}${extension}`;
                    if (existsSync(candidate)) {
                        return { path: candidate };
                    }
                }
                return undefined;
            });
        },
    };
}

// The whole bundle: a stdin entry that imports the target file for its
// registration side effect, catching anything it throws on the way — a bad
// import, a `describe()` callback that itself throws — as the "crashed"
// outcome, then hands the driver every case `browser_registry.ts` collected.
//
// The target is reached through a DYNAMIC `import()` rather than a static one
// specifically so a throw during its evaluation is a rejected promise this
// wrapper can catch, rather than an error the module graph never finishes
// loading and the page's `<script type=module>` fails on with nothing this
// process can observe through `Runtime.evaluate`.
async function bundleFile(absoluteTestPath: string): Promise<string> {
    const entryContents = [
        `import { run } from ${JSON.stringify(kBrowserRegistryPath)};`,
        "window.__hammerReport = async function () {",
        "    try {",
        `        await import(${JSON.stringify(absoluteTestPath)});`,
        "    } catch (error) {",
        "        return { crashed: error && error.stack ? String(error.stack) : String(error) };",
        "    }",
        "    return { results: await run() };",
        "};",
    ].join("\n");

    const result = await build({
        stdin: { contents: entryContents, resolveDir: kRoot, loader: "ts", sourcefile: "entry.ts" },
        bundle: true,
        format: "esm",
        target: "es2022",
        platform: "browser",
        jsx: "automatic",
        write: false,
        outdir: `${kRoot}dist/never-written`,
        charset: "utf8",
        logLevel: "silent",
        plugins: [browserTestResolvePlugin()],
    });
    return result.outputFiles.map((file) => file.text).join("");
}

// --- the on-demand module server ---------------------------------------------
//
// One route per bundled file (`/f<index>/` and `/f<index>/app.js`), and
// everything else falls through to a repo-relative TypeScript-to-ESM
// transpile — the same `.js`→`.ts`/`.tsx` fallback the bundler plugin applies,
// done here for the one thing bundling cannot serve: a specifier a test builds
// at RUN time (`sanitized.test.ts`'s `?fresh=` counter), which esbuild cannot
// see at bundle time and therefore leaves as a genuine runtime `import()` —
// resolved by the browser against this page's own URL, which is why every
// bundle is served at a single path segment: two levels of `../` from
// `/f<n>/app.js` lands back at this server's root exactly as it would from
// `/app.js`, so the specifier a test file wrote against its OWN position in the
// repository still resolves.
function pageHtml(index: number): string {
    return [
        "<!doctype html>",
        '<html lang="en"><head><meta charset="utf-8"><title>hammer</title></head>',
        `<body><script type="module" src="/f${index}/app.js"></script></body></html>`,
    ].join("");
}

function serveModule(pathname: string, response: ServerResponse): void {
    const relative = pathname.replace(/^\/+/, "");
    let filePath = join(kRoot, relative);

    if (filePath.endsWith(".js") && !existsSync(filePath)) {
        const stem = filePath.slice(0, -".js".length);
        const tsCandidate = `${stem}.ts`;
        const tsxCandidate = `${stem}.tsx`;
        if (existsSync(tsCandidate)) {
            filePath = tsCandidate;
        } else if (existsSync(tsxCandidate)) {
            filePath = tsxCandidate;
        }
    }

    if (!existsSync(filePath)) {
        response.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" }).end(`not found: ${relative}`);
        return;
    }

    const isTsx = filePath.endsWith(".tsx");
    const source = readFileSync(filePath, "utf8");
    const { code } = transformSync(source, {
        loader: isTsx ? "tsx" : "ts",
        format: "esm",
        jsx: "automatic",
        target: "es2022",
        sourcefile: filePath,
        sourcemap: "inline",
    });

    response.writeHead(200, {
        "Content-Type": "text/javascript; charset=utf-8",
        "Content-Security-Policy": kContentSecurityPolicy,
    });
    response.end(code);
}

type ServedFile = { readonly html: string; readonly script: string };

function makeServer(served: Map<number, ServedFile>) {
    return createServer((request: IncomingMessage, response: ServerResponse) => {
        const url = new URL(request.url ?? "/", "http://internal.invalid");
        const match = /^\/f(\d+)\/(app\.js)?$/.exec(url.pathname);

        if (match !== null) {
            const index = Number(match[1]);
            const entry = served.get(index);
            if (entry === undefined) {
                response.writeHead(404).end();
                return;
            }
            if (match[2] === "app.js") {
                response.writeHead(200, {
                    "Content-Type": "text/javascript; charset=utf-8",
                    "Content-Security-Policy": kContentSecurityPolicy,
                });
                response.end(entry.script);
            } else {
                response.writeHead(200, {
                    "Content-Type": "text/html; charset=utf-8",
                    "Content-Security-Policy": kContentSecurityPolicy,
                });
                response.end(entry.html);
            }
            return;
        }

        serveModule(url.pathname, response);
    });
}

// --- replaying a file's results as node:test cases ---------------------------

type ResultNode = {
    readonly children: Map<string, ResultNode>;
    readonly leaves: CaseResult[];
};

function buildTree(results: readonly CaseResult[]): ResultNode {
    const root: ResultNode = { children: new Map(), leaves: [] };
    for (const result of results) {
        let node = root;
        for (const segment of result.path) {
            let child = node.children.get(segment);
            if (child === undefined) {
                child = { children: new Map(), leaves: [] };
                node.children.set(segment, child);
            }
            node = child;
        }
        node.leaves.push(result);
    }
    return root;
}

function registerNode(node: ResultNode): void {
    for (const result of node.leaves) {
        it(result.name, () => {
            if (result.ok) {
                return;
            }
            const message = result.error?.message ?? "failed with no message";
            const stack = result.error?.stack;
            throw new Error(stack !== undefined ? `${message}\n${stack}` : message);
        });
    }
    for (const [name, child] of node.children) {
        describe(name, () => registerNode(child));
    }
}

// --- the run itself: top-level await, before any describe/it is registered --
//
// Wrapped in one try/finally around the browser's whole lifetime: if bundling,
// navigation or evaluation throws for reasons this loop's own try/catch does
// not already turn into a per-file "crashed" outcome — a plugin bug, the HTTP
// server refusing to bind — the browser and the server are still torn down
// rather than leaking a process and a temp directory past this module's own
// failure (the acceptance check `pgrep -f user-data-dir` after a run is
// checking for, `tests/browser/cdp.ts`'s own header).

let browser: Browser | undefined;
const perFile: { readonly file: string; readonly report: PageReport }[] = [];

try {
    browser = await launch();

    const served = new Map<number, ServedFile>();
    const server = makeServer(served);
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    const port = (server.address() as AddressInfo).port;
    const baseUrl = `http://127.0.0.1:${port}`;

    try {
        for (let index = 0; index < kFiles.length; index += 1) {
            const file = kFiles[index];
            if (file === undefined) {
                continue;
            }
            let context: BrowserContext | undefined;
            try {
                const script = await bundleFile(`${kRoot}${file}`);
                served.set(index, { html: pageHtml(index), script });

                context = await browser.newContext();
                const page = await context.newPage();
                await page.goto(`${baseUrl}/f${index}/`, { waitUntil: "load" });

                // A background target Chromium never activated does not
                // reliably move `document.activeElement` on `.focus()` or on
                // a real `.click()`'s own default focusing step — a gap
                // happy-dom never had an opinion on because it implements no
                // focus model at all. `bell.test.ts`'s "gives focus back to
                // what had it" is what caught this on the first real run.
                const cdp = await context.newCDPSession(page);
                await cdp.send("Page.bringToFront");

                const report = await page.evaluate<PageReport>(
                    () => (window as unknown as { __hammerReport: () => Promise<PageReport> }).__hammerReport(),
                );
                perFile.push({ file, report });
            } catch (error) {
                const message = error instanceof Error ? (error.stack ?? error.message) : String(error);
                perFile.push({ file, report: { crashed: message } });
            } finally {
                served.delete(index);
                await context?.close();
            }
        }
    } finally {
        await new Promise<void>((resolve, reject) => {
            server.close((closeError) => (closeError === undefined ? resolve() : reject(closeError)));
        });
    }
} finally {
    await browser?.close();
}

// --- registration: node:test finds out about all of this only now -----------

describe("the DOM and React suite, in a real browser", () => {
    for (const { file, report } of perFile) {
        describe(file, () => {
            if ("crashed" in report) {
                it("registers at least one test", () => {
                    throw new Error(`the page crashed before registering a test:\n${report.crashed}`);
                });
                return;
            }
            if (report.results.length === 0) {
                it("registers at least one test", () => {
                    throw new Error("the page loaded and registered zero tests");
                });
                return;
            }
            registerNode(buildTree(report.results));
        });
    }
});
