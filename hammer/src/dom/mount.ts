// The shape every component in this layer has, and the one way it lets go.
//
// Eleven components need the same four things — a handle their owner can close,
// a document to build in, a class name applied from the application's table, and
// a subscription that is released when the component is. One module holds them,
// for the reason `state/counts.ts` holds one union of names: five modules each
// declaring their own is five things to keep agreeing with each other.
//
// --- the document is injected, and that is enforced -------------------------
//
// A component takes its document from the element it was asked to mount in, and
// never from the global. `tools/check-layering.sh` fails the build on a bare
// `document.` in this layer for the same reason it does in the three below it:
// the platform's singletons are injected so a test supplies its own rather than
// racing every other test in the file (`ENGINEERING_RULES.md` §3.3). Here it buys a second
// thing — a component that reaches the global cannot be rendered into a document
// that is not the tab's, which is what an editor preview and a print view both
// are.
//
// --- closing is not optional ------------------------------------------------
//
// `Mounted.close` is `[[nodiscard]]` in spirit (`ENGINEERING_RULES.md` §3.3). A component
// subscribes to stores, opens resources that are refcounted and may hold a timer;
// a caller that drops the handle leaks all three, in a tab that stays open for
// days. Every component built here routes its cleanup through `Closers`, so
// there is one list and it runs once.

import { detectDirection, isolate } from "../core/bidi.js";

import type { Readable, Unsubscribe } from "../state/store.js";

// What a render hands back. `element` is the node it created, so a caller can
// place it, label something else by its id, or measure it; `close` is the whole
// of its lifetime.
export type Mounted = {
    readonly element: HTMLElement;
    readonly close: () => void;
};

// The document a component builds in.
//
// A throw rather than a `Result`, and it is one of the few places that is true:
// an element with no owner document is a caller that passed something it built
// wrong, which is programmer error (`ENGINEERING_RULES.md` §3.1). The alternative — falling
// back to the global — is exactly the reach this layer is not allowed to make.
export function documentOf(mount: Element): Document {
    const owner = mount.ownerDocument;
    if (owner === null) {
        throw new Error("the mount point belongs to no document");
    }
    return owner;
}

// An element, with the application's class name on it if there is one.
//
// The class is passed rather than looked up: hammer ships no class name and no
// default for one (`ENGINEERING_RULES.md` §1), and a component that invented a fallback
// would be shipping a design system one string at a time.
export function elementIn<K extends keyof HTMLElementTagNameMap>(
    doc: Document,
    tag: K,
    className?: string,
): HTMLElementTagNameMap[K] {
    const made = doc.createElement(tag);
    if (className !== undefined && className.length > 0) {
        made.className = className;
    }
    return made;
}

// Text somebody typed, placed so it cannot reorder the sentence around it.
//
// `dir="auto"` resolves from the first strong character in the value itself, so
// an Arabic name inside a Latin interface lays out as Arabic and does not drag
// the punctuation around it to the other side. Without it the result is not a
// cosmetic defect: it changes which words the sentence appears to contain
// (`ENGINEERING_RULES.md` §8).
//
// `textContent`, never a markup route. The one insertion site in this library
// takes a `SanitizedHtml` and lives in `dom/sanitized.ts`.
export function setUserText(target: HTMLElement, text: string): void {
    target.textContent = text;
    target.dir = "auto";
}

// The same value where an attribute cannot reach — an `aria-label`, a `title`,
// anything read as a flat string. There is no element to carry `dir` there, so
// the isolate characters travel in the value (`core/bidi.ts`).
export function isolatedAttribute(text: string): string {
    return detectDirection(text) === "neutral" ? text : isolate(text);
}

// An id for wiring a label, a description or an error to the control it belongs
// to. `aria-describedby` and `for` are references by id and there is no other
// way to spell them.
//
// A counter and not `crypto.getRandomValues`, and the distinction is exactly
// what `ENGINEERING_RULES.md` §5's rule is about: that rule governs an id, a key or a nonce
// whose value has to be UNPREDICTABLE, because guessing one is the attack. This
// one has to be unique and nothing else — it is a reference between two elements
// that is sitting in the markup, readable by anyone who can read the markup at
// all, and a random one would buy nothing and cost a call per field.
//
// Checked against the document as well as counted, because a counter alone
// cannot know what the application already put there.
let minted = 0;

export function uniqueId(doc: Document, stem: string): string {
    for (;;) {
        minted += 1;
        const id = `${stem}-${minted}`;
        if (doc.getElementById(id) === null) {
            return id;
        }
    }
}

// Read now, and again on every change, until the component closes.
//
// Every component in this layer opens with this: it renders from the value the
// store already holds rather than waiting for the next one, because a component
// that only drew on change is a component that is blank until something happens.
export function bind<T>(store: Readable<T>, render: (value: T) => void): Unsubscribe {
    render(store.get());
    return store.subscribe(render);
}

// Takes a node out of the tree, through its parent.
//
// `node.remove()` is the shorter spelling and this does not use it: happy-dom
// 15 resolves the parent of a `<form>` through its form-owner rather than
// through the tree, and throws `removeChild` at itself — so a form renderer that
// used it would leave its root behind in the suite and nowhere else. Going
// through `parentNode` is one call longer, correct in both, and says which tree
// the node is being taken out of, which `remove()` never does.
export function detach(node: ChildNode): void {
    const parent = node.parentNode;
    if (parent !== null) {
        parent.removeChild(node);
    }
}

// One cleanup list, run once.
//
// Idempotent because a caller that closes twice must not abort a controller some
// later mount created, and because a component closed from inside its own
// subscriber — a session gate tearing down on a logout it just heard about — is
// ordinary rather than exotic.
export class Closers {
    private readonly held: (() => void)[];
    private done: boolean;

    constructor() {
        this.held = [];
        this.done = false;
    }

    add(close: () => void): void {
        if (this.done) {
            // Already closed. Running it now rather than holding it is what stops
            // a subscription made during teardown from outliving the component.
            close();
            return;
        }
        this.held.push(close);
    }

    run(): void {
        if (this.done) {
            return;
        }
        this.done = true;
        // Backwards: the last thing built is the first thing released, so a
        // subscription taken out over a resource comes off before the resource
        // it reads does.
        for (let i = this.held.length - 1; i >= 0; i -= 1) {
            const close = this.held[i];
            if (close === undefined) {
                continue;
            }
            try {
                close();
            } catch {
                // Every task body catches (`docs/00-architecture.md` §3). One
                // listener that throws on the way out must not strand the
                // resource handle behind it, which would be a request that
                // outlives the screen that wanted it.
            }
        }
        this.held.length = 0;
    }
}
