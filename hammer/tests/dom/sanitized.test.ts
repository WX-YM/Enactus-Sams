// @vitest-environment happy-dom
//
// The one insertion site, and the brand that is the only way into it.
//
// Half of this file is type-level, and that half is the point: it asserts what
// cannot be WRITTEN. The second of the two type assertions is the one that
// matters — without it the first is a speed bump, which is the same reasoning
// anvil gives for closing `SanitizedHtml`'s constructor.
//
// The rest drives the allow-list against the markup it exists for. Every case
// below is a thing an editor, a paste buffer or an attacker actually produces;
// none of them is hypothetical, and the obfuscated-scheme cases are the reason
// the address check strips before it reads.

import { describe, expect, it, vi } from "vitest";

import { installTrustedTypes, sanitize, setSanitized } from "../../src/dom/sanitized.js";
import type { SanitizedHtml } from "../../src/dom/sanitized.js";

function into(html: string): HTMLElement {
    const host = document.createElement("div");
    setSanitized(host, sanitize(html));
    return host;
}

// --- a divergence this environment has from a browser ------------------------
//
// `DOMParser.parseFromString(html, "text/html")` yields a document with NO
// browsing context, so a browser neither runs a script in it nor fetches a `src`
// from it. That inertness is what makes it safe to walk markup nobody has
// checked. happy-dom does not implement it that way: its parsed document is
// attached to a window, and a `<script>` in one executes the moment the parser
// appends it — during `sanitize()`, before the walk has had a chance to drop it.
//
// So the dangerous cases below assert the STRING `sanitize` produces rather than
// round-tripping through `setSanitized`. That is the actual contract and it is
// the same assertion in every environment. Their payloads are inert by content
// as well, because a test that trips the host's script runner is a test that
// fails for a reason that has nothing to do with the code.
//
// The property itself — that a real parser runs nothing — cannot be asserted
// from here and is a live-run row in `docs/15-tasks.md` §Phase 7, alongside the
// Trusted Types run. A unit test of an environment that gets this wrong is worth
// nothing either way.
const kInertBody = "void 0";

describe("what the brand refuses to be", () => {
    it("is not constructible from a string", () => {
        // @ts-expect-error the brand asserts the value went through the pass,
        // and a plain string has not. This is the assertion that makes the one
        // below worth anything.
        const forged: SanitizedHtml = "<p>trusted</p>";
        expect(typeof forged).toBe("string");
    });

    it("is not constructible from bytes", () => {
        // @ts-expect-error the same hole from the other direction: a value
        // assembled somewhere else is a value nothing sanitised.
        const forged: SanitizedHtml = new TextDecoder().decode(new Uint8Array([60, 112, 62]));
        expect(typeof forged).toBe("string");
    });

    it("does not let the insertion site be called with a string", () => {
        const host = document.createElement("div");
        // @ts-expect-error the site takes the brand and nothing else. A string
        // reaching it is the defect this whole module exists to make unspellable.
        setSanitized(host, "<p>trusted</p>");
        expect(host).toBeTruthy();
    });
});

describe("what survives the pass", () => {
    it("keeps the structure somebody wrote", () => {
        const host = into("<p>a <strong>bold</strong> word</p><ul><li>one</li></ul>");
        expect(host.querySelectorAll("p").length).toBe(1);
        expect(host.querySelector("strong")?.textContent).toBe("bold");
        expect(host.querySelector("li")?.textContent).toBe("one");
    });

    it("keeps a table whole", () => {
        const host = into("<table><tr><th scope='col'>h</th><td colspan='2'>c</td></tr></table>");
        expect(host.querySelector("th")?.getAttribute("scope")).toBe("col");
        expect(host.querySelector("td")?.getAttribute("colspan")).toBe("2");
    });

    it("keeps the direction and language an author set", () => {
        const host = into('<p dir="rtl" lang="ar">مرحبا</p>');
        expect(host.querySelector("p")?.getAttribute("dir")).toBe("rtl");
        expect(host.querySelector("p")?.getAttribute("lang")).toBe("ar");
    });

    it("keeps a relative address", () => {
        const host = into('<a href="/about">about</a>');
        expect(host.querySelector("a")?.getAttribute("href")).toBe("/about");
    });

    // A colon that is not a scheme. Refusing these would break ordinary links,
    // which is how an allow-list gets widened by somebody in a hurry.
    it("keeps a colon that is not a scheme", () => {
        for (const href of ["/a:b", "?x=a:b", "#a:b"]) {
            const host = into(`<a href="${href}">x</a>`);
            expect(host.querySelector("a")?.getAttribute("href")).toBe(href);
        }
    });
});

describe("what does not", () => {
    it("drops a script with its contents", () => {
        const out = sanitize(`<p>before</p><script>${kInertBody}</script><p>after</p>`);
        expect(out).toBe("<p>before</p><p>after</p>");
    });

    it("drops a style, which is a layout and a tracker", () => {
        expect(sanitize("<style>p{color:red}</style><p>x</p>")).toBe("<p>x</p>");
    });

    // Markup that collects input inside a page that holds a session is a
    // phishing surface with the right origin in the address bar.
    it("drops a form and its controls", () => {
        const out = sanitize('<form action="/collect"><input name="password"></form><p>x</p>');
        expect(out).toBe("<p>x</p>");
    });

    it("drops a frame", () => {
        expect(sanitize("<iframe></iframe><p>x</p>")).toBe("<p>x</p>");
    });

    it("drops an event handler attribute", () => {
        const host = into('<p onclick="alert(1)" onmouseover="alert(2)">x</p>');
        const paragraph = host.querySelector("p");
        expect(paragraph?.getAttribute("onclick")).toBeNull();
        expect(paragraph?.getAttribute("onmouseover")).toBeNull();
        expect(paragraph?.textContent).toBe("x");
    });

    // Unwrapped rather than dropped: somebody's words are inside it.
    it("unwraps an element it does not know, keeping the words", () => {
        const host = into("<p>a <font size='7'>big</font> word</p>");
        expect(host.querySelector("font")).toBeNull();
        expect(host.querySelector("p")?.textContent).toBe("a big word");
    });

    it("drops a comment", () => {
        expect(sanitize("<p>a</p><!-- a conditional comment --><p>b</p>")).toBe("<p>a</p><p>b</p>");
    });

    it("drops an id, which is the application's namespace and not an author's", () => {
        const host = into('<p id="main">x</p>');
        expect(host.querySelector("p")?.getAttribute("id")).toBeNull();
    });
});

describe("an address that hides a scheme", () => {
    // The tab and the newline are load-bearing for the attacker and invisible to
    // the reader, which is why the value is stripped before the scheme is read.
    it("refuses one broken up by control characters", () => {
        const hidden = ["java\tscript:alert(1)", "java\nscript:alert(1)", " javascript:alert(1)"];
        for (const href of hidden) {
            const host = into(`<a href="${href}">x</a>`);
            expect(host.querySelector("a")?.getAttribute("href")).toBeNull();
        }
    });

    it("refuses one in mixed case", () => {
        const host = into('<a href="JaVaScRiPt:alert(1)">x</a>');
        expect(host.querySelector("a")?.getAttribute("href")).toBeNull();
    });

    // A data URL is a document with its own contents, and an anchor to one is a
    // navigation to markup nobody sanitised.
    it("refuses a data address", () => {
        expect(sanitize('<a href="data:text/html,PHA+eDwvcD4=">x</a>')).toBe("<a>x</a>");
    });

    it("refuses one on an image source", () => {
        const host = into('<img src="javascript:alert(1)" alt="x">');
        expect(host.querySelector("img")?.getAttribute("src")).toBeNull();
        expect(host.querySelector("img")?.getAttribute("alt")).toBe("x");
    });
});

// A link that opens a new context hands over `window.opener`, and the document
// it hands it to can navigate the one it came from.
describe("a link that opens a new context", () => {
    it("carries the pair that closes the opener", () => {
        const host = into('<a href="https://example.test" target="_blank">x</a>');
        expect(host.querySelector("a")?.getAttribute("rel")).toBe("noopener noreferrer");
    });

    it("drops a target that is not a new context", () => {
        const host = into('<a href="/x" target="_top">x</a>');
        const link = host.querySelector("a");
        expect(link?.getAttribute("target")).toBeNull();
        expect(link?.getAttribute("rel")).toBeNull();
    });

    // An author cannot spell the pair themselves, because `rel` is not on the
    // allow-list: it is written by the pass or it is absent.
    it("does not take an author's own rel", () => {
        const host = into('<a href="/x" rel="opener">x</a>');
        expect(host.querySelector("a")?.getAttribute("rel")).toBeNull();
    });
});

describe("the escaping", () => {
    it("makes text that looks like markup stay text", () => {
        const host = into("<p>&lt;b&gt;not bold&lt;/b&gt;</p>");
        expect(host.querySelector("b")).toBeNull();
        expect(host.querySelector("p")?.textContent).toBe("<b>not bold</b>");
    });

    it("survives being passed through twice", () => {
        // The value is a string precisely so it can be cached, broadcast and
        // replayed. Re-sanitising one must not double-escape it, or a quoted
        // word grows an `&amp;` every time it is read from a cache.
        const once = sanitize("<p>a &amp; b &lt; c</p>");
        const twice = sanitize(once);
        expect(twice).toBe(once);

        const host = document.createElement("div");
        setSanitized(host, twice);
        expect(host.querySelector("p")?.textContent).toBe("a & b < c");
    });

    it("keeps a quote out of an attribute's own delimiter", () => {
        const host = into('<a href=\'/a"onmouseover="alert(1)\'>x</a>');
        const link = host.querySelector("a");
        expect(link?.getAttribute("onmouseover")).toBeNull();
    });
});

describe("the insertion site", () => {
    it("replaces what was there rather than appending to it", () => {
        const host = document.createElement("div");
        setSanitized(host, sanitize("<p>first</p>"));
        setSanitized(host, sanitize("<p>second</p>"));
        expect(host.querySelectorAll("p").length).toBe(1);
        expect(host.textContent).toBe("second");
    });

    // The nodes are imported into the target's own document, which is what lets
    // a component render into a preview or a frame (`dom/mount.ts`).
    it("imports into the document the target belongs to", () => {
        const other = new DOMParser().parseFromString("<main></main>", "text/html");
        const target = other.querySelector("main");
        expect(target).not.toBeNull();
        if (target === null) {
            return;
        }
        setSanitized(target, sanitize("<p>x</p>"));
        expect(target.querySelector("p")?.ownerDocument).toBe(other);
    });
});

describe("the Trusted Types policy", () => {
    it("installs one that refuses every sink", () => {
        const made: { name: string; rules: Record<string, unknown> }[] = [];
        const result = installTrustedTypes({
            trustedTypes: {
                createPolicy: (name, rules) => {
                    made.push({ name, rules: rules as Record<string, unknown> });
                    return {};
                },
            },
        });

        expect(result.ok).toBe(true);
        expect(made.length).toBe(1);
        expect(made[0]?.name).toBe("default");

        // Refusing rather than sanitising. hammer's own parse goes through its
        // OWN named policy (below), so the default one exists purely to make
        // every OTHER sink on the page fail loudly instead of silently working
        // until a CSP is enforced.
        for (const sink of ["createHTML", "createScript", "createScriptURL"]) {
            const refuse = made[0]?.rules[sink];
            expect(typeof refuse).toBe("function");
            expect(() => (refuse as () => unknown)()).toThrow();
        }
    });

    // The common case, and not a failure of the call: a browser without Trusted
    // Types is not being served the CSP that would have enforced the policy.
    it("reports a platform that has none", () => {
        expect(installTrustedTypes({})).toEqual({ ok: false, error: "unavailable" });
    });

    it("refuses to replace a policy somebody already installed", () => {
        const result = installTrustedTypes({
            trustedTypes: { createPolicy: () => ({}), defaultPolicy: {} },
        });
        expect(result).toEqual({ ok: false, error: "already-installed" });
    });

    // The page's CSP names the policies it permits and this one may not be among
    // them. An application that chose not to allow it has not made a programmer
    // error, so it is reported rather than thrown.
    it("reports a policy the page's own CSP refused", () => {
        const result = installTrustedTypes({
            trustedTypes: {
                createPolicy: () => {
                    throw new Error("the policy is not on the allow-list");
                },
            },
        });
        expect(result).toEqual({ ok: false, error: "refused" });
    });
});

// --- the policy the parse itself needs --------------------------------------
//
// `DOMParser.parseFromString(..., "text/html")` is a Trusted Types sink. This
// module's header denied that until a real browser was pointed at it, and the
// consequence was that every rich-text render threw on any page enforcing
// `require-trusted-types-for 'script'` — the deployment that had been most
// careful. `tests/browser/trusted_types.test.ts` is what found it and is what
// keeps the claim honest; these cases cover the branches a browser cannot be
// asked to produce on demand.
//
// Each one re-imports the module, because the policy is resolved once per module
// instance: the platform refuses a second policy of the same name, so asking
// twice is not something the code may do.

type PolicyRules = { readonly createHTML: (html: string) => string };

async function freshSanitizer(
    trustedTypes: { readonly createPolicy: (name: string, rules: PolicyRules) => unknown } | undefined,
): Promise<typeof import("../../src/dom/sanitized.js")> {
    const scope = globalThis as { trustedTypes?: unknown };
    if (trustedTypes === undefined) {
        delete scope.trustedTypes;
    } else {
        scope.trustedTypes = trustedTypes;
    }
    vi.resetModules();
    return import("../../src/dom/sanitized.js");
}

describe("the policy the sanitiser's own parse needs", () => {
    it("creates exactly one, named for this library, and parses through it", async () => {
        const made: string[] = [];
        const seen: string[] = [];

        const fresh = await freshSanitizer({
            createPolicy: (name, rules) => {
                made.push(name);
                return {
                    createHTML: (html: string) => {
                        seen.push(html);
                        return rules.createHTML(html);
                    },
                };
            },
        });

        try {
            expect(String(fresh.sanitize("<p>one</p>"))).toBe("<p>one</p>");

            // A SECOND parse, which is the property that matters: the platform
            // refuses a duplicate policy name, so a module that asked again
            // would throw on every render after the first.
            expect(String(fresh.sanitize("<p>two</p>"))).toBe("<p>two</p>");

            expect(made).toEqual(["hammer"]);
            expect(seen.length).toBe(2);

            // What reaches the sink is what the parse was going to send, wrapped
            // the way the parse wraps it. A policy that received something else
            // would be auditing a different string from the one parsed.
            expect(seen[0]).toContain("<p>one</p>");
        } finally {
            delete (globalThis as { trustedTypes?: unknown }).trustedTypes;
            vi.resetModules();
        }
    });

    it("throws, naming the policy, on a page that enforces without permitting it", async () => {
        const fresh = await freshSanitizer({
            createPolicy: () => {
                throw new Error("not on the allow-list");
            },
        });

        try {
            // A misconfigured client is what `throw` is for (`ENGINEERING_RULES.md` §3.1).
            // The alternative is a blank section and a clean console, which is
            // the failure nobody diagnoses.
            expect(() => fresh.sanitize("<p>x</p>")).toThrow(/hammer/);
        } finally {
            delete (globalThis as { trustedTypes?: unknown }).trustedTypes;
            vi.resetModules();
        }
    });

    it("asks for none where the platform has none", async () => {
        const fresh = await freshSanitizer(undefined);
        try {
            // Every other browser, and every other suite in this repository.
            // The string goes to the parser exactly as it always did.
            expect(String(fresh.sanitize("<p>x</p>"))).toBe("<p>x</p>");
        } finally {
            vi.resetModules();
        }
    });
});
