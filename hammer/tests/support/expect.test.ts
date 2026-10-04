// Every matcher `tests/support/expect.ts` implements, each with a case that
// passes and a case that fails. A matcher that cannot fail is a matcher that
// passes for the wrong reason — the same lesson `tests/dom/csp.test.ts`
// learned from a trap that turned out to be shadowed, applied to the tool
// every other suite in this repository now trusts.
//
// The negative cases call the matcher through `toThrow`, rather than a raw
// `try`/`catch`: `expect(() => expect(actual).toBe(expected)).toThrow(...)`
// exercises the exact failure path a real assertion takes, including the
// message-building half nothing else here reaches.

import { AssertionError } from "./expect.js";
import { describe, expect, fn, it } from "./test.js";

describe("toBe", () => {
    it("passes for values identical under Object.is", () => {
        expect(1).toBe(1);
        expect(Number.NaN).toBe(Number.NaN);
    });
    it("fails otherwise, including -0 against 0", () => {
        expect(() => expect(1).toBe(2)).toThrow(AssertionError);
        expect(() => expect(0).toBe(-0)).toThrow(AssertionError);
    });
});

describe("toEqual", () => {
    it("compares structurally, ignoring undefined properties", () => {
        expect({ a: 1, b: undefined }).toEqual({ a: 1 });
        expect([1, [2, 3]]).toEqual([1, [2, 3]]);
    });
    it("compares Map, Set, Date and RegExp by content", () => {
        expect(new Map([["a", 1]])).toEqual(new Map([["a", 1]]));
        expect(new Set([1, 2])).toEqual(new Set([2, 1]));
        expect(new Date(2020, 0, 1)).toEqual(new Date(2020, 0, 1));
        expect(/abc/gi).toEqual(/abc/gi);
    });
    it("compares typed arrays and ArrayBuffer by their bytes", () => {
        expect(new Uint8Array([1, 2, 3])).toEqual(new Uint8Array([1, 2, 3]));
        expect(new Uint8Array([1, 2, 3]).buffer).toEqual(new Uint8Array([1, 2, 3]).buffer);
    });
    it("treats NaN as equal to itself and -0 as equal to 0, unlike toBe", () => {
        expect(Number.NaN).toEqual(Number.NaN);
        expect(-0).toEqual(0);
    });
    it("fails on a structural mismatch", () => {
        expect(() => expect({ a: 1 }).toEqual({ a: 2 })).toThrow(AssertionError);
        expect(() => expect([1, 2]).toEqual([1, 2, 3])).toThrow(AssertionError);
        expect(() => expect(new Set([1, 2])).toEqual(new Set([1, 3]))).toThrow(AssertionError);
        expect(() => expect(new Uint8Array([1, 2])).toEqual(new Int8Array([1, 2]))).toThrow(AssertionError);
    });
    it("requires the same constructor for two objects to match", () => {
        class Box {
            public constructor(public readonly value: number) {}
        }
        expect(() => expect(new Box(1)).toEqual({ value: 1 })).toThrow(AssertionError);
    });
});

describe("toMatchObject", () => {
    it("passes when actual is a superset of expected", () => {
        expect({ a: 1, b: 2, c: { d: 3 } }).toMatchObject({ a: 1, c: { d: 3 } });
    });
    it("matches an array element-wise, requiring an equal length", () => {
        expect({ items: [1, 2] }).toMatchObject({ items: [1, 2] });
        expect(() => expect({ items: [1, 2] }).toMatchObject({ items: [1] })).toThrow(AssertionError);
    });
    it("fails when a nested value differs or a key is missing", () => {
        expect(() => expect({ a: 1 }).toMatchObject({ a: 2 })).toThrow(AssertionError);
        expect(() => expect({ a: 1 }).toMatchObject({ b: 1 })).toThrow(AssertionError);
    });
});

describe("toContain", () => {
    it("passes for a substring and for an array member", () => {
        expect("hello world").toContain("world");
        expect([1, 2, 3]).toContain(2);
    });
    it("fails otherwise", () => {
        expect(() => expect("hello").toContain("bye")).toThrow(AssertionError);
        expect(() => expect([1, 2]).toContain(3)).toThrow(AssertionError);
    });
});

describe("toContainEqual", () => {
    it("passes when an element is deep-equal to the expected value", () => {
        expect([{ a: 1 }, { a: 2 }]).toContainEqual({ a: 2 });
    });
    it("fails when no element matches", () => {
        expect(() => expect([{ a: 1 }]).toContainEqual({ a: 2 })).toThrow(AssertionError);
    });
});

describe("toHaveLength", () => {
    it("passes on the actual length", () => {
        expect([1, 2, 3]).toHaveLength(3);
        expect("abc").toHaveLength(3);
    });
    it("fails on any other length", () => {
        expect(() => expect([1, 2]).toHaveLength(3)).toThrow(AssertionError);
    });
});

describe("toBeNull", () => {
    it("passes on null", () => {
        expect(null).toBeNull();
    });
    it("fails on undefined or any other value", () => {
        expect(() => expect(undefined).toBeNull()).toThrow(AssertionError);
        expect(() => expect(0).toBeNull()).toThrow(AssertionError);
    });
});

describe("toBeDefined", () => {
    it("passes on any value that is not undefined, including null", () => {
        expect(null).toBeDefined();
        expect(0).toBeDefined();
    });
    it("fails on undefined", () => {
        expect(() => expect(undefined).toBeDefined()).toThrow(AssertionError);
    });
});

describe("toBeUndefined", () => {
    it("passes on undefined", () => {
        expect(undefined).toBeUndefined();
    });
    it("fails on null", () => {
        expect(() => expect(null).toBeUndefined()).toThrow(AssertionError);
    });
});

describe("toBeTruthy", () => {
    it("passes on a truthy value", () => {
        expect("x").toBeTruthy();
    });
    it("fails on a falsy one", () => {
        expect(() => expect(0).toBeTruthy()).toThrow(AssertionError);
    });
});

describe("toBeInstanceOf", () => {
    it("passes for a real instance", () => {
        expect(new Map()).toBeInstanceOf(Map);
    });
    it("fails for a value of another type", () => {
        expect(() => expect({}).toBeInstanceOf(Map)).toThrow(AssertionError);
    });
});

describe("the four orderings", () => {
    it("toBeGreaterThan and toBeGreaterThanOrEqual pass at and past the bound", () => {
        expect(2).toBeGreaterThan(1);
        expect(2).toBeGreaterThanOrEqual(2);
    });
    it("toBeGreaterThan and toBeGreaterThanOrEqual fail at and past the bound", () => {
        expect(() => expect(1).toBeGreaterThan(1)).toThrow(AssertionError);
        expect(() => expect(1).toBeGreaterThanOrEqual(2)).toThrow(AssertionError);
    });
    it("toBeLessThan and toBeLessThanOrEqual pass at and past the bound", () => {
        expect(1).toBeLessThan(2);
        expect(1).toBeLessThanOrEqual(1);
    });
    it("toBeLessThan and toBeLessThanOrEqual fail at and past the bound", () => {
        expect(() => expect(2).toBeLessThan(2)).toThrow(AssertionError);
        expect(() => expect(2).toBeLessThanOrEqual(1)).toThrow(AssertionError);
    });
});

describe("toMatch", () => {
    it("passes for a substring and for a regular expression", () => {
        expect("hello").toMatch("ell");
        expect("hello").toMatch(/l+o/);
    });
    it("fails otherwise", () => {
        expect(() => expect("hello").toMatch("bye")).toThrow(AssertionError);
        expect(() => expect("hello").toMatch(/^bye/)).toThrow(AssertionError);
    });
});

describe("toThrow", () => {
    it("passes when the call throws, optionally matching a string, a RegExp or a class", () => {
        expect(() => {
            throw new Error("boom");
        }).toThrow();
        expect(() => {
            throw new Error("boom");
        }).toThrow("boom");
        expect(() => {
            throw new Error("boom");
        }).toThrow(/bo+m/);
        expect(() => {
            throw new TypeError("x");
        }).toThrow(TypeError);
    });
    it("fails when the call does not throw, or the thrown value does not match", () => {
        expect(() => expect(() => 1).toThrow()).toThrow(AssertionError);
        expect(() =>
            expect(() => {
                throw new Error("boom");
            }).toThrow("nope"),
        ).toThrow(AssertionError);
        expect(() =>
            expect(() => {
                throw new Error("boom");
            }).toThrow(TypeError),
        ).toThrow(AssertionError);
    });
});

describe(".not", () => {
    it("inverts every matcher it wraps", () => {
        expect(1).not.toBe(2);
        expect([1]).not.toContain(2);
    });
    it("fails when the wrapped matcher would have passed", () => {
        expect(() => expect(1).not.toBe(1)).toThrow(AssertionError);
    });
});

describe("fn", () => {
    it("records every call and answers toHaveBeenCalled*", () => {
        const mock = fn((a: number, b: number) => a + b);
        expect(mock).not.toHaveBeenCalled();

        const result = mock(1, 2);

        expect(result).toBe(3);
        expect(mock).toHaveBeenCalled();
        expect(mock).toHaveBeenCalledTimes(1);
        expect(mock).toHaveBeenCalledWith(1, 2);
    });
    it("fails toHaveBeenCalled* on the wrong count or the wrong arguments", () => {
        const mock = fn();
        expect(() => expect(mock).toHaveBeenCalled()).toThrow(AssertionError);

        mock(1);

        expect(() => expect(mock).toHaveBeenCalledTimes(2)).toThrow(AssertionError);
        expect(() => expect(mock).toHaveBeenCalledWith(2)).toThrow(AssertionError);
    });
    it("mockReturnValue and mockImplementation replace the answer", () => {
        const mock = fn<[], number>().mockReturnValue(5);
        expect(mock()).toBe(5);

        mock.mockImplementation(() => 6);
        expect(mock()).toBe(6);
    });
    it("mockRestore clears the calls and the implementation set after creation", () => {
        const mock = fn(() => 1);
        mock();
        mock.mockReturnValue(2);

        mock.mockRestore();

        expect(mock.mock.calls).toHaveLength(0);
        expect(mock()).toBe(1);
    });
});

describe("resolves", () => {
    it("passes when the promise resolves to the matched value", async () => {
        await expect(Promise.resolve(1)).resolves.toBe(1);
        await expect(Promise.resolve({ a: 1, b: 2 })).resolves.toMatchObject({ a: 1 });
        await expect(Promise.resolve(undefined)).resolves.toBeUndefined();
        await expect(Promise.resolve(null)).resolves.toBeNull();
        await expect(Promise.resolve([1, 2])).resolves.toEqual([1, 2]);
    });
    it("fails when the promise rejects instead of resolving", async () => {
        await expect(expect(Promise.reject(new Error("x"))).resolves.toBe(1)).rejects.toThrow();
    });
    it("fails when the resolved value does not match", async () => {
        await expect(expect(Promise.resolve(1)).resolves.toBe(2)).rejects.toThrow(AssertionError);
    });
});

describe("rejects", () => {
    it("passes when the promise rejects, optionally matching the reason", async () => {
        await expect(Promise.reject(new Error("boom"))).rejects.toThrow("boom");
    });
    it("fails when the promise resolves instead of rejecting", async () => {
        await expect(expect(Promise.resolve(1)).rejects.toThrow()).rejects.toThrow(AssertionError);
    });
});
