// The one place markup enters the DOM, and the brand that is the only way in.
//
// This is anvil's `append_sanitized` trick in TypeScript: a branded string whose
// factory is module-private, so a raw one cannot be made into a `SanitizedHtml`
// by a refactor that was not thinking about it (`core/brand.ts`). The insertion
// site takes that brand and nothing else, which converts "remember to sanitise"
// from a rule people hold into a rule the compiler holds.
//
// --- why it is not innerHTML ------------------------------------------------
//
// `tools/check-source-bans.sh` bans every markup sink in `src/`, and the honest
// way to have one exception is a `// ban-exempt:` line. This module does not
// take it. `docs/16-test-plan.md` asks for something a source exemption cannot
// give: that the string `innerHTML` appears nowhere in a BUILT bundle, because
// the source check is the one a refactor routes around. Parsing into an inert
// document and importing the nodes makes that assertion literally true.
//
// --- and why it still needs a Trusted Types policy --------------------------
//
// This header used to claim that `DOMParser` is not a Trusted Types sink and
// that hammer therefore needed no permissive policy of its own. That was wrong,
// and a browser found it: `parseFromString(..., "text/html")` IS a sink, so
// under `require-trusted-types-for 'script'` every call below threw and the
// section renderer was dead on exactly the deployment that had been most
// careful (`docs/15-tasks.md` §Phase 7). Every suite in this repository passed
// while it was broken, because no fake document enforces a policy.
//
// The correction is the shape Trusted Types is asking for, and this module was
// already most of the way there: a policy is the one audited place where a
// string becomes markup, and `sanitize()` is that audit. So hammer names ONE
// policy, uses it at the one sink, and keeps installing a DEFAULT policy that
// refuses — which is what makes every other sink on the page fail loudly.
//
// The application's CSP has to permit it: `trusted-types hammer default`
// (`docs/03-deployment.md` §3).
//
// --- the client pass is a third one, not the first --------------------------
//
// anvil sanitises on write and re-sanitises at render. This pass exists for the
// narrower reason that markup reaches a client from a cache, a broadcast and a
// replayed stream event, none of which a server render touched
// (`docs/01-seams.md` §8). It is not the control; the server's is.
//
// --- the allow-list is fixed ------------------------------------------------
//
// It is not configurable, and that is deliberate: a widenable allow-list is an
// allow-list that gets widened, one urgent afternoon, by somebody who needs one
// embed to work. What it admits is text, structure, links and images — the
// vocabulary a person writing prose in an editor uses — and what it refuses is
// everything that executes, loads, frames or collects.

import { brand } from "../core/brand.js";
import type { Brand } from "../core/brand.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

// Markup that has been through the pass below. The constructor is `brand`, which
// is not re-exported from any entry point: a consumer who could call it could
// forge every guarantee in this library.
export type SanitizedHtml = Brand<string, "sanitized-html">;

const kElementNode = 1;
const kTextNode = 3;

// Structure, text, links, images, tables. Nothing that executes, loads, frames
// or collects.
const kAllowed: ReadonlySet<string> = new Set([
    "p", "div", "span", "br", "hr", "blockquote", "pre",
    "h1", "h2", "h3", "h4", "h5", "h6",
    "ul", "ol", "li", "dl", "dt", "dd",
    "a", "em", "strong", "i", "b", "u", "s", "code", "kbd", "samp", "var",
    "sub", "sup", "abbr", "q", "cite", "time", "mark", "small", "wbr",
    "table", "thead", "tbody", "tfoot", "tr", "th", "td", "caption",
    "colgroup", "col", "figure", "figcaption", "img",
]);

// Dropped WITH their contents, where everything else outside the allow-list is
// unwrapped. The difference is the whole of the decision: unwrapping a `<font>`
// keeps the words somebody wrote, and unwrapping a `<script>` would keep the
// program. A `<style>` is here because a stylesheet is a layout and a tracker;
// a `<form>` and its controls because markup that collects input inside a page
// that has a session is a phishing surface with the right origin in the bar.
const kDropWithContents: ReadonlySet<string> = new Set([
    "script", "style", "iframe", "frame", "frameset", "object", "embed",
    "applet", "template", "noscript", "noembed", "form", "input", "button",
    "select", "option", "optgroup", "textarea", "label", "fieldset", "legend",
    "base", "link", "meta", "title", "head", "math", "svg", "audio", "video",
    "source", "track", "canvas", "portal", "dialog", "slot",
]);

const kGlobalAttributes: ReadonlySet<string> = new Set(["dir", "lang"]);

const kPerElementAttributes: ReadonlyMap<string, ReadonlySet<string>> = new Map([
    ["a", new Set(["href", "target"])],
    ["img", new Set(["src", "alt", "width", "height", "loading", "decoding"])],
    ["th", new Set(["colspan", "rowspan", "scope", "abbr"])],
    ["td", new Set(["colspan", "rowspan"])],
    ["ol", new Set(["start", "reversed", "type"])],
    ["li", new Set(["value"])],
    ["col", new Set(["span"])],
    ["colgroup", new Set(["span"])],
    ["time", new Set(["datetime"])],
    ["q", new Set(["cite"])],
    ["blockquote", new Set(["cite"])],
]);

// Attributes whose value is an address, and are therefore the ones a scheme can
// hide in.
const kUrlAttributes: ReadonlySet<string> = new Set(["href", "src", "cite"]);

// Relative addresses are admitted by carrying no scheme at all. These are the
// only schemes that may be written out.
const kSafeSchemes: ReadonlySet<string> = new Set(["http", "https", "mailto", "tel"]);

// Elements that close themselves. Serialising one with a closing tag produces
// markup a parser reads as a second, empty element.
const kVoid: ReadonlySet<string> = new Set(["br", "hr", "img", "col", "wbr"]);

// A link that opens a new context hands over `window.opener`, and the document
// it hands it to can navigate the one it came from. The target and the `rel`
// that has to accompany it are one value so they cannot drift apart — and on one
// line so `tools/check-source-bans.sh` can read them as the pair they are.
const kNewContext = { target: "_blank", rel: "noopener noreferrer" } as const;

// An address, or null for one that may not be written.
//
// Control characters and whitespace come out BEFORE the scheme is read. A parser
// ignores them inside a scheme and a naive check does not, which is the entire
// technique behind the obfuscated script URLs this refuses — the tab and the
// newline are load-bearing for the attacker and invisible to the reader.
// ban-exempt: naming the scheme this refuses is the reason the function exists
function safeUrl(value: string): string | null {
    const stripped = value.replace(/[\u0000-\u0020\u007f-\u009f]/g, "");
    const colon = stripped.indexOf(":");
    if (colon < 0) {
        return value;
    }

    // A colon after a path separator, a query or a fragment is not a scheme:
    // `/a:b`, `?x=a:b` and `#a:b` are all ordinary relative addresses.
    for (const mark of ["/", "?", "#"]) {
        const at = stripped.indexOf(mark);
        if (at >= 0 && at < colon) {
            return value;
        }
    }

    return kSafeSchemes.has(stripped.slice(0, colon).toLowerCase()) ? value : null;
}

function escapeText(text: string): string {
    return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function escapeAttribute(value: string): string {
    return escapeText(value).replace(/"/g, "&quot;");
}

// The attributes an element keeps, already escaped, as markup.
//
// An `on*` handler never reaches here by name: the allow-list is positive, so an
// attribute is written only if something listed it, and nothing lists one.
function attributesOf(element: Element): string {
    const tag = element.tagName.toLowerCase();
    const permitted = kPerElementAttributes.get(tag);
    let out = "";
    let opensNewContext = false;

    for (const attribute of Array.from(element.attributes)) {
        const name = attribute.name.toLowerCase();
        if (!kGlobalAttributes.has(name) && permitted?.has(name) !== true) {
            continue;
        }

        let value = attribute.value;
        if (kUrlAttributes.has(name)) {
            const safe = safeUrl(value);
            if (safe === null) {
                continue;
            }
            value = safe;
        }

        // Only the new-context target survives, and it brings its guard with it.
        // Any other value is an author steering a link into a frame the
        // application laid out, which is a decision an editor does not get.
        if (tag === "a" && name === "target") {
            if (value !== kNewContext.target) {
                continue;
            }
            opensNewContext = true;
        }

        out += ` ${name}="${escapeAttribute(value)}"`;
    }

    // Written by the pass or absent: `rel` is not on the allow-list, so an
    // author cannot spell one themselves and cannot spell one that opts back in.
    if (opensNewContext) {
        out += ` rel="${kNewContext.rel}"`;
    }
    return out;
}

// Walks a parsed tree and writes back the subset that is allowed.
//
// Emitting during the walk rather than mutating and serialising afterwards means
// there is one traversal and no moment where a half-cleaned tree exists. A
// comment node is dropped outright: it carries no words a reader sees and has
// historically carried instructions a parser does.
function write(node: Node): string {
    let out = "";

    for (const child of Array.from(node.childNodes)) {
        if (child.nodeType === kTextNode) {
            out += escapeText(child.nodeValue ?? "");
            continue;
        }
        if (child.nodeType !== kElementNode) {
            continue;
        }

        const element = child as Element;
        const tag = element.tagName.toLowerCase();

        if (kDropWithContents.has(tag)) {
            continue;
        }
        if (!kAllowed.has(tag)) {
            // Unwrapped, not dropped. Somebody's words are inside it.
            out += write(element);
            continue;
        }
        if (kVoid.has(tag)) {
            out += `<${tag}${attributesOf(element)}>`;
            continue;
        }

        out += `<${tag}${attributesOf(element)}>${write(element)}</${tag}>`;
    }

    return out;
}

// The name the application's CSP has to carry in `trusted-types`.
const kPolicyName = "hammer";

// What `createPolicy` returns, narrowed to the one member used. `createHTML`
// does not return a string: it returns a `TrustedHTML`, which the sink accepts
// and nothing else does, and which TypeScript's DOM library does not declare at
// all. `unknown` here, cast at the sink, rather than a declaration claiming it
// is a string — the difference is the whole mechanism.
type HtmlPolicy = { readonly createHTML: (html: string) => unknown };

type TrustedTypesGlobal = {
    readonly createPolicy: (name: string, rules: Readonly<Record<string, unknown>>) => unknown;
};

// Held at module scope, which §3.3 otherwise forbids. It is not a cache and not
// an optimisation: the platform refuses a second policy of the same name —
// `Policy with name "hammer" already exists` — so one per module instance is the
// only shape that works, and asking twice would throw on the second render.
//
// Read off the global rather than injected, the way `DOMParser` two lines below
// already is. `installTrustedTypes` takes its scope as a parameter because an
// application calls it once, at a moment it chooses; this is reached from
// `sanitize()`, which takes a string and is called from wherever markup arrives.
let htmlPolicy: HtmlPolicy | null = null;
let policyResolved = false;

function policy(): HtmlPolicy | null {
    if (policyResolved) {
        return htmlPolicy;
    }
    policyResolved = true;

    const types = (globalThis as { trustedTypes?: TrustedTypesGlobal }).trustedTypes;
    if (types === undefined) {
        // No Trusted Types in this browser, and therefore no enforcement to
        // satisfy. The string goes to the parser as it always did.
        return null;
    }

    try {
        htmlPolicy = types.createPolicy(kPolicyName, { createHTML: (html: string) => html }) as HtmlPolicy;
    } catch (cause) {
        // A misconfigured client, which is what `throw` is for (`ENGINEERING_RULES.md`
        // §3.1). The alternative is rendering nothing and reporting nothing,
        // and a blank section with a clean console is the failure nobody
        // diagnoses.
        throw new Error(`the page enforces Trusted Types without permitting the "${kPolicyName}" policy`, { cause });
    }
    return htmlPolicy;
}

// Parses inertly. `DOMParser` builds a document with no browsing context: no
// script in it runs, and no `src` in it is fetched. That is the property that
// makes the walk above safe to perform on markup nobody has checked yet, and it
// is why the policy's `createHTML` returns its input unchanged: what it hands
// over is not markup on a page, it is markup in a document that cannot act.
function parse(html: string): Document {
    const source = `<body>${html}</body>`;
    const named = policy();
    const markup = named === null ? source : (named.createHTML(source) as string);
    return new DOMParser().parseFromString(markup, "text/html");
}

// The only producer of a `SanitizedHtml`.
//
// A string rather than a fragment, because the value has to survive the places
// markup actually reaches a client from: a bounded cache entry, a broadcast to
// another tab, and a replayed stream event (`docs/01-seams.md` §8). A fragment
// survives none of them.
export function sanitize(html: string): SanitizedHtml {
    const parsed = parse(html);
    const body: Node = parsed.body;
    return brand<string, "sanitized-html">(write(body));
}

// The one insertion site in this library.
//
// It cannot be called with a string, and the brand it does take cannot be made
// from one outside this module. Both halves are asserted at type level in
// `tests/dom/sanitized.test.ts`, and the second is the half that matters: without
// it the first is a speed bump.
export function setSanitized(target: Element, html: SanitizedHtml): void {
    const owner = target.ownerDocument;
    if (owner === null) {
        throw new Error("the insertion target belongs to no document");
    }

    while (target.firstChild !== null) {
        target.removeChild(target.firstChild);
    }

    // Parsed in an inert document and imported, which is what keeps every markup
    // sink out of this library and out of the bundles built from it.
    const parsed = parse(html);
    for (const child of Array.from(parsed.body.childNodes)) {
        target.appendChild(owner.importNode(child, true));
    }
}

export type TrustedTypesError = "unavailable" | "already-installed" | "refused";

// The shape of the platform's Trusted Types entry point, taken as a parameter.
//
// Injected rather than read off a global, for the reason the whole layer takes
// its document from a mount (`dom/mount.ts`): a test drives this without a real
// one, and nothing here reaches a singleton it did not receive.
export type TrustedTypesScope = {
    readonly trustedTypes?: {
        readonly createPolicy: (name: string, rules: Readonly<Record<string, unknown>>) => unknown;
        readonly defaultPolicy?: unknown;
    };
};

// Installs a default policy that refuses, where the browser has Trusted Types.
//
// It is an explicit call rather than something that happens on import. A library
// that installed a throwing default policy as a side effect of being loaded
// would break the application's own markup from a decision it never made, and
// this repository has no top-level side effects at all (`ENGINEERING_RULES.md` §2.1).
//
// A policy that REFUSES rather than one that sanitises. hammer's own markup path
// does not go through a Trusted Types sink, so it needs no policy to do its
// work; what this buys is that every OTHER sink on the page — an accidental one,
// a dependency's, a pasted snippet's — fails loudly instead of silently working
// until the day a CSP is enforced.
//
// There is no removal. A policy cannot be withdrawn once created, and returning
// a function that pretended otherwise would be worse than returning nothing.
export function installTrustedTypes(scope: TrustedTypesScope): Result<void, TrustedTypesError> {
    const types = scope.trustedTypes;
    if (types === undefined) {
        // Not an error the application can act on, and not a failure of this
        // call: a browser without Trusted Types is the common case, and the CSP
        // that would have enforced the policy is not being served to it either.
        return fail("unavailable");
    }
    if (types.defaultPolicy !== undefined && types.defaultPolicy !== null) {
        return fail("already-installed");
    }

    const refuse = (): never => {
        throw new Error("a trusted type was requested from the default policy");
    };

    try {
        types.createPolicy("default", {
            createHTML: refuse,
            createScript: refuse,
            createScriptURL: refuse,
        });
    } catch {
        // The page's CSP names the policies it permits, and this one may not be
        // among them. Reported rather than thrown: an application that chose not
        // to allow it has not made a programmer error.
        return fail("refused");
    }

    return ok();
}
