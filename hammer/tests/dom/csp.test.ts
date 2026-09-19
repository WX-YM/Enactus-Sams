// @vitest-environment happy-dom
//
// Every component under a policy with no `unsafe-inline` and no `unsafe-eval`.
//
// happy-dom enforces neither, so the absence is asserted the way
// `tests/state/persistence.test.ts` asserts that nothing is persisted: by
// installing tripwires on every route a component would have to take to break
// the policy, driving the whole layer through them, and — the part that makes it
// worth anything — driving the tripwire itself at the end. A check that cannot
// fail reports clean for the wrong reason.
//
// The policy this stands in for is the application's (`docs/00-architecture.md`
// §8). What it costs to get wrong is not an error: a component that sets an
// inline style passes every other suite in this repository and is silently blank
// in production.

import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { mountEach } from "./registry.js";

// Recorded rather than thrown on, so a failure names what was reached instead of
// surfacing as whatever some caller's `catch` decided to do with it — and so one
// violation does not hide the next.
const touched: string[] = [];

const restore: (() => void)[] = [];

function trapProperty(target: object, name: string, onSet: (value: unknown) => void): void {
    const held = Object.getOwnPropertyDescriptor(target, name);
    Object.defineProperty(target, name, {
        configurable: true,
        get(): unknown {
            return held?.get?.call(this);
        },
        set(value: unknown) {
            onSet(value);
            held?.set?.call(this, value);
        },
    });
    restore.push(() => {
        if (held === undefined) {
            delete (target as Record<string, unknown>)[name];
        } else {
            Object.defineProperty(target, name, held);
        }
    });
}

function trapMethod<T extends object>(target: T, name: string, watch: (args: unknown[]) => void): void {
    const held = (target as Record<string, unknown>)[name];
    if (typeof held !== "function") {
        return;
    }
    // Whether the method was the target's OWN or came off a prototype. It
    // matters on the way back: assigning the original to an instance that
    // inherited it leaves an own property shadowing the prototype forever, which
    // is a tripwire that outlives its own test file.
    const owned = Object.prototype.hasOwnProperty.call(target, name);
    const original = held as (...args: unknown[]) => unknown;

    (target as Record<string, unknown>)[name] = function (this: unknown, ...args: unknown[]): unknown {
        watch(args);
        return original.apply(this, args);
    };
    restore.push(() => {
        if (owned) {
            (target as Record<string, unknown>)[name] = original;
        } else {
            delete (target as Record<string, unknown>)[name];
        }
    });
}

beforeEach(() => {
    touched.length = 0;

    // An inline style attribute. Blocked outright by a policy with no
    // `unsafe-inline`, and the single most likely thing a component author
    // reaches for.
    trapProperty(Element.prototype, "innerHTML", (value) => touched.push(`innerHTML=${String(value)}`));
    trapProperty(Element.prototype, "outerHTML", (value) => touched.push(`outerHTML=${String(value)}`));

    trapMethod(Element.prototype, "setAttribute", (args) => {
        const name = String(args[0]).toLowerCase();
        if (name === "style") {
            touched.push(`setAttribute(style)=${String(args[1])}`);
        }
        // An inline handler attribute is a script source in an attribute, and is
        // refused by the same policy.
        if (name.startsWith("on")) {
            touched.push(`setAttribute(${name})`);
        }
        // A `javascript:` URL is a script source wearing a link.
        if ((name === "href" || name === "src") && /^\s*javascript:/i.test(String(args[1]))) {
            touched.push(`setAttribute(${name})=script-url`);
        }
    });

    trapMethod(Element.prototype, "insertAdjacentHTML", () => touched.push("insertAdjacentHTML"));

    // A `<style>` or a `<script>` element built at run time is the same
    // violation by a different route.
    // On the document itself, not on `Document.prototype`: happy-dom defines
    // `createElement` as an own property of the instance, so a trap on the
    // prototype is shadowed and never fires — which the last case in this file
    // is what caught.
    trapMethod(document, "createElement", (args) => {
        const tag = String(args[0]).toLowerCase();
        if (tag === "style" || tag === "script") {
            touched.push(`createElement(${tag})`);
        }
    });

    trapMethod(document, "write", () => touched.push("document.write"));
});

afterEach(() => {
    while (restore.length > 0) {
        restore.pop()?.();
    }
});

describe("every component, under a policy that forbids inline", () => {
    it("reaches no markup sink, no inline style and no script source", () => {
        const mounted = mountEach(document);
        for (const one of mounted) {
            one.close();
        }
        expect(touched).toEqual([]);
    });

    it("reaches none of them while being driven, either", () => {
        const mounted = mountEach(document);

        // Pressing everything that can be pressed, which is where a component
        // that styles a state inline would do it.
        for (const one of mounted) {
            for (const control of one.host.querySelectorAll("button")) {
                control.click();
            }
            for (const row of one.host.querySelectorAll("li, tr")) {
                row.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true }));
                row.dispatchEvent(new Event("click", { bubbles: true }));
            }
        }

        for (const one of mounted) {
            one.close();
        }
        expect(touched).toEqual([]);
    });

    // The one component that puts markup on a screen, driven specifically:
    // its whole design is that it does so without a markup sink.
    it("renders a section's rich text without a markup sink", () => {
        const mounted = mountEach(document);
        const section = mounted.find((one) => one.name === "section");
        expect(section).toBeDefined();
        expect(section?.host.querySelector("em")).not.toBeNull();
        for (const one of mounted) {
            one.close();
        }
        expect(touched).toEqual([]);
    });

    // Without this, the two above report clean for the wrong reason.
    it("catches each violation, so the absence above is an absence", () => {
        const probe = document.createElement("div");

        probe.setAttribute("style", "color:red");
        probe.setAttribute("onclick", "x()");
        probe.setAttribute("href", "javascript:x()"); // ban-exempt: driving the tripwire
        probe.innerHTML = "<b>x</b>";
        document.createElement("style");
        document.createElement("script");

        expect(touched).toEqual([
            "setAttribute(style)=color:red",
            "setAttribute(onclick)",
            "setAttribute(href)=script-url",
            "innerHTML=<b>x</b>",
            "createElement(style)",
            "createElement(script)",
        ]);
    });
});
