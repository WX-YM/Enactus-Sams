//
// Every component under a policy with no `unsafe-inline` and no `unsafe-eval`.
//
// This page runs under the real policy now (`tests/dom/in_browser.test.ts`
// serves it under the same CSP `tests/browser/harness.ts` does), which is a
// second, independent proof of the same absence Chromium itself enforces. The
// tripwires below are still worth keeping rather than deleting now that a real
// browser is watching too: they name the EXACT call a component would have to
// make to break the policy, driving the whole layer through them, and — the
// part that makes it worth anything — driving the tripwire itself at the end.
// A check that cannot fail reports clean for the wrong reason, the same lesson
// `tests/state/persistence.test.ts` applies to asserting nothing is persisted.
//
// The policy this stands in for is the application's (`docs/00-architecture.md`
// §8). What it costs to get wrong is not an error: a component that sets an
// inline style passes every other suite in this repository and is silently blank
// in production.

import { afterEach, beforeEach, describe, expect, it } from "../support/test.js";

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
    // On the document itself, not on `Document.prototype`: happy-dom used to
    // define `createElement` as an own property of the instance, which
    // shadowed a trap on the prototype and never fired it — caught by the
    // last case in this file, back when that fake DOM was what this suite
    // ran on. A real browser defines it on the prototype and either shape
    // works with an instance-level trap, so this stays where it is rather
    // than moving now that the reason for it is history.
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

        // Each attempt is wrapped rather than called bare: a real browser
        // enforcing Trusted Types does not just report `onclick` and
        // `innerHTML` as violations, it REFUSES them outright — a TypeError
        // at the sink, same as `tests/browser/trusted_types.test.ts`'s own
        // probe. happy-dom enforced neither, so this used to run all six
        // straight through. The trap above records an attempt before the
        // real call runs, which is what this test is checking; whether
        // Chromium then also refuses the write is its own enforcement, not
        // this test's concern, so the refusal is swallowed rather than
        // asserted on here.
        const attempt = (write: () => void): void => {
            try {
                write();
            } catch {
                // deliberately swallowed — see above.
            }
        };

        attempt(() => probe.setAttribute("style", "color:red"));
        attempt(() => probe.setAttribute("onclick", "x()"));
        attempt(() => probe.setAttribute("href", "javascript:x()")); // ban-exempt: driving the tripwire
        attempt(() => {
            probe.innerHTML = "<b>x</b>";
        });
        attempt(() => {
            document.createElement("style");
        });
        attempt(() => {
            document.createElement("script");
        });

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
