// The same four things `dom/mount.ts` holds, for a layer that may not import it.
//
// `hammer/chart` is its own entry point so that an application with no dashboard
// pays nothing for one (`ENGINEERING_RULES.md` §2.1), and `tools/check-layering.sh` allows
// it `core` and `state` — not `dom`. If it reached across, every bundle with a
// chart in it would carry the form renderer, the bell and the insertion site.
//
// So these twenty lines are duplicated on purpose. The duplication IS the
// entry-point split doing its job, and the alternative — a shared module both
// import — is a third entry point whose only content is a handle type.

import { detectDirection, isolate } from "../core/bidi.js";

export type Mounted = {
    readonly element: HTMLElement;
    readonly close: () => void;
};

export function documentOf(mount: Element): Document {
    const owner = mount.ownerDocument;
    if (owner === null) {
        throw new Error("the mount point belongs to no document");
    }
    return owner;
}

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

const kSvgNamespace = "http://www.w3.org/2000/svg";

// An SVG element, which is not an HTML one and cannot be made by
// `createElement`: a `<rect>` in the HTML namespace is an unknown element that
// renders nothing, and it is the mistake that produces a chart-shaped blank.
//
// The class goes on by attribute. `className` on an SVG element is an
// `SVGAnimatedString` and assigning a string to it silently does nothing.
export function svgIn(doc: Document, tag: string, className?: string): SVGElement {
    const made = doc.createElementNS(kSvgNamespace, tag) as SVGElement;
    if (className !== undefined && className.length > 0) {
        made.setAttribute("class", className);
    }
    return made;
}

export function setUserText(target: HTMLElement, text: string): void {
    target.textContent = text;
    target.dir = "auto";
}

export function isolatedAttribute(text: string): string {
    return detectDirection(text) === "neutral" ? text : isolate(text);
}

export function detach(node: ChildNode): void {
    const parent = node.parentNode;
    if (parent !== null) {
        parent.removeChild(node);
    }
}

export class Closers {
    private readonly held: (() => void)[];
    private done: boolean;

    constructor() {
        this.held = [];
        this.done = false;
    }

    add(close: () => void): void {
        if (this.done) {
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
        for (let i = this.held.length - 1; i >= 0; i -= 1) {
            const close = this.held[i];
            if (close === undefined) {
                continue;
            }
            try {
                close();
            } catch {
                // Every task body catches (`docs/00-architecture.md` §3).
            }
        }
        this.held.length = 0;
    }
}
