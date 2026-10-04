//
// The reference consumer's components, driven through the package specifiers a
// real application would use.
//
// Type-checking `tests/testapp/app/screen.ts` proves each seam CAN be satisfied.
// This proves they can all be satisfied at once, by one consumer, in one
// construction order — and it proves the specifiers resolve at run time, which
// type-checking alone would let stay a fiction: `paths` is the compiler's map
// and not the runtime's. That gap is exactly what caught phase 4 out.

import { describe, expect, it } from "../support/test.js";

import type { Consent, InboxState, PagerState } from "hammer/state";
import { SessionStore } from "hammer/state";
import { Store } from "hammer/state";

import type { Locale } from "../testapp/api/hammer.generated.js";
import type { Alert, ScreenParts } from "../testapp/app/screen.js";
import { screenComponents } from "../testapp/app/screen.js";
import { kServerHash, kBits,} from "../support/session.js";

function parts(): ScreenParts {
    return {
        inbox: new Store<InboxState<Alert>>({
            unread: 2,
            items: [
                { id: "a", type: "content.published", body: { headline: "One" }, read: false },
                { id: "b", type: "content.published", body: { headline: "Two" }, read: true },
            ],
            live: true,
        }),
        pager: new Store<PagerState<{ readonly id: string }, never>>({
            status: "ready",
            items: [{ id: "a" }],
            hasMore: true,
            error: null,
        }),
        consent: new Store<Consent>("unknown"),
        session: new SessionStore({ clientHash: kServerHash, permissionBits: kBits }),
        markRead: () => undefined,
        more: () => undefined,
        setConsent: () => undefined,
        upload: async () => undefined,
        signIn: async () => ({ ok: true, value: undefined }),
        signUp: async () => ({ ok: true, value: undefined }),
        sendAccount: async () => ({ ok: true, value: undefined }),
        prepare: async (body) => ({ ok: true, value: body }),
    };
}

describe("the reference consumer's screen", () => {
    it("mounts every component this library ships, at once", () => {
        const host = document.createElement("div");
        document.body.append(host);

        const mounted = screenComponents("en", parts()).map((component) => {
            const at = document.createElement("div");
            host.append(at);
            return { name: component.name, view: component.mount(at) };
        });

        expect(mounted.length).toBeGreaterThan(0);
        for (const one of mounted) {
            expect(one.view.element.ownerDocument).toBe(document);
        }

        for (const one of mounted) {
            one.view.close();
        }
        host.remove();
    });

    // The half `paths` cannot prove. Nothing imported a VALUE from `hammer/dom`
    // until this file did, so the specifier was a map entry the runtime had
    // never resolved.
    it("reaches hammer through the specifiers a real application uses", async () => {
        const dom = await import("hammer/dom");
        expect(typeof dom.renderBell).toBe("function");
        expect(typeof dom.sanitize).toBe("function");

        // Not re-exported, and that is the guarantee rather than an oversight: a
        // consumer who could call `brand` could forge every `SanitizedHtml` in
        // the library.
        expect("brand" in dom).toBe(false);
    });

    it("satisfies every seam in both declared locales", () => {
        for (const locale of ["en", "ar"] as const satisfies readonly Locale[]) {
            const host = document.createElement("div");
            document.body.append(host);
            const views = screenComponents(locale, parts()).map((component) => {
                const at = document.createElement("div");
                host.append(at);
                return component.mount(at);
            });
            for (const view of views) {
                view.close();
            }
            host.remove();
        }
    });

    // The section renderer is the only caller of the insertion site, and the
    // reference application's `home.about` is the section that actually carries
    // a rich-text field — so this is the seam end to end, from the descriptor's
    // own table to the DOM.
    it("puts a section's rich text through the one insertion site", () => {
        const host = document.createElement("div");
        document.body.append(host);

        const section = screenComponents("en", parts()).find((one) => one.name === "section");
        expect(section).toBeDefined();
        const view = section?.mount(host);

        expect(host.querySelector(".ts-value em")?.textContent).toBe("prose");
        view?.close();
        host.remove();
    });
});
