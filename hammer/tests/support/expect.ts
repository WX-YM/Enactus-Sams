// The in-repo `expect`, replacing `vitest`'s (docs/15-tasks.md Phase 8 B1).
//
// This implements exactly the matchers the suite uses — found by grepping the
// suite, not by guessing at Jest's or vitest's full surface — and nothing
// else. A matcher nobody calls is a matcher nobody's test file has ever
// disagreed with, which is the same argument `tools/check-surface-coverage.sh`
// makes about a published value with no caller.
//
// **This file imports nothing from `node:`.** Row B2 runs it inside a real
// Chromium page in place of happy-dom, and a `node:` import here is an import
// that resolves in this process and not in a browser's. Every primitive used
// below — `Object.is`, `ArrayBuffer.isView`, a hand-rolled deep comparison —
// is plain JavaScript for exactly that reason.
//
// Every matcher is a real, named member of `Matchers` rather than an entry in
// an indexed map: `noPropertyAccessFromIndexSignature` (`tsconfig.json`)
// would otherwise force every call site to write `expect(x)["toBe"](y)`.
//
// `tests/support/expect.test.ts` gives every matcher a positive AND a negative
// case: a matcher that cannot fail is a matcher that passes for the wrong
// reason, which is the same lesson `tests/dom/csp.test.ts` learned from a trap
// that turned out to be shadowed.

// --- formatting for a failure message ---------------------------------------
//
// `util.inspect` is off-limits (it is `node:util`), so this is a small,
// deliberately incomplete stringifier: enough to make a failure message
// legible, not a general-purpose formatter. Circular references are guarded
// against because a thrown error or a store's own state can carry one, and a
// formatter that stack-overflows on the value it was asked to describe is a
// worse failure than the one it was reporting.

function quote(value: string): string {
    return `"${value.replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
}

function functionLabel(value: (...args: unknown[]) => unknown): string {
    return value.name ? `[Function: ${value.name}]` : "[Function (anonymous)]";
}

function stringify(value: unknown, seen: readonly unknown[] = []): string {
    if (value === null) return "null";
    if (value === undefined) return "undefined";
    if (typeof value === "string") return quote(value);
    if (typeof value === "bigint") return `${value}n`;
    if (typeof value === "number" || typeof value === "boolean") return String(value);
    if (typeof value === "symbol") return value.toString();
    if (typeof value === "function") return functionLabel(value as (...args: unknown[]) => unknown);

    if (seen.includes(value)) return "[Circular]";
    const nextSeen = [...seen, value];

    if (value instanceof Date) return value.toISOString();
    if (value instanceof RegExp) return value.toString();
    if (ArrayBuffer.isView(value)) {
        const named = value as unknown as { readonly constructor: { readonly name: string } };
        const items = Array.from(value as unknown as Iterable<unknown>, (item) => stringify(item, nextSeen));
        return `${named.constructor.name}(${items.length}) [${items.join(", ")}]`;
    }
    if (value instanceof ArrayBuffer) {
        return `ArrayBuffer { byteLength: ${value.byteLength} }`;
    }
    if (Array.isArray(value)) {
        return `[${value.map((item) => stringify(item, nextSeen)).join(", ")}]`;
    }
    if (value instanceof Map) {
        const entries = [...value].map(([k, v]) => `${stringify(k, nextSeen)} => ${stringify(v, nextSeen)}`);
        return `Map(${value.size}) {${entries.length > 0 ? ` ${entries.join(", ")} ` : ""}}`;
    }
    if (value instanceof Set) {
        const items = [...value].map((item) => stringify(item, nextSeen));
        return `Set(${value.size}) {${items.length > 0 ? ` ${items.join(", ")} ` : ""}}`;
    }

    const record = value as Record<string, unknown>;
    const ctor = (value as { readonly constructor?: { readonly name: string } }).constructor;
    const name = ctor && ctor !== Object ? `${ctor.name} ` : "";
    const entries = Object.keys(record).map((key) => `${key}: ${stringify(record[key], nextSeen)}`);
    return `${name}{${entries.length > 0 ? ` ${entries.join(", ")} ` : ""}}`;
}

// --- the one error every matcher throws -------------------------------------

export class AssertionError extends Error {
    public constructor(message: string) {
        super(message);
        this.name = "AssertionError";
    }
}

interface CheckResult {
    readonly pass: boolean;
    readonly message: (self: string) => string;
}

function report(result: CheckResult, negate: boolean, label?: string): void {
    const self = negate ? "not " : "";
    if (result.pass === negate) {
        const prefix = label === undefined ? "" : `${label}: `;
        throw new AssertionError(`${prefix}${result.message(self)}`);
    }
}

// --- deep equality, Jest's `toEqual` semantics -------------------------------
//
// Recursive; a property whose value is `undefined` is ignored on both sides,
// so `{ a: 1, b: undefined }` equals `{ a: 1 }`; `Map`, `Set`, `Date`, `RegExp`,
// typed arrays and `ArrayBuffer` compare by content rather than by reference.

function sameValueZero(a: unknown, b: unknown): boolean {
    if (a === b) return true;
    return typeof a === "number" && typeof b === "number" && Number.isNaN(a) && Number.isNaN(b);
}

function typedArrayBytes(value: ArrayBufferView): Uint8Array {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
}

function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
        if (a[i] !== b[i]) return false;
    }
    return true;
}

function definedEntries(value: Record<string, unknown>): ReadonlyArray<readonly [string, unknown]> {
    return Object.keys(value)
        .filter((key) => value[key] !== undefined)
        .map((key): readonly [string, unknown] => [key, value[key]]);
}

function deepEqual(a: unknown, b: unknown, seen: Map<object, object> = new Map()): boolean {
    if (sameValueZero(a, b)) return true;
    if (typeof a !== typeof b) return false;
    if (a === null || b === null || a === undefined || b === undefined) return false;
    if (typeof a !== "object") return false;

    if (a instanceof Date || b instanceof Date) {
        return a instanceof Date && b instanceof Date && a.getTime() === b.getTime();
    }
    if (a instanceof RegExp || b instanceof RegExp) {
        return a instanceof RegExp && b instanceof RegExp && a.source === b.source && a.flags === b.flags;
    }
    if (ArrayBuffer.isView(a) || ArrayBuffer.isView(b)) {
        if (!ArrayBuffer.isView(a) || !ArrayBuffer.isView(b)) return false;
        if (a.constructor !== b.constructor) return false;
        return bytesEqual(typedArrayBytes(a), typedArrayBytes(b));
    }
    if (a instanceof ArrayBuffer || b instanceof ArrayBuffer) {
        if (!(a instanceof ArrayBuffer) || !(b instanceof ArrayBuffer)) return false;
        return bytesEqual(new Uint8Array(a), new Uint8Array(b));
    }

    const objectA: object = a;
    const objectB: object = b;
    if (seen.get(objectA) === objectB) return true;
    seen.set(objectA, objectB);

    if (Array.isArray(a) || Array.isArray(b)) {
        if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
        return a.every((item, index) => deepEqual(item, b[index], seen));
    }

    if (a instanceof Map || b instanceof Map) {
        if (!(a instanceof Map) || !(b instanceof Map) || a.size !== b.size) return false;
        for (const [key, value] of a) {
            if (b.has(key)) {
                if (!deepEqual(value, b.get(key), seen)) return false;
                continue;
            }
            // A key that is itself an object may be structurally, not
            // referentially, equal — the same reason `toEqual` recurses at all.
            const matched = [...b].some(([bKey, bValue]) => deepEqual(key, bKey, seen) && deepEqual(value, bValue, seen));
            if (!matched) return false;
        }
        return true;
    }

    if (a instanceof Set || b instanceof Set) {
        if (!(a instanceof Set) || !(b instanceof Set) || a.size !== b.size) return false;
        const remaining = [...b];
        for (const item of a) {
            const index = remaining.findIndex((candidate) => deepEqual(item, candidate, seen));
            if (index === -1) return false;
            remaining.splice(index, 1);
        }
        return true;
    }

    if ((a as { constructor?: unknown }).constructor !== (b as { constructor?: unknown }).constructor) return false;

    const entriesA = definedEntries(a as Record<string, unknown>);
    const entriesB = definedEntries(b as Record<string, unknown>);
    if (entriesA.length !== entriesB.length) return false;
    return entriesA.every(([key, value]) => {
        const other = (b as Record<string, unknown>)[key];
        return other !== undefined && deepEqual(value, other, seen);
    });
}

// --- `toMatchObject`: a recursive subset match -------------------------------
//
// Every key `expected` names must be present and matching on `actual`; extra
// keys on `actual` are ignored, which is the entire reason to reach for this
// matcher instead of `toEqual`. Arrays are the exception Jest also makes: an
// array inside the expected shape matches element-wise with an EQUAL length,
// not as a subset — a shorter or longer array is a different array.

function isPlainMatchTarget(value: unknown): value is Record<string, unknown> {
    return (
        typeof value === "object" &&
        value !== null &&
        !Array.isArray(value) &&
        !(value instanceof Date) &&
        !(value instanceof RegExp) &&
        !(value instanceof Map) &&
        !(value instanceof Set) &&
        !ArrayBuffer.isView(value) &&
        !(value instanceof ArrayBuffer)
    );
}

function matchesSubset(actual: unknown, expected: unknown, seen: Map<object, object> = new Map()): boolean {
    if (Array.isArray(expected)) {
        if (!Array.isArray(actual) || actual.length !== expected.length) return false;
        return expected.every((item, index) => matchesSubset(actual[index], item, seen));
    }
    if (isPlainMatchTarget(expected)) {
        if (typeof actual !== "object" || actual === null) return false;
        return Object.keys(expected).every(
            (key) =>
                key in (actual as object) &&
                matchesSubset((actual as Record<string, unknown>)[key], expected[key], seen),
        );
    }
    return deepEqual(actual, expected, seen);
}

// --- mock functions, replacing `vi.fn()` -------------------------------------

export interface MockFn<Args extends readonly unknown[] = readonly unknown[], Return = unknown> {
    (...args: Args): Return;
    readonly mock: { readonly calls: readonly (readonly [...Args])[] };
    mockImplementation(implementation: (...args: Args) => Return): MockFn<Args, Return>;
    mockReturnValue(value: Return): MockFn<Args, Return>;
    mockRestore(): void;
}

export function fn<Args extends readonly unknown[] = readonly unknown[], Return = unknown>(
    implementation?: (...args: Args) => Return,
): MockFn<Args, Return> {
    const initial = implementation ?? ((() => undefined) as (...args: Args) => Return);
    let current = initial;
    const calls: Args[] = [];

    const mockFn = ((...args: Args): Return => {
        calls.push(args);
        return current(...args);
    }) as MockFn<Args, Return>;

    Object.defineProperty(mockFn, "mock", { value: { calls }, enumerable: true });
    mockFn.mockImplementation = (next) => {
        current = next;
        return mockFn;
    };
    mockFn.mockReturnValue = (value) => {
        current = () => value;
        return mockFn;
    };
    mockFn.mockRestore = () => {
        calls.length = 0;
        current = initial;
    };

    return mockFn;
}

function isMockFn(value: unknown): value is MockFn {
    if (typeof value !== "function") return false;
    const candidate = value as { readonly mock?: { readonly calls?: unknown } };
    return Array.isArray(candidate.mock?.calls);
}

// --- `toThrow`, shared between the sync form and `.rejects` ------------------
//
// A sync `toThrow` is handed a closure and calls it. `.rejects.toThrow()` is
// handed the rejection reason itself — the promise has already been awaited
// and has already thrown by the time this runs — so the two cases are told
// apart by whether the value received IS the thing to call.

type ErrorConstructor = new (...args: never[]) => Error;

function thrown(actual: unknown): { readonly didThrow: boolean; readonly error: unknown } {
    if (typeof actual === "function") {
        try {
            (actual as () => unknown)();
            return { didThrow: false, error: undefined };
        } catch (error) {
            return { didThrow: true, error };
        }
    }
    return { didThrow: true, error: actual };
}

function errorMessage(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

function matchesExpectedThrow(error: unknown, expected: string | RegExp | ErrorConstructor | undefined): boolean {
    if (expected === undefined) return true;
    if (typeof expected === "string") return errorMessage(error).includes(expected);
    if (expected instanceof RegExp) return expected.test(errorMessage(error));
    return error instanceof expected;
}

// --- the checkers, shared between the sync and the `.resolves`/`.rejects` forms

function checkBe(actual: unknown, expected: unknown): CheckResult {
    return { pass: Object.is(actual, expected), message: (self) => `expected ${stringify(actual)} ${self}to be ${stringify(expected)} (Object.is)` };
}

function checkEqual(actual: unknown, expected: unknown): CheckResult {
    return { pass: deepEqual(actual, expected), message: (self) => `expected ${stringify(actual)} ${self}to equal ${stringify(expected)}` };
}

function checkMatchObject(actual: unknown, expected: unknown): CheckResult {
    return {
        pass: matchesSubset(actual, expected),
        message: (self) => `expected ${stringify(actual)} ${self}to match object ${stringify(expected)}`,
    };
}

function checkContain(actual: unknown, expected: unknown): CheckResult {
    // The suite calls this on a string (a substring check) and on an array
    // (`Object.keys(...)`, membership) — nothing else, so `.includes` is the
    // whole check. Both give it the same meaning already; nothing here needs
    // to guess at a third container shape nobody has passed it.
    const container = actual as { includes?: (item: unknown) => boolean } | null | undefined;
    const pass = typeof container?.includes === "function" && container.includes(expected);
    return { pass, message: (self) => `expected ${stringify(actual)} ${self}to contain ${stringify(expected)}` };
}

function checkContainEqual(actual: unknown, expected: unknown): CheckResult {
    const items = Array.from(actual as Iterable<unknown>);
    return {
        pass: items.some((item) => deepEqual(item, expected)),
        message: (self) => `expected ${stringify(actual)} ${self}to contain an item equal to ${stringify(expected)}`,
    };
}

function checkHaveLength(actual: unknown, expected: number): CheckResult {
    const length = (actual as { length?: number } | null | undefined)?.length;
    return {
        pass: length === expected,
        message: (self) => `expected ${stringify(actual)} ${self}to have length ${expected}, got ${stringify(length)}`,
    };
}

function checkBeNull(actual: unknown): CheckResult {
    return { pass: actual === null, message: (self) => `expected ${stringify(actual)} ${self}to be null` };
}

function checkBeDefined(actual: unknown): CheckResult {
    return { pass: actual !== undefined, message: (self) => `expected ${stringify(actual)} ${self}to be defined` };
}

function checkBeUndefined(actual: unknown): CheckResult {
    return { pass: actual === undefined, message: (self) => `expected ${stringify(actual)} ${self}to be undefined` };
}

function checkBeTruthy(actual: unknown): CheckResult {
    return { pass: Boolean(actual), message: (self) => `expected ${stringify(actual)} ${self}to be truthy` };
}

function checkBeInstanceOf(actual: unknown, expected: new (...args: never[]) => unknown): CheckResult {
    return {
        pass: actual instanceof expected,
        message: (self) => `expected ${stringify(actual)} ${self}to be an instance of ${expected.name}`,
    };
}

function checkGreaterThan(actual: unknown, expected: number): CheckResult {
    return { pass: (actual as number) > expected, message: (self) => `expected ${stringify(actual)} ${self}to be greater than ${expected}` };
}

function checkGreaterThanOrEqual(actual: unknown, expected: number): CheckResult {
    return {
        pass: (actual as number) >= expected,
        message: (self) => `expected ${stringify(actual)} ${self}to be greater than or equal to ${expected}`,
    };
}

function checkLessThan(actual: unknown, expected: number): CheckResult {
    return { pass: (actual as number) < expected, message: (self) => `expected ${stringify(actual)} ${self}to be less than ${expected}` };
}

function checkLessThanOrEqual(actual: unknown, expected: number): CheckResult {
    return {
        pass: (actual as number) <= expected,
        message: (self) => `expected ${stringify(actual)} ${self}to be less than or equal to ${expected}`,
    };
}

function checkMatch(actual: unknown, expected: string | RegExp): CheckResult {
    const value = actual as string;
    const pass = typeof expected === "string" ? value.includes(expected) : expected.test(value);
    return { pass, message: (self) => `expected ${stringify(actual)} ${self}to match ${stringify(expected)}` };
}

function checkThrow(actual: unknown, expected: string | RegExp | ErrorConstructor | undefined): CheckResult {
    const { didThrow, error } = thrown(actual);
    const pass = didThrow && matchesExpectedThrow(error, expected);
    return {
        pass,
        message: (self) =>
            didThrow
                ? `expected the call ${self}to throw matching ${stringify(expected)}, threw ${stringify(error)}`
                : `expected the call ${self}to throw`,
    };
}

function checkHaveBeenCalled(actual: unknown): CheckResult {
    return {
        pass: isMockFn(actual) && actual.mock.calls.length > 0,
        message: (self) => `expected the mock ${self}to have been called`,
    };
}

function checkHaveBeenCalledTimes(actual: unknown, expected: number): CheckResult {
    const times = isMockFn(actual) ? actual.mock.calls.length : -1;
    return {
        pass: times === expected,
        message: (self) => `expected the mock ${self}to have been called ${expected} time(s), was called ${times} time(s)`,
    };
}

function checkHaveBeenCalledWith(actual: unknown, expected: readonly unknown[]): CheckResult {
    const calls = isMockFn(actual) ? actual.mock.calls : [];
    const pass = calls.some((call) => deepEqual(call, expected));
    return {
        pass,
        message: (self) =>
            `expected the mock ${self}to have been called with ${stringify(expected)}, calls were ${stringify(calls)}`,
    };
}

// --- the public matcher shapes ------------------------------------------------

export interface Matchers {
    toBe(expected: unknown): void;
    toEqual(expected: unknown): void;
    toMatchObject(expected: unknown): void;
    toContain(expected: unknown): void;
    toContainEqual(expected: unknown): void;
    toHaveLength(expected: number): void;
    toBeNull(): void;
    toBeDefined(): void;
    toBeUndefined(): void;
    toBeTruthy(): void;
    toBeInstanceOf(expected: new (...args: never[]) => unknown): void;
    toBeGreaterThan(expected: number): void;
    toBeGreaterThanOrEqual(expected: number): void;
    toBeLessThan(expected: number): void;
    toBeLessThanOrEqual(expected: number): void;
    toMatch(expected: string | RegExp): void;
    toThrow(expected?: string | RegExp | ErrorConstructor): void;
    toHaveBeenCalled(): void;
    toHaveBeenCalledTimes(expected: number): void;
    toHaveBeenCalledWith(...expected: readonly unknown[]): void;
}

// The subset reachable through `.resolves` and `.rejects` — the union of what
// each one is actually chained with in this suite (`resolves.toBe`,
// `.toBeUndefined`, `.toEqual`, `.toMatchObject`, `.toBeNull`; `rejects.toThrow`).
export interface AsyncMatchers {
    toBe(expected: unknown): Promise<void>;
    toEqual(expected: unknown): Promise<void>;
    toMatchObject(expected: unknown): Promise<void>;
    toBeNull(): Promise<void>;
    toBeUndefined(): Promise<void>;
    toThrow(expected?: string | RegExp | ErrorConstructor): Promise<void>;
}

function makeMatchers(actual: unknown, negate: boolean, label: string | undefined): Matchers {
    return {
        toBe: (expected) => report(checkBe(actual, expected), negate, label),
        toEqual: (expected) => report(checkEqual(actual, expected), negate, label),
        toMatchObject: (expected) => report(checkMatchObject(actual, expected), negate, label),
        toContain: (expected) => report(checkContain(actual, expected), negate, label),
        toContainEqual: (expected) => report(checkContainEqual(actual, expected), negate, label),
        toHaveLength: (expected) => report(checkHaveLength(actual, expected), negate, label),
        toBeNull: () => report(checkBeNull(actual), negate, label),
        toBeDefined: () => report(checkBeDefined(actual), negate, label),
        toBeUndefined: () => report(checkBeUndefined(actual), negate, label),
        toBeTruthy: () => report(checkBeTruthy(actual), negate, label),
        toBeInstanceOf: (expected) => report(checkBeInstanceOf(actual, expected), negate, label),
        toBeGreaterThan: (expected) => report(checkGreaterThan(actual, expected), negate, label),
        toBeGreaterThanOrEqual: (expected) => report(checkGreaterThanOrEqual(actual, expected), negate, label),
        toBeLessThan: (expected) => report(checkLessThan(actual, expected), negate, label),
        toBeLessThanOrEqual: (expected) => report(checkLessThanOrEqual(actual, expected), negate, label),
        toMatch: (expected) => report(checkMatch(actual, expected), negate, label),
        toThrow: (expected) => report(checkThrow(actual, expected), negate, label),
        toHaveBeenCalled: () => report(checkHaveBeenCalled(actual), negate, label),
        toHaveBeenCalledTimes: (expected) => report(checkHaveBeenCalledTimes(actual, expected), negate, label),
        toHaveBeenCalledWith: (...expected) => report(checkHaveBeenCalledWith(actual, expected), negate, label),
    };
}

async function settleResolved(candidate: unknown): Promise<unknown> {
    try {
        return await (candidate as Promise<unknown>);
    } catch (error) {
        throw new AssertionError(`expected the promise to resolve, but it rejected with ${stringify(error)}`);
    }
}

async function settleRejected(candidate: unknown): Promise<unknown> {
    let resolvedValue: unknown;
    try {
        resolvedValue = await (candidate as Promise<unknown>);
    } catch (error) {
        return error;
    }
    throw new AssertionError(`expected the promise to reject, but it resolved with ${stringify(resolvedValue)}`);
}

function makeAsyncMatchers(
    settle: (candidate: unknown) => Promise<unknown>,
    candidate: unknown,
    label: string | undefined,
): AsyncMatchers {
    return {
        toBe: async (expected) => report(checkBe(await settle(candidate), expected), false, label),
        toEqual: async (expected) => report(checkEqual(await settle(candidate), expected), false, label),
        toMatchObject: async (expected) => report(checkMatchObject(await settle(candidate), expected), false, label),
        toBeNull: async () => report(checkBeNull(await settle(candidate)), false, label),
        toBeUndefined: async () => report(checkBeUndefined(await settle(candidate)), false, label),
        toThrow: async (expected) => report(checkThrow(await settle(candidate), expected), false, label),
    };
}

// --- the public `expect` -----------------------------------------------------

export interface Expectation extends Matchers {
    readonly not: Matchers;
    readonly resolves: AsyncMatchers;
    readonly rejects: AsyncMatchers;
}

// The second parameter mirrors vitest's own `expect(actual, message)`: a label
// prefixed onto whichever matcher ends up failing. Several suites loop over a
// generated seed or a randomised case and pass the seed here, because a
// failure with no label just says "one of these many cases disagreed" — the
// property this whole approach exists to test.
export function expect(actual: unknown, message?: string): Expectation {
    return {
        ...makeMatchers(actual, false, message),
        not: makeMatchers(actual, true, message),
        resolves: makeAsyncMatchers(settleResolved, actual, message),
        rejects: makeAsyncMatchers(settleRejected, actual, message),
    };
}
