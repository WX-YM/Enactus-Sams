// The error surface: the application's words for a code, the request id, and
// nothing else.
//
// --- there is no message on the wire to render ------------------------------
//
// anvil answers every failure with a code and never a sentence, and this module
// is the reason that is the right design: the words belong to whoever knows the
// audience and the locale. An English default here is a string that ships to an
// Arabic user, from a library, so it ships to every application at once
// (`CLAUDE.md` §1).
//
// --- a 404 is not a permission error ----------------------------------------
//
// anvil answers a denied request on a stealth route with a byte-identical 404 so
// that a probe cannot distinguish a missing object from a forbidden one. This
// surface renders the application's `NOT_FOUND` words for a 404 and has no
// branch that could say anything else — a client that rendered "you do not have
// permission" there would hand back the oracle the server spent a whole design
// removing (`docs/00-architecture.md` §6).
//
// The way that is guaranteed is structural rather than remembered: the only
// thing this module does with an error is look up one string by its code. There
// is no second table keyed by status and no place to put one.
//
// --- the request id is displayed and never interpreted ----------------------
//
// It is the only thing a person can report that a server-side log can be found
// by. It is also NULLABLE, and null is the common case rather than a defensive
// one: anvil's 404 body is a single constexpr string shared byte for byte by a
// stealth drop, an unmatched route and a genuinely missing object, and Nginx's
// `error_page` serves those same bytes with no request behind them to have an
// id. A surface that assumed a string renders the word `undefined` on the most
// common failure there is.
//
// --- nothing submitted is echoed back ---------------------------------------
//
// No value from the request reaches this surface. Reflecting one is the
// reflected-XSS and log-injection hazard anvil keeps out of its own responses,
// and it is an encoding hazard on non-Latin input besides (`CLAUDE.md` §5).

import type { HammerError } from "../core/errors.js";
import type { ClassNames } from "../core/tables.js";

import { Closers, detach, documentOf, elementIn } from "./mount.js";
import type { Mounted } from "./mount.js";

export type ErrorPart = "root" | "message" | "requestId";

export type ErrorCopy<Code extends string> = {
    // Total over the error vocabulary, `Unknown` included — a code this bundle
    // predates still has to say something, and what it says is the application's
    // (`core/tables.ts`). This is `FailureCopy["errors"]` for the locale being
    // read.
    readonly errors: Readonly<Record<Code, string>>;

    // What hammer could not reach the server to find out. A transport failure
    // and a client-side refusal carry no code from anvil, because anvil never
    // saw them.
    readonly unreachable: string;

    // Names the id for somebody who is about to read it out over a telephone.
    readonly requestId: string;
};

export type ErrorOptions<Code extends string> = {
    // Null draws nothing. A surface that is present and empty is a surface that
    // takes up room on every screen that has no error on it.
    readonly error: HammerError<Code, string> | null;

    readonly classes: ClassNames<ErrorPart>;
    readonly copy: ErrorCopy<Code>;
};

export function renderError<Code extends string>(
    mount: Element,
    options: ErrorOptions<Code>,
): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { classes, copy, error } = options;

    const root = elementIn(doc, "div", classes.root);
    // It answers something that has already happened, so announcing it
    // interrupts nothing the person was in the middle of.
    root.role = "alert";

    const message = elementIn(doc, "p", classes.message);
    const requestId = elementIn(doc, "p", classes.requestId);
    root.append(message, requestId);

    if (error === null) {
        root.hidden = true;
        mount.append(root);
        closers.add(() => detach(root));
        return { element: root, close: () => closers.run() };
    }

    // One lookup, by code. There is no branch on the status and nowhere to add
    // one, which is what makes the 404 rule structural.
    message.textContent =
        error.kind === "server" ? copy.errors[error.code] : copy.unreachable;

    const id = error.kind === "server" ? error.requestId : null;
    if (id === null) {
        // The common case. Drawing the label with nothing after it would be a
        // heading for a value that is not there.
        requestId.hidden = true;
    } else {
        // Verbatim, as text. It is displayed and never interpreted, and it was
        // already validated on the way in — a control character in it would be
        // log injection in the one field that is meant to be safe to echo.
        requestId.textContent = `${copy.requestId} ${id}`;
    }

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
