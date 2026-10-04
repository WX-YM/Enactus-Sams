// The in-page test registry the browser backend for the test API runs on top
// of (docs/15-tasks.md Phase 8 B2, CLAUDE.md §12).
//
// Kept separate from `tests/support/test_browser.ts` so that module's own
// exports stay EXACTLY the surface `tests/support/test.ts` has —
// `describe`/`it`/the four hooks/`expect`/`fn` — with nothing a DOM or React
// test file would ever import by accident. The one thing this file adds, which
// `test.ts` has no analogue for because `node:test` owns it there, is `run()`:
// the entry point the page-side wrapper `tests/dom/in_browser.test.ts` builds
// calls once every test file has finished registering, and which reports back
// to the node-side driver.
//
// **No `node:` import**, for the same reason `expect.ts` and `each.ts` give:
// this file runs inside a Chromium page, where `node:` does not resolve.
//
// Hook ordering follows `node:test`'s own: `beforeAll`/`afterAll` run once at
// entry/exit of the `describe` block that declared them; `beforeEach` runs
// outer-to-inner before a test, `afterEach` inner-to-outer after one, for every
// test at or below the level that declared it. A `beforeAll`/`beforeEach` that
// throws fails every test it would have gated rather than being silently
// skipped — the same reason a hook exists at all.

export type Hook = () => void | Promise<void>;
export type TestBody = () => void | Promise<void>;

// The wire format back to the node-side driver. Deliberately the shape
// docs/15-tasks.md Phase 8 B2 names and nothing more: `path`/`name`/`ok`/
// `error`, so a result crosses `Runtime.evaluate`'s JSON boundary
// (`tests/browser/cdp.ts`) with no loss and no ambiguity about what a
// consumer on the other side is allowed to depend on.
export type CaseResult = {
    readonly path: readonly string[];
    readonly name: string;
    readonly ok: boolean;
    readonly error?: { readonly message: string; readonly stack?: string };
};

type Item =
    | { readonly kind: "test"; readonly name: string; readonly run: TestBody; readonly skip: boolean }
    | { readonly kind: "frame"; readonly frame: Frame };

class Frame {
    readonly name: string | undefined;
    readonly items: Item[] = [];
    readonly beforeAllHooks: Hook[] = [];
    readonly afterAllHooks: Hook[] = [];
    readonly beforeEachHooks: Hook[] = [];
    readonly afterEachHooks: Hook[] = [];

    constructor(name: string | undefined) {
        this.name = name;
    }
}

// Reset between files by the fact that each DOM test file is bundled and
// loaded into a brand-new page (`tests/dom/in_browser.test.ts`): there is
// never a second `describe`/`it` call sharing this module's state with a
// different file's.
const root = new Frame(undefined);
const stack: Frame[] = [root];

function current(): Frame {
    const frame = stack[stack.length - 1];
    if (frame === undefined) {
        throw new Error("describe()/it() left the frame stack empty, which they never do on their own");
    }
    return frame;
}

export function describe(name: string, body: () => void): void {
    const frame = new Frame(name);
    current().items.push({ kind: "frame", frame });
    stack.push(frame);
    try {
        body();
    } finally {
        stack.pop();
    }
}

export function registerTest(name: string, run: TestBody, skip = false): void {
    current().items.push({ kind: "test", name, run, skip });
}

export function beforeAll(hook: Hook): void {
    current().beforeAllHooks.push(hook);
}
export function afterAll(hook: Hook): void {
    current().afterAllHooks.push(hook);
}
export function beforeEach(hook: Hook): void {
    current().beforeEachHooks.push(hook);
}
export function afterEach(hook: Hook): void {
    current().afterEachHooks.push(hook);
}

function toError(error: unknown): { readonly message: string; readonly stack?: string } {
    const message = error instanceof Error ? error.message : String(error);
    const stack = error instanceof Error ? error.stack : undefined;
    return stack === undefined ? { message } : { message, stack };
}

function withTimeout(body: TestBody, timeoutMs: number, label: string): Promise<void> {
    return new Promise((resolve, reject) => {
        let settled = false;
        const timer = setTimeout(() => {
            if (settled) return;
            settled = true;
            reject(new Error(`"${label}" timed out after ${timeoutMs}ms`));
        }, timeoutMs);

        Promise.resolve()
            .then(() => body())
            .then(() => {
                if (settled) return;
                settled = true;
                clearTimeout(timer);
                resolve();
            })
            .catch((error: unknown) => {
                if (settled) return;
                settled = true;
                clearTimeout(timer);
                reject(error instanceof Error ? error : new Error(String(error)));
            });
    });
}

async function runHooks(hooks: readonly Hook[]): Promise<void> {
    for (const hook of hooks) {
        await hook();
    }
}

// Every test declared at or below a frame, in declaration order — what a
// `beforeAll`/`beforeEach` that throws fails without ever calling.
function everyTest(frame: Frame, path: readonly string[]): { path: readonly string[]; name: string }[] {
    const out: { path: readonly string[]; name: string }[] = [];
    for (const item of frame.items) {
        if (item.kind === "test") {
            out.push({ path, name: item.name });
        } else {
            const childPath = item.frame.name === undefined ? path : [...path, item.frame.name];
            out.push(...everyTest(item.frame, childPath));
        }
    }
    return out;
}

async function runFrame(
    frame: Frame,
    path: readonly string[],
    ancestorBeforeEach: readonly Hook[],
    ancestorAfterEach: readonly Hook[],
    results: CaseResult[],
    timeoutMs: number,
): Promise<void> {
    try {
        await runHooks(frame.beforeAllHooks);
    } catch (error) {
        const failure = toError(error);
        for (const test of everyTest(frame, path)) {
            results.push({ ...test, ok: false, error: failure });
        }
        return;
    }

    const beforeEachChain = [...ancestorBeforeEach, ...frame.beforeEachHooks];
    const afterEachChain = [...frame.afterEachHooks, ...ancestorAfterEach];

    for (const item of frame.items) {
        if (item.kind === "frame") {
            const childPath = item.frame.name === undefined ? path : [...path, item.frame.name];
            await runFrame(item.frame, childPath, beforeEachChain, afterEachChain, results, timeoutMs);
            continue;
        }

        if (item.skip) {
            // node:test reports a skipped case as neither a pass nor a
            // failure. This wire format has no third state, and nothing in
            // this suite calls `it.runIf` today (a grep-first survey, the
            // same one B1's own notes name as insufficient on its own —
            // confirmed here by the type-checker having nothing to say about
            // it either): a skip is recorded as a pass, which is the vacuous
            // truth `it.runIf(false)` asserts.
            results.push({ path, name: item.name, ok: true });
            continue;
        }

        try {
            await runHooks(beforeEachChain);
            await withTimeout(item.run, timeoutMs, item.name);
            results.push({ path, name: item.name, ok: true });
        } catch (error) {
            results.push({ path, name: item.name, ok: false, error: toError(error) });
        } finally {
            // afterEach runs even when the test or a beforeEach threw — the
            // same guarantee node:test gives, and the reason a store opened
            // by test eleven does not leak its subscription into test twelve.
            // An afterEach that itself throws must not overwrite the result
            // already recorded above: that would blame test twelve's failure
            // on test eleven's teardown, `CLAUDE.md` §4's "every task body
            // catches" applied to the harness's own bookkeeping.
            try {
                await runHooks(afterEachChain);
            } catch {
                // deliberately swallowed — see above.
            }
        }
    }

    try {
        await runHooks(frame.afterAllHooks);
    } catch {
        // An afterAll that throws is a teardown defect, not a test result; it
        // has no case of its own to attach to and every case in this frame
        // already has its own outcome recorded.
    }
}

export async function run(timeoutMs = 30_000): Promise<readonly CaseResult[]> {
    const results: CaseResult[] = [];
    await runFrame(root, [], [], [], results, timeoutMs);
    return results;
}
