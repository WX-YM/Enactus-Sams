// The grant, and the two things the type has to make impossible.
//
// The load-bearing half of this module is the type-level half, and it is the
// half a green suite otherwise says nothing about: a requirement discharged by
// the compiler is worth exactly as much as the day it stops compiling. Each
// `@ts-expect-error` below fails the build when the line it guards starts being
// accepted, which is the only way a rule about what a caller can EXPRESS stays
// true.

import { describe, expect, it } from "../support/test.js";

import type { Capability } from "../../src/wire/capability.js";
import { burnsOnUse, capabilityToken, mintCapability } from "../../src/wire/capability.js";

function mint<Scope extends string>(token: unknown) {
    const result = mintCapability<Scope>(token);
    if (!result.ok) {
        throw new Error(`the test's own token does not mint: ${result.error}`);
    }
    return result.value;
}

describe("minting", () => {
    it("produces the token the server sent", () => {
        expect(capabilityToken(mint("01JABCDEF"))).toBe("01JABCDEF");
    });

    it("refuses a response that carried no token", () => {
        expect(mintCapability(undefined)).toEqual({ ok: false, error: "no-token" });
        expect(mintCapability(null)).toEqual({ ok: false, error: "no-token" });
        expect(mintCapability("")).toEqual({ ok: false, error: "no-token" });
    });

    it("refuses one that is not a string at all", () => {
        expect(mintCapability({ token: "x" })).toEqual({ ok: false, error: "no-token" });
    });

    it("refuses one past the header bound", () => {
        expect(mintCapability("a".repeat(513))).toEqual({ ok: false, error: "too-long" });
    });

    // `fetch` throws on a header value holding one of these, and a throw is the
    // wrong report for a value that arrived off a wire.
    it("refuses a value a header cannot carry", () => {
        expect(mintCapability("tok\nInjected: 1")).toEqual({ ok: false, error: "not-header-safe" });
        expect(mintCapability("tok\u0000")).toEqual({ ok: false, error: "not-header-safe" });
        expect(mintCapability("tøk")).toEqual({ ok: false, error: "not-header-safe" });
    });
});

describe("a capability is not a string", () => {
    it("cannot be written from one", () => {
        // @ts-expect-error a string is not a grant. If this compiles, a call
        // site can satisfy a destructive route with a value no server issued,
        // and the requirement has stopped being checked anywhere.
        const forged: Capability<"ContentDelete"> = "anything";
        expect(forged).toBe("anything");
    });

    it("does not travel between scopes", () => {
        const upload = mint<"MediaUpload">("tok");
        // @ts-expect-error a grant for one scope is not a grant for another.
        // The server would refuse it — the binding is in the lookup filter — and
        // the point of refusing it here is that the refusal costs no round trip
        // and no spent token.
        const wrong: Capability<"ContentDelete"> = upload;
        expect(capabilityToken(wrong)).toBe("tok");
    });
});

describe("whether redemption burns the token", () => {
    const table = { ContentDelete: true, DraftPreview: false } as const;

    it("is read from the generated table", () => {
        expect(burnsOnUse(table, "ContentDelete")).toBe(true);
        expect(burnsOnUse(table, "DraftPreview")).toBe(false);
    });

    it("is false for a route that requires no capability", () => {
        expect(burnsOnUse(table, null)).toBe(false);
    });

    // A client older than the server meets a scope its table predates. The
    // direction to be wrong in is the one that retries nothing: a retry that
    // presents a spent token reports a failure for an operation that succeeded.
    it("treats a scope the table has never heard of as single-use", () => {
        expect(burnsOnUse(table, "SomethingAddedThisMorning")).toBe(true);
    });
});
