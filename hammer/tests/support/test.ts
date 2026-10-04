// The one module every test file imports, replacing `vitest` (docs/15-tasks.md
// Phase 8 B1, CLAUDE.md §12). Every `node:test` binding this repository uses
// lives here and nowhere else, so a future backend swap — row B2 aliases this
// one module to a browser transport instead of `node:test` — touches this file
// and no other.
//
// --- what moved here from the deleted `vitest.config.ts` --------------------
//
// Three configurations existed for one reason each, and the reason survives
// as the shape of the scripts in `package.json` now that the configs are gone:
//
//   the default run    `npm run test`, over every suite except `live` and
//                       `browser`. This is what every commit passes.
//   `test:live`         excluded from the default run because it needs a
//                       server nobody's `npm run check` can assume exists.
//                       It fails rather than skips with no
//                       `HAMMER_LIVE_ORIGIN`, because a live suite that
//                       quietly passes with no server is a suite whose green
//                       means nothing (`tests/live/harness.ts`).
//   `test:browser`      excluded for the matching reason: a Chromium binary,
//                       not a server. It fails rather than skips with none
//                       found either (`tests/browser/harness.ts`).
//
// `node:test` needs no equivalent of vitest's `mergeConfig` warning — each
// script names its own explicit globs rather than a shared base a second
// config could accidentally concatenate into — but the reason the three runs
// stay three separate `npm` scripts rather than one with a flag is the same
// one the deleted configs gave: `npm run check` is what every commit passes,
// and mixing in a run that depends on a machine's environment is how a suite
// becomes something people learn to ignore.

import { after, afterEach, before, beforeEach, describe, it as nodeIt } from "node:test";

import type { EachRow } from "./each.js";
import { asRow, formatCaseName } from "./each.js";
import type { MockFn } from "./expect.js";
import { fn as makeFn } from "./expect.js";

export { expect } from "./expect.js";
export type { AsyncMatchers, Expectation, MockFn, Matchers } from "./expect.js";
export { afterEach, beforeEach, describe };
export { after as afterAll, before as beforeAll };

// --- mocks, restored after every test ----------------------------------------
//
// vitest's `restoreMocks: true` (`vitest.config.ts`) reset every `vi.fn()`
// after each test regardless of which test created it. `createdMocks` is this
// file's own module-level state doing the same job: `node --test` runs each
// test FILE in its own process, so this array is scoped to one file's run and
// never leaks into another's, the same isolation the deleted config relied on
// vitest's own worker pool for.
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

// --- it.each -----------------------------------------------------------------
//
// `node:test` has no equivalent, so this is a loop generating one named `it`
// per case. The formatting itself lives in `./each.js`, shared with the
// browser backend (`test_browser.ts`).

type TestBody = () => void | Promise<void>;

export interface ItWithEach {
    (name: string, fn: TestBody): void;
    each<T>(cases: readonly T[]): (name: string, fn: (...args: EachRow<T>) => void | Promise<void>) => void;
    // vitest's conditional case: run it when `condition` is true, otherwise
    // record it as skipped rather than failed. `tests/live/anvil.test.ts` uses
    // this for the superadmin-only rows, which need an account the reference
    // application's seed script may not have created.
    runIf(condition: boolean): (name: string, fn: TestBody) => void;
}

const itFn: ItWithEach = Object.assign(
    (name: string, testFn: TestBody): void => {
        nodeIt(name, testFn);
    },
    {
        each<T>(cases: readonly T[]) {
            return (name: string, testFn: (...args: EachRow<T>) => void | Promise<void>): void => {
                cases.forEach((value, index) => {
                    const row = asRow(value);
                    const caseName = formatCaseName(name, row, index);
                    nodeIt(caseName, () => testFn(...row));
                });
            };
        },
        runIf(condition: boolean) {
            return (name: string, testFn: TestBody): void => {
                nodeIt(name, { skip: !condition }, testFn);
            };
        },
    },
);

export const it: ItWithEach = itFn;
