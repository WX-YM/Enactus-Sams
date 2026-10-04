// The browser backend for the test API (docs/15-tasks.md Phase 8 B2,
// CLAUDE.md §12), replacing the `happy-dom` stopgap. Exports EXACTLY the
// surface `tests/support/test.ts` has — `describe`, `it` (with `.each` and
// `.runIf`), the four hooks, `expect` and `fn` — so a DOM or React test file
// written against `"../support/test.js"` runs unmodified whether the bundler
// resolves that specifier to `test.ts` (never, once this file exists — see
// `tests/dom/in_browser.test.ts`'s resolve plugin) or to this one.
//
// There is no `node:test` here: this module is bundled by esbuild and run
// inside a real Chromium page, where `node:test` does not exist. Registration
// (`describe`/`it`/the hooks) is delegated to `./browser_registry.js`, which
// also owns `run()` — the function the page-side wrapper calls once the test
// file has finished registering. `run()` is deliberately NOT re-exported here:
// a DOM test file has no reason to call it, and a surface this module exports
// beyond test.ts's own is a surface nothing checks stays a drop-in replacement.
//
// Mock restoration matches `test.ts`: every `fn()` this module hands out is
// restored after the test that created it, regardless of which file's
// `afterEach` chain runs it, because a mock is per-file state here exactly as
// it is per-process state there (`node --test` runs one file per process;
// this backend runs one file per page).

import {
    afterAll as afterAllReg,
    afterEach as afterEachReg,
    beforeAll as beforeAllReg,
    beforeEach as beforeEachReg,
    describe as describeReg,
    registerTest,
} from "./browser_registry.js";
import type { EachRow } from "./each.js";
import { asRow, formatCaseName } from "./each.js";
import type { MockFn } from "./expect.js";
import { fn as makeFn } from "./expect.js";

export { expect } from "./expect.js";
export type { AsyncMatchers, Expectation, MockFn, Matchers } from "./expect.js";

export const describe: (name: string, body: () => void) => void = describeReg;
export const beforeAll: (hook: () => void | Promise<void>) => void = beforeAllReg;
export const afterAll: (hook: () => void | Promise<void>) => void = afterAllReg;
export const beforeEach: (hook: () => void | Promise<void>) => void = beforeEachReg;
export const afterEach: (hook: () => void | Promise<void>) => void = afterEachReg;

// --- mocks, restored after every test ----------------------------------------

const createdMocks: MockFn[] = [];

export function fn<Args extends readonly unknown[] = readonly unknown[], Return = unknown>(
    implementation?: (...args: Args) => Return,
): MockFn<Args, Return> {
    const mock = makeFn(implementation);
    createdMocks.push(mock as unknown as MockFn);
    return mock;
}

afterEach(() => {
    for (const mock of createdMocks) {
        mock.mockRestore();
    }
    createdMocks.length = 0;
});

// --- it, it.each, it.runIf ---------------------------------------------------

type TestBody = () => void | Promise<void>;

export interface ItWithEach {
    (name: string, fn: TestBody): void;
    each<T>(cases: readonly T[]): (name: string, fn: (...args: EachRow<T>) => void | Promise<void>) => void;
    runIf(condition: boolean): (name: string, fn: TestBody) => void;
}

const itFn: ItWithEach = Object.assign(
    (name: string, testFn: TestBody): void => {
        registerTest(name, testFn);
    },
    {
        each<T>(cases: readonly T[]) {
            return (name: string, testFn: (...args: EachRow<T>) => void | Promise<void>): void => {
                cases.forEach((value, index) => {
                    const row = asRow(value);
                    const caseName = formatCaseName(name, row, index);
                    registerTest(caseName, () => testFn(...row));
                });
            };
        },
        runIf(condition: boolean) {
            return (name: string, testFn: TestBody): void => {
                registerTest(name, testFn, !condition);
            };
        },
    },
);

export const it: ItWithEach = itFn;
