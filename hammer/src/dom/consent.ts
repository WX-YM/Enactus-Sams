// The consent gate: explicit, revocable, and asked before anything is queued.
//
// --- three states, and a boolean cannot hold them ---------------------------
//
// Consent is `unknown`, `granted` or `denied`. A boolean makes the first
// indistinguishable from the third, which is how a consent prompt ends up asking
// somebody who has already said no — every session, forever
// (`state/analytics.ts`).
//
// --- nothing is queued before the answer ------------------------------------
//
// The gate this component draws is not where that is enforced. The sink refuses
// an event requiring consent at the door and does not buffer it: a buffer that
// flushes when consent arrives is a buffer of pre-consent data, which is the
// thing consent was about (`docs/01-seams.md` §10). This is the control that
// produces the answer; the refusal is a layer down, where it cannot be forgotten
// by whoever draws the prompt.
//
// --- revoking is a control, not a link somewhere ----------------------------
//
// Withdrawing is offered wherever granting was, and the sink drops what it has
// already collected when the answer changes to anything but `granted`. A grant
// that can only be withdrawn by finding a settings page is a grant in one
// direction.

import type { ClassNames } from "../core/tables.js";

import type { Consent } from "../state/analytics.js";
import type { Readable } from "../state/store.js";

import { Closers, bind, detach, documentOf, elementIn, uniqueId } from "./mount.js";
import type { Mounted } from "./mount.js";

export type ConsentPart = "root" | "question" | "grant" | "deny" | "granted" | "revoke";

export type ConsentCopy = {
    // What is being asked. The words are the application's because what is being
    // collected, and why, is the application's.
    readonly question: string;

    readonly grant: string;
    readonly deny: string;

    // Shown once there is an answer, so somebody can see what they chose and
    // change it.
    readonly granted: string;
    readonly denied: string;
    readonly revoke: string;
    readonly reconsider: string;
};

export type ConsentOptions = {
    // `AnalyticsSink.consent`. A store rather than a getter, so this redraws when
    // the answer changes somewhere else — a revocation made from a settings
    // screen is the ordinary case.
    readonly consent: Readable<Consent>;

    readonly setConsent: (answer: Consent) => void;

    readonly classes: ClassNames<ConsentPart>;
    readonly copy: ConsentCopy;
};

export function renderConsent(mount: Element, options: ConsentOptions): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { classes, copy } = options;

    const root = elementIn(doc, "div", classes.root);
    // A region rather than a dialogue: a consent prompt that trapped focus would
    // hold somebody in it until they answered, and an answer given to get out of
    // a trap is not consent.
    root.role = "region";
    const questionId = uniqueId(doc, "hammer-consent");
    root.setAttribute("aria-labelledby", questionId);

    const question = elementIn(doc, "p", classes.question);
    question.id = questionId;

    const grant = elementIn(doc, "button", classes.grant);
    grant.type = "button";
    grant.textContent = copy.grant;

    const deny = elementIn(doc, "button", classes.deny);
    deny.type = "button";
    deny.textContent = copy.deny;

    const settled = elementIn(doc, "p", classes.granted);

    const revoke = elementIn(doc, "button", classes.revoke);
    revoke.type = "button";

    root.append(question, grant, deny, settled, revoke);

    const answer = (next: Consent) => (): void => options.setConsent(next);
    const onGrant = answer("granted");
    const onDeny = answer("denied");
    grant.addEventListener("click", onGrant);
    deny.addEventListener("click", onDeny);
    closers.add(() => {
        grant.removeEventListener("click", onGrant);
        deny.removeEventListener("click", onDeny);
    });

    // Whichever way the current answer points, the other way is one press away.
    const onChange = (): void => {
        options.setConsent(options.consent.get() === "granted" ? "denied" : "granted");
    };
    revoke.addEventListener("click", onChange);
    closers.add(() => revoke.removeEventListener("click", onChange));

    const draw = (state: Consent): void => {
        const asked = state !== "unknown";

        question.textContent = copy.question;
        question.hidden = asked;
        grant.hidden = asked;
        deny.hidden = asked;

        settled.hidden = !asked;
        revoke.hidden = !asked;
        settled.textContent = state === "granted" ? copy.granted : copy.denied;
        revoke.textContent = state === "granted" ? copy.revoke : copy.reconsider;
    };

    closers.add(bind(options.consent, draw));

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
