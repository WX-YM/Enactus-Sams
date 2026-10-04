// A minimal Chrome DevTools Protocol client, replacing `playwright-core`
// (`CLAUDE.md` §12, `docs/15-tasks.md` Phase 8 B3).
//
// --- why not playwright --------------------------------------------------
//
// `playwright-core` is zero-dependency and well-owned, but it is an entire
// browser-automation protocol client — every domain, every browser engine's
// quirks, a test-generator, a trace viewer — for the sixteen calls this
// suite actually makes. A dependency is reviewed once at the version it is
// pinned to; the review this repository can actually stand behind is of
// code it wrote, not of a surface a hundred times the size of what is used.
// The three browser properties `tests/browser/harness.ts` documents — a
// policy enforced by a real browser, two tabs sharing one lock manager, a
// tab the OS actually freezes — need a real Chromium and the DevTools wire
// protocol underneath it, not the abstraction playwright builds on top.
//
// --- why a pipe and not a port -------------------------------------------
//
// Chromium offers two transports for the same protocol: `--remote-debugging-port`,
// which opens a TCP port and a `ws://` endpoint anything on the machine can
// connect to (the same surface CVE reports about "attach to a running
// Chrome" abuse), or `--remote-debugging-pipe`, which speaks the identical
// JSON messages over a pair of inherited file descriptors that exist only
// for the lifetime of this one child process and are never a listening
// socket at all. Nothing here needs a second process to attach — this
// module launches the browser and drives it in the same breath — so the
// port buys nothing and costs an attack surface. NUL-delimited JSON on fds
// 3 (this process writes, Chromium reads) and 4 (Chromium writes, this
// process reads) is a few dozen lines more than parsing a `ws://` URL out
// of stderr, which is the "not much harder" the task standard asks for.
//
// --- the protocol ----------------------------------------------------------
//
// Flattened sessions throughout: one `Target.attachToTarget({flatten: true})`
// per page, after which every command for that page carries its `sessionId`
// and every event arrives tagged with the same. `Connection` below is the
// whole of the transport — one pending-call table keyed by message id, one
// event-listener table keyed by `sessionId:method`, and every entry in
// either table is rejected or dropped the moment the pipe closes, so a
// crashed or killed Chromium fails every in-flight call rather than hanging
// a `beforeAll` forever (`CLAUDE.md` §4).
//
// Scope is deliberately narrow: this implements exactly the CDP calls
// `tests/browser/harness.ts` and the three suites need today, not the
// protocol. A new call site is a new few lines here, not a new dependency.

import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Readable, Writable } from "node:stream";

// --- the wire: NUL-delimited JSON over two inherited file descriptors -----

type PendingCall = {
    readonly resolve: (result: unknown) => void;
    readonly reject: (error: Error) => void;
};

type EventListener = (params: unknown) => void;

const kDefaultCallTimeoutMs = 30_000;

// The one place a CDP payload is narrowed from `unknown`, because every
// message on this wire is JSON somebody else wrote. Every caller past this
// point works with plain property lookups on a `Record`, never a cast
// straight off `JSON.parse`.
function asRecord(value: unknown): Record<string, unknown> | undefined {
    return typeof value === "object" && value !== null ? (value as Record<string, unknown>) : undefined;
}

function asString(value: unknown): string | undefined {
    return typeof value === "string" ? value : undefined;
}

class Connection {
    private nextId = 1;
    private readonly pending = new Map<number, PendingCall>();
    private readonly listeners = new Map<string, Set<EventListener>>();
    private buffer = "";
    private closed: Error | undefined;

    constructor(
        private readonly writeChannel: Writable,
        readChannel: Readable,
    ) {
        readChannel.on("data", (chunk: Buffer) => this.onData(chunk));
        // A closed pipe and a socket error are the same fact from here: the
        // browser is gone. Every pending call is rejected rather than left to
        // time out one by one — `CLAUDE.md` §4, nothing may hang forever.
        readChannel.on("close", () => this.fail(new Error("the DevTools pipe closed")));
        readChannel.on("error", (error: Error) => this.fail(error));
    }

    send(
        method: string,
        params: Readonly<Record<string, unknown>> = {},
        sessionId?: string,
        timeoutMs = kDefaultCallTimeoutMs,
    ): Promise<unknown> {
        if (this.closed !== undefined) {
            return Promise.reject(this.closed);
        }

        const id = this.nextId;
        this.nextId += 1;
        const message: Record<string, unknown> =
            sessionId === undefined ? { id, method, params } : { id, method, params, sessionId };

        return new Promise<unknown>((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error(`CDP call "${method}" timed out after ${timeoutMs}ms`));
            }, timeoutMs);

            this.pending.set(id, {
                resolve: (result) => {
                    clearTimeout(timer);
                    resolve(result);
                },
                reject: (error) => {
                    clearTimeout(timer);
                    reject(error);
                },
            });

            this.writeChannel.write(`${JSON.stringify(message)}\0`);
        });
    }

    // Returns its own unsubscribe (`CLAUDE.md` §3.3): every registration here
    // is either paired with one in the same function or lives for the
    // connection's whole life, and both are deliberate rather than a leak
    // nobody wrote down.
    on(sessionId: string | undefined, method: string, listener: EventListener): () => void {
        const key = `${sessionId ?? ""}:${method}`;
        let set = this.listeners.get(key);
        if (set === undefined) {
            set = new Set();
            this.listeners.set(key, set);
        }
        const bound = set;
        bound.add(listener);
        return () => bound.delete(listener);
    }

    // Waits for exactly one occurrence of an event, bounded by its own
    // timeout regardless of what the connection does — a browser that never
    // sends `Page.loadEventFired` fails this call rather than hanging the
    // suite (`CLAUDE.md` §4).
    once(sessionId: string | undefined, method: string, timeoutMs = kDefaultCallTimeoutMs): Promise<unknown> {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                off();
                reject(new Error(`timed out after ${timeoutMs}ms waiting for "${method}"`));
            }, timeoutMs);
            const off = this.on(sessionId, method, (params) => {
                clearTimeout(timer);
                off();
                resolve(params);
            });
        });
    }

    private onData(chunk: Buffer): void {
        this.buffer += chunk.toString("utf8");
        let index = this.buffer.indexOf("\0");
        while (index !== -1) {
            const raw = this.buffer.slice(0, index);
            this.buffer = this.buffer.slice(index + 1);
            if (raw.length > 0) {
                this.dispatch(JSON.parse(raw) as unknown);
            }
            index = this.buffer.indexOf("\0");
        }
    }

    private dispatch(message: unknown): void {
        const record = asRecord(message);
        if (record === undefined) {
            return;
        }

        const id = record["id"];
        if (typeof id === "number") {
            const pending = this.pending.get(id);
            if (pending === undefined) {
                return;
            }
            this.pending.delete(id);
            const error = asRecord(record["error"]);
            if (error !== undefined) {
                pending.reject(new Error(asString(error["message"]) ?? `CDP error responding to id ${id}`));
            } else {
                pending.resolve(record["result"]);
            }
            return;
        }

        const method = asString(record["method"]);
        if (method === undefined) {
            return;
        }
        const sessionId = asString(record["sessionId"]);
        const set = this.listeners.get(`${sessionId ?? ""}:${method}`);
        if (set === undefined) {
            return;
        }
        for (const listener of set) {
            listener(record["params"]);
        }
    }

    private fail(error: Error): void {
        if (this.closed !== undefined) {
            return;
        }
        this.closed = error;
        for (const pending of this.pending.values()) {
            pending.reject(error);
        }
        this.pending.clear();
        this.listeners.clear();
    }
}

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

// --- the public surface: exactly what tests/browser/*.ts calls today ------

export type Cookie = {
    readonly name: string;
    readonly value: string;
    readonly domain: string;
    readonly path: string;
};

export type CookieFilter = {
    readonly name?: string;
};

export type NewContextOptions = {
    readonly ignoreHTTPSErrors?: boolean;
};

export type WaitUntil = "load" | "networkidle";

export type NavigateOptions = {
    readonly waitUntil?: WaitUntil;
};

export type FulfillResponse = {
    readonly status: number;
    readonly contentType: string;
    readonly body: string;
};

export type Route = {
    request(): { url(): string };
    fulfill(response: FulfillResponse): Promise<void>;
};

export type RouteHandler = (route: Route) => void | Promise<void>;

export type PageEventName = "request" | "requestfinished" | "requestfailed";

export type CDPSession = {
    send(method: string, params?: Readonly<Record<string, unknown>>): Promise<unknown>;
};

export type Page = {
    goto(url: string, options?: NavigateOptions): Promise<void>;
    reload(options?: NavigateOptions): Promise<void>;
    evaluate<Result>(fn: () => Result | Promise<Result>): Promise<Result>;
    evaluate<Result, Arg>(fn: (arg: Arg) => Result | Promise<Result>, arg: Arg): Promise<Result>;
    on(event: PageEventName, listener: () => void): void;
    waitForTimeout(ms: number): Promise<void>;
    waitForFunction(fn: () => boolean, arg: undefined, options: { readonly timeout: number }): Promise<void>;
};

export type BrowserContext = {
    newPage(): Promise<Page>;
    cookies(): Promise<readonly Cookie[]>;
    clearCookies(filter?: CookieFilter): Promise<void>;
    route(urlPattern: string, handler: RouteHandler): Promise<void>;
    newCDPSession(page: Page): Promise<CDPSession>;
    close(): Promise<void>;
};

export type Browser = {
    newPage(): Promise<Page>;
    newContext(options?: NewContextOptions): Promise<BrowserContext>;
    close(): Promise<void>;
};

// --- Runtime.evaluate: the one place a function crosses into the page -----
//
// `fn.toString()` is the whole trick: every call site in this suite passes a
// closure over nothing but its own parameter (`window`, a login/password
// pair, a count), so re-parsing its source text inside the page and calling
// it with a JSON-serialized argument reproduces exactly what playwright's
// own `page.evaluate` does under the hood. `returnByValue: true` asks CDP
// itself to do the JSON serialisation of the result; `awaitPromise: true` is
// a no-op when the expression is not a promise and the whole point when it
// is.
async function runEvaluate(connection: Connection, sessionId: string, expression: string): Promise<unknown> {
    const response = asRecord(
        await connection.send(
            "Runtime.evaluate",
            { expression, awaitPromise: true, returnByValue: true },
            sessionId,
        ),
    );
    if (response === undefined) {
        throw new Error("Runtime.evaluate returned an unexpected shape");
    }

    const exceptionDetails = asRecord(response["exceptionDetails"]);
    if (exceptionDetails !== undefined) {
        const exception = asRecord(exceptionDetails["exception"]);
        const text =
            (exception !== undefined ? asString(exception["description"]) : undefined) ??
            asString(exceptionDetails["text"]) ??
            "Runtime.evaluate threw";
        throw new Error(text);
    }

    const result = asRecord(response["result"]);
    if (result === undefined) {
        throw new Error("Runtime.evaluate returned no result");
    }
    return result["type"] === "undefined" ? undefined : result["value"];
}

function evaluateExpression(source: string, args: readonly unknown[]): string {
    return args.length > 0 ? `(${source})(${JSON.stringify(args[0])})` : `(${source})()`;
}

const kPageEventMethods: Readonly<Record<PageEventName, string>> = {
    request: "Network.requestWillBeSent",
    requestfinished: "Network.loadingFinished",
    requestfailed: "Network.loadingFailed",
};

// `page.goto(url, {waitUntil: "networkidle"})` has no single CDP event: it is
// playwright's own definition (no request in flight for a quiet window)
// re-implemented against the `Network.requestWillBeSent` /
// `Network.loadingFinished` / `Network.loadingFailed` triple every `Page`
// already tracks for its own `.on("request", ...)` listeners.
async function waitForNetworkIdle(
    inFlight: ReadonlySet<string>,
    timeoutMs = 30_000,
    idleMs = 500,
): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
        if (inFlight.size === 0) {
            await sleep(idleMs);
            if (inFlight.size === 0) {
                return;
            }
        } else {
            await sleep(50);
        }
        if (Date.now() > deadline) {
            throw new Error(`timed out after ${timeoutMs}ms waiting for the network to go idle`);
        }
    }
}

class PageImpl implements Page {
    readonly sessionId: string;
    private readonly connection: Connection;
    private readonly inFlightRequests = new Set<string>();

    constructor(connection: Connection, sessionId: string) {
        this.connection = connection;
        this.sessionId = sessionId;

        this.connection.on(sessionId, "Network.requestWillBeSent", (params) => {
            const requestId = asString(asRecord(params)?.["requestId"]);
            if (requestId !== undefined) {
                this.inFlightRequests.add(requestId);
            }
        });
        const drop = (params: unknown): void => {
            const requestId = asString(asRecord(params)?.["requestId"]);
            if (requestId !== undefined) {
                this.inFlightRequests.delete(requestId);
            }
        };
        this.connection.on(sessionId, "Network.loadingFinished", drop);
        this.connection.on(sessionId, "Network.loadingFailed", drop);
    }

    async goto(url: string, options: NavigateOptions = {}): Promise<void> {
        const loaded = this.connection.once(this.sessionId, "Page.loadEventFired");
        const response = asRecord(await this.connection.send("Page.navigate", { url }, this.sessionId));
        const errorText = response !== undefined ? asString(response["errorText"]) : undefined;
        if (errorText !== undefined) {
            throw new Error(`navigation to ${url} failed: ${errorText}`);
        }
        await loaded;
        if (options.waitUntil === "networkidle") {
            await waitForNetworkIdle(this.inFlightRequests);
        }
    }

    async reload(options: NavigateOptions = {}): Promise<void> {
        const loaded = this.connection.once(this.sessionId, "Page.loadEventFired");
        await this.connection.send("Page.reload", {}, this.sessionId);
        await loaded;
        if (options.waitUntil === "networkidle") {
            await waitForNetworkIdle(this.inFlightRequests);
        }
    }

    evaluate<Result>(fn: () => Result | Promise<Result>): Promise<Result>;
    evaluate<Result, Arg>(fn: (arg: Arg) => Result | Promise<Result>, arg: Arg): Promise<Result>;
    async evaluate(fn: (...args: readonly unknown[]) => unknown, ...args: readonly unknown[]): Promise<unknown> {
        return runEvaluate(this.connection, this.sessionId, evaluateExpression(fn.toString(), args));
    }

    on(event: PageEventName, listener: () => void): void {
        this.connection.on(this.sessionId, kPageEventMethods[event], () => listener());
    }

    async waitForTimeout(ms: number): Promise<void> {
        await sleep(ms);
    }

    async waitForFunction(
        fn: () => boolean,
        _arg: undefined,
        options: { readonly timeout: number },
    ): Promise<void> {
        const source = fn.toString();
        const deadline = Date.now() + options.timeout;
        for (;;) {
            const value = await runEvaluate(this.connection, this.sessionId, `(${source})()`);
            if (value === true) {
                return;
            }
            if (Date.now() > deadline) {
                throw new Error(`timed out after ${options.timeout}ms waiting for the page function`);
            }
            await sleep(100);
        }
    }
}

type RegisteredRoute = {
    readonly pattern: RegExp;
    readonly rawPattern: string;
    readonly handler: RouteHandler;
};

// Playwright's glob (`*` = any run of characters) reduced to what
// `tests/browser/harness.ts` actually writes: one trailing `**`. Consecutive
// wildcards collapsing to one `.*` is a correctness non-issue — `.*.*`
// matches exactly the same strings `.*` does — and CDP's own `Fetch.enable`
// pattern gets the ORIGINAL glob text unchanged; this regex only decides
// which registered handler a paused request is dispatched to once Chromium
// has already decided to pause it.
function globToRegExp(pattern: string): RegExp {
    const escaped = pattern.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*").replace(/\?/g, ".");
    return new RegExp(`^${escaped}$`);
}

async function handleRequestPaused(
    connection: Connection,
    sessionId: string,
    routes: readonly RegisteredRoute[],
    params: unknown,
): Promise<void> {
    const record = asRecord(params);
    const requestId = asString(record?.["requestId"]);
    const url = asString(asRecord(record?.["request"])?.["url"]);
    if (requestId === undefined || url === undefined) {
        return;
    }

    const matched = routes.find((route) => route.pattern.test(url));
    if (matched === undefined) {
        await connection.send("Fetch.continueRequest", { requestId }, sessionId);
        return;
    }

    const route: Route = {
        request: () => ({ url: () => url }),
        fulfill: async (response) => {
            await connection.send(
                "Fetch.fulfillRequest",
                {
                    requestId,
                    responseCode: response.status,
                    responseHeaders: [{ name: "Content-Type", value: response.contentType }],
                    body: Buffer.from(response.body, "utf8").toString("base64"),
                },
                sessionId,
            );
        },
    };

    await matched.handler(route);
}

async function attachRoutes(connection: Connection, sessionId: string, routes: readonly RegisteredRoute[]): Promise<void> {
    await connection.send(
        "Fetch.enable",
        { patterns: routes.map((route) => ({ urlPattern: route.rawPattern })) },
        sessionId,
    );

    connection.on(sessionId, "Fetch.requestPaused", (params) => {
        // Every task body catches (`CLAUDE.md` §4): this runs from an event
        // callback nothing awaits, so a route handler that throws must not
        // become an unhandled rejection — it would abort the whole test
        // process over one bad fixture rather than failing the one test.
        handleRequestPaused(connection, sessionId, routes, params).catch(() => {
            connection.send("Fetch.failRequest", { requestId: asRecord(params)?.["requestId"], errorReason: "Failed" }, sessionId).catch(() => undefined);
        });
    });
}

async function createPage(
    connection: Connection,
    browserContextId: string | undefined,
    routes: readonly RegisteredRoute[],
    ignoreHTTPSErrors: boolean,
): Promise<PageImpl> {
    const created = asRecord(
        await connection.send("Target.createTarget", {
            url: "about:blank",
            // `newWindow: true` is load-bearing and not a style choice: on
            // this Chromium build, creating a target inside a FRESH
            // `Target.createBrowserContext` without it fails with "Failed to
            // open new tab - no browser is open" (verified against
            // 153.0.8010.52). Passing it unconditionally, including for the
            // default context, keeps one code path rather than two untested
            // ones.
            newWindow: true,
            ...(browserContextId === undefined ? {} : { browserContextId }),
        }),
    );
    const targetId = asString(created?.["targetId"]);
    if (targetId === undefined) {
        throw new Error("Target.createTarget did not return a targetId");
    }

    const attached = asRecord(await connection.send("Target.attachToTarget", { targetId, flatten: true }));
    const sessionId = asString(attached?.["sessionId"]);
    if (sessionId === undefined) {
        throw new Error("Target.attachToTarget did not return a sessionId");
    }

    await Promise.all([
        connection.send("Page.enable", {}, sessionId),
        connection.send("Runtime.enable", {}, sessionId),
        connection.send("Network.enable", {}, sessionId),
    ]);

    if (ignoreHTTPSErrors) {
        await connection.send("Security.enable", {}, sessionId);
        await connection.send("Security.setIgnoreCertificateErrors", { ignore: true }, sessionId);
    }

    const page = new PageImpl(connection, sessionId);

    if (routes.length > 0) {
        await attachRoutes(connection, sessionId, routes);
    }

    return page;
}

function toCookie(value: unknown): Cookie {
    const record = asRecord(value);
    return {
        name: asString(record?.["name"]) ?? "",
        value: asString(record?.["value"]) ?? "",
        domain: asString(record?.["domain"]) ?? "",
        path: asString(record?.["path"]) ?? "",
    };
}

class ContextImpl implements BrowserContext {
    private readonly routes: RegisteredRoute[] = [];
    private readonly pages: PageImpl[] = [];

    constructor(
        private readonly connection: Connection,
        private readonly browserContextId: string,
        private readonly ignoreHTTPSErrors: boolean,
    ) {}

    async newPage(): Promise<Page> {
        const page = await createPage(this.connection, this.browserContextId, this.routes, this.ignoreHTTPSErrors);
        this.pages.push(page);
        return page;
    }

    async route(urlPattern: string, handler: RouteHandler): Promise<void> {
        this.routes.push({ pattern: globToRegExp(urlPattern), rawPattern: urlPattern, handler });
    }

    async cookies(): Promise<readonly Cookie[]> {
        const response = asRecord(
            await this.connection.send("Storage.getCookies", { browserContextId: this.browserContextId }),
        );
        const cookies = response?.["cookies"];
        return Array.isArray(cookies) ? cookies.map(toCookie) : [];
    }

    async clearCookies(filter: CookieFilter = {}): Promise<void> {
        if (filter.name === undefined) {
            await this.connection.send("Storage.clearCookies", { browserContextId: this.browserContextId });
            return;
        }

        // `Storage.setCookies` is additive — writing back "every cookie
        // except this one" leaves the excluded one exactly as it was, which
        // was verified by trying it before writing this comment. The actual
        // deletion primitive is `Network.deleteCookies`, and it is a
        // per-TARGET command rather than a per-context one, so it has to run
        // over an already-open page's session — which every caller of this
        // method has, because it deletes a cookie mid-run rather than before
        // any tab exists.
        const existing = await this.cookies();
        const target = existing.find((cookie) => cookie.name === filter.name);
        if (target === undefined) {
            return;
        }

        const page = this.pages[0];
        if (page === undefined) {
            throw new Error("clearCookies({name}) needs at least one open page to issue Network.deleteCookies on");
        }

        await this.connection.send(
            "Network.deleteCookies",
            { name: target.name, domain: target.domain, path: target.path },
            page.sessionId,
        );
    }

    async newCDPSession(page: Page): Promise<CDPSession> {
        if (!(page instanceof PageImpl)) {
            throw new Error("newCDPSession needs a Page created by this module");
        }
        const sessionId = page.sessionId;
        return {
            send: (method, params = {}) => this.connection.send(method, params, sessionId),
        };
    }

    async close(): Promise<void> {
        // Disposing the browser context tears down every target it holds.
        // Failures here are swallowed rather than thrown: `browser.close()`
        // always follows a context close in this suite, and it terminates
        // the whole process regardless of whether this call raced it.
        await this.connection.send("Target.disposeBrowserContext", { browserContextId: this.browserContextId }).catch(() => undefined);
    }
}

function waitForExit(child: ChildProcess, timeoutMs: number): Promise<void> {
    if (child.exitCode !== null || child.signalCode !== null) {
        return Promise.resolve();
    }
    return new Promise((resolve) => {
        const timer = setTimeout(() => {
            // The graceful `Browser.close` did not bring the process down in
            // time. SIGKILL rather than SIGTERM: this is a headless browser
            // with no state worth flushing, and the acceptance check for this
            // module is that NOTHING is left running after a failing run.
            child.kill("SIGKILL");
            resolve();
        }, timeoutMs);
        child.once("exit", () => {
            clearTimeout(timer);
            resolve();
        });
    });
}

class BrowserImpl implements Browser {
    constructor(
        private readonly connection: Connection,
        private readonly child: ChildProcess,
        private readonly userDataDir: string,
    ) {}

    async newPage(): Promise<Page> {
        return createPage(this.connection, undefined, [], false);
    }

    async newContext(options: NewContextOptions = {}): Promise<BrowserContext> {
        const created = asRecord(
            await this.connection.send("Target.createBrowserContext", { disposeOnDetach: true }),
        );
        const browserContextId = asString(created?.["browserContextId"]);
        if (browserContextId === undefined) {
            throw new Error("Target.createBrowserContext did not return a browserContextId");
        }
        return new ContextImpl(this.connection, browserContextId, options.ignoreHTTPSErrors ?? false);
    }

    async close(): Promise<void> {
        await this.connection.send("Browser.close", {}, undefined, 5_000).catch(() => undefined);
        await waitForExit(this.child, 5_000);
        // The one thing this whole launcher exists to guarantee: no
        // `--user-data-dir` survives the run it was created for, whether the
        // run passed or the browser had to be killed.
        await rm(this.userDataDir, { recursive: true, force: true });
    }
}

export type LaunchOptions = {
    readonly executablePath: string;
};

const kChromiumArgs = [
    "--headless=new",
    "--remote-debugging-pipe",
    "--no-first-run",
    "--no-default-browser-check",
    "--disable-gpu",
    "--disable-dev-shm-usage",
    "--disable-extensions",
    "--disable-component-extensions-with-background-pages",
    "--disable-background-networking",
    "--disable-sync",
    "--mute-audio",
    "about:blank",
];

// Launches Chromium with a fresh, throwaway profile and connects over its
// DevTools pipe. `close()` on the returned `Browser` is what removes the
// profile directory again — nothing here leaves a `--user-data-dir` behind,
// on a clean run or a failing one, which is the property
// `pgrep -f user-data-dir` after a run is checking for.
export async function launch(options: LaunchOptions): Promise<Browser> {
    const userDataDir = await mkdtemp(join(tmpdir(), "hammer-cdp-"));

    const child = spawn(options.executablePath, [`--user-data-dir=${userDataDir}`, ...kChromiumArgs], {
        stdio: ["ignore", "ignore", "pipe", "pipe", "pipe"],
    });

    let stderrTail = "";
    const stderr = child.stderr;
    if (stderr !== null) {
        stderr.on("data", (chunk: Buffer) => {
            stderrTail = (stderrTail + chunk.toString("utf8")).slice(-4_000);
        });
    }

    const writeChannel = child.stdio[3];
    const readChannel = child.stdio[4];
    if (!(writeChannel instanceof Writable) || !(readChannel instanceof Readable)) {
        child.kill();
        await rm(userDataDir, { recursive: true, force: true });
        throw new Error(`Chromium did not open the DevTools pipe on fds 3/4. Its stderr:\n${stderrTail}`);
    }

    const connection = new Connection(writeChannel, readChannel);

    try {
        // Discovery has to be turned on before ANY `Target.targetCreated`
        // event arrives at all, and waiting for the first one — the initial
        // `about:blank` window this process was launched with — is what
        // this waits on rather than a fixed delay: it is the signal that
        // Chromium's own target list, and not just its pipe, is ready.
        const ready = connection.once(undefined, "Target.targetCreated", 20_000);
        await connection.send("Target.setDiscoverTargets", { discover: true });
        await ready;
    } catch (error) {
        child.kill();
        await rm(userDataDir, { recursive: true, force: true });
        const reason = error instanceof Error ? error.message : String(error);
        throw new Error(`Chromium failed to start: ${reason}\n${stderrTail}`);
    }

    return new BrowserImpl(connection, child, userDataDir);
}
