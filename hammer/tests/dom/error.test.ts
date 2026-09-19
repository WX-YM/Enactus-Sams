// @vitest-environment happy-dom
//
// The error surface, and the two rules it exists to keep.
//
// A 404 renders as not-found and never as a denial: anvil answers a denied
// request on a stealth route with a byte-identical 404 so a probe cannot tell a
// missing object from a forbidden one. And the request id is usually absent,
// which is the case a surface written against the happy path renders as the word
// `undefined`.

import { describe, expect, it } from "vitest";

import type { HammerError } from "../../src/core/errors.js";
import type { ClassNames } from "../../src/core/tables.js";
import { renderError } from "../../src/dom/error.js";
import type { ErrorCopy, ErrorPart } from "../../src/dom/error.js";

type Code = "NOT_FOUND" | "FORBIDDEN" | "INTERNAL" | "Unknown";

const kClasses: ClassNames<ErrorPart> = { root: "e", message: "e-message", requestId: "e-id" };

const kCopy: ErrorCopy<Code> = {
    errors: {
        NOT_FOUND: "We could not find that.",
        FORBIDDEN: "You do not have access to that.",
        INTERNAL: "Something went wrong on our side.",
        Unknown: "Something went wrong.",
    },
    unreachable: "No connection.",
    requestId: "Reference",
};

function server(over: Partial<Extract<HammerError<Code, string>, { kind: "server" }>> = {}) {
    return {
        kind: "server",
        code: "INTERNAL",
        status: 500,
        requestId: null,
        fields: null,
        ...over,
    } as HammerError<Code, string>;
}

function mount(error: HammerError<Code, string> | null) {
    const host = document.createElement("div");
    document.body.append(host);
    const view = renderError(host, { error, classes: kClasses, copy: kCopy });
    const find = <E extends Element>(s: string): E => {
        const f = host.querySelector<E>(s);
        if (f === null) {
            throw new Error(`the surface did not draw ${s}`);
        }
        return f;
    };
    return { host, view, find, close: () => { view.close(); host.remove(); } };
}

describe("the words", () => {
    it("are the application's, looked up by the code the envelope carried", () => {
        const app = mount(server({ code: "INTERNAL" }));
        expect(app.find(".e-message").textContent).toBe("Something went wrong on our side.");
        app.close();
    });

    // A code this bundle predates still has to say something, and what it says
    // is the application's.
    it("answer a code this bundle has never heard of", () => {
        const app = mount(server({ code: "Unknown" }));
        expect(app.find(".e-message").textContent).toBe("Something went wrong.");
        app.close();
    });

    // A transport failure carries no code from anvil, because anvil never saw
    // it.
    it("say what could not be reached when the server was not", () => {
        const app = mount({ kind: "transport", cause: "network" });
        expect(app.find(".e-message").textContent).toBe("No connection.");
        app.close();
    });

    it("announce themselves, because they answer something already done", () => {
        const app = mount(server());
        expect(app.find(".e").getAttribute("role")).toBe("alert");
        app.close();
    });

    // A surface that is present and empty takes up room on every screen that has
    // no error on it.
    it("draw nothing at all when there is no error", () => {
        const app = mount(null);
        expect(app.find<HTMLElement>(".e").hidden).toBe(true);
        expect(app.find(".e-message").textContent).toBe("");
        app.close();
    });
});

// The oracle anvil spent a whole design removing.
describe("a 404", () => {
    it("renders the not-found words and not a denial", () => {
        const app = mount(server({ code: "NOT_FOUND", status: 404 }));
        const words = app.find(".e-message").textContent ?? "";
        expect(words).toBe("We could not find that.");
        expect(words).not.toContain("access");
        app.close();
    });

    // The guarantee is structural rather than remembered: the only thing this
    // module does with an error is one lookup by code. A 403 and a 404 differ
    // only by what the application wrote for each, and there is nowhere to put a
    // second table keyed by status.
    it("is told apart from a 403 by the code alone", () => {
        const notFound = mount(server({ code: "NOT_FOUND", status: 404 }));
        const forbidden = mount(server({ code: "FORBIDDEN", status: 403 }));
        expect(notFound.find(".e-message").textContent).toBe(kCopy.errors.NOT_FOUND);
        expect(forbidden.find(".e-message").textContent).toBe(kCopy.errors.FORBIDDEN);
        notFound.close();
        forbidden.close();
    });

    // anvil's 404 body is one constexpr string with no request behind it, so the
    // id is absent exactly where this surface is most often shown.
    it("says nothing about a reference it does not have", () => {
        const app = mount(server({ code: "NOT_FOUND", status: 404, requestId: null }));
        expect(app.find<HTMLElement>(".e-id").hidden).toBe(true);
        app.close();
    });
});

describe("the request id", () => {
    it("is displayed verbatim when there is one", () => {
        const app = mount(server({ requestId: "01J2X3Y4Z5" }));
        const shown = app.find<HTMLElement>(".e-id");
        expect(shown.hidden).toBe(false);
        expect(shown.textContent).toBe("Reference 01J2X3Y4Z5");
        app.close();
    });

    // A surface written against the happy path renders the word `undefined` on
    // the most common failure there is.
    it("never renders the absence as a word", () => {
        for (const error of [server({ requestId: null }), { kind: "transport", cause: "timeout" } as const]) {
            const app = mount(error as HammerError<Code, string>);
            expect(app.host.textContent).not.toContain("undefined");
            expect(app.host.textContent).not.toContain("null");
            app.close();
        }
    });

    it("reaches the page as text and never as markup", () => {
        const app = mount(server({ requestId: "<img src=x onerror=y>" }));
        expect(app.find(".e-id").children.length).toBe(0);
        expect(app.find(".e-id").textContent).toContain("<img src=x onerror=y>");
        app.close();
    });
});

// Reflecting one is the reflected-XSS and log-injection hazard anvil keeps out
// of its own responses, and an encoding hazard on non-Latin input besides.
describe("what is never echoed", () => {
    it("renders nothing from the request that failed", () => {
        const app = mount(
            server({ code: "INTERNAL", fields: new Map([["email", "BAD_FORMAT"]]) }),
        );
        const words = app.host.textContent ?? "";
        expect(words).not.toContain("email");
        expect(words).not.toContain("BAD_FORMAT");
        app.close();
    });
});
