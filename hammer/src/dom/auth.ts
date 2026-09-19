// The session gate, the permission gate, and the login flow.
//
// This is the largest piece of logic in the library and it renders almost
// nothing. It drives a credential it cannot read: anvil's tokens are `__Host-`
// cookies with `HttpOnly`, and every property that makes them worth using is a
// property hammer gets by having no implementation
// (`docs/00-architecture.md` §5.1). What is here is the orchestration — which
// surface is on screen, what comes off when the session ends, and what a
// submitted secret does after it has been sent.
//
// --- three states, and the third is the one people get wrong ----------------
//
// A session is `unknown`, `anonymous` or `active`, and `unknown` is NOT
// signed-out. Rendering the signed-out surface before the first read has
// answered is a login form that flashes at every person who is already signed
// in, on every cold load. So the gate renders neither surface until there is an
// answer, and the application may supply something for the wait.
//
// --- what comes off matters more than what goes on --------------------------
//
// When the session ends, the gate CLOSES the surface it had mounted rather than
// hiding it. A signed-in shell left in the document after another tab signed out
// is one shared device away from being a disclosure, and a hidden subtree still
// holds its subscriptions, its resource handles and whatever it had fetched
// (`docs/00-architecture.md` §5).
//
// --- a gate hides an affordance and cannot refuse a call --------------------
//
// There is no error in this library meaning "denied here", and nothing in this
// module invents one. A local refusal turns a stale permission copy into a
// denial no server-side change can clear, so the rule is: hide it, and if it is
// invoked anyway, send it and let the server decide
// (`docs/00-architecture.md` §4.1). The gate governs what is on screen and
// nothing else.

import type { HammerError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import type { ClassNames } from "../core/tables.js";

import type { Form } from "../state/forms.js";
import type { SessionState, SessionStore } from "../state/session.js";

import { renderForm } from "./form.js";
import type { FormCopy, FormField, FormPart } from "./form.js";
import { Closers, bind, detach, documentOf, elementIn } from "./mount.js";
import type { Mounted } from "./mount.js";

export type AuthPart = "root" | "pending" | "error";

// What the application draws once the gate has decided. It hands back its own
// handle, because the gate is what will close it.
export type Branch = (mount: Element) => Mounted;

export type SessionGateOptions = {
    readonly session: SessionStore;
    readonly classes: ClassNames<AuthPart>;

    readonly whenActive: Branch;
    readonly whenAnonymous: Branch;

    // Optional, and the only one that is: an application may legitimately want
    // nothing at all on screen while the first read is outstanding. What it may
    // not have is the signed-out surface, which is why this is a third slot
    // rather than a default onto one of the other two.
    readonly whenUnknown?: Branch;
};

export function renderSessionGate(mount: Element, options: SessionGateOptions): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();

    const root = elementIn(doc, "div", options.classes.root);

    let showing: SessionState["status"] | null = null;
    let branch: Mounted | null = null;

    const swap = (state: SessionState): void => {
        if (showing === state.status) {
            return;
        }
        showing = state.status;

        // Closed, not hidden. A hidden subtree still holds its subscriptions,
        // its refcounted resource handles and whatever it had already fetched.
        branch?.close();
        branch = null;

        const draw =
            state.status === "active"
                ? options.whenActive
                : state.status === "anonymous"
                  ? options.whenAnonymous
                  : options.whenUnknown;

        if (draw !== undefined) {
            branch = draw(root);
        }
    };

    closers.add(() => {
        branch?.close();
        branch = null;
    });
    closers.add(bind(options.session.store, swap));

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}

export type PermissionGateOptions = {
    readonly session: SessionStore;

    // Re-asked on every session change. The application writes it as
    // `() => session.affords(routeX)` for a route affordance, and as
    // `() => session.holds([kPermX])` only where the affordance is not a route:
    // the route table is the server's own answer and the bits can disagree with
    // it, because a superadmin's set is deliberately not all-ones
    // (`docs/01-seams.md` §4.1.1).
    readonly allows: () => boolean;

    readonly render: Branch;
};

// An affordance that is on screen only while the session affords it.
//
// Re-asked on every session change rather than once at mount, because a `403`
// refetches the session and the local copy was stale by definition — the whole
// reason the refetch exists is that what this gate answered a moment ago was
// wrong.
export function renderPermissionGate(mount: Element, options: PermissionGateOptions): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();

    const root = elementIn(doc, "div");
    let shown: boolean | null = null;
    let branch: Mounted | null = null;

    const decide = (): void => {
        const allowed = options.allows();
        if (shown === allowed) {
            return;
        }
        shown = allowed;

        branch?.close();
        branch = null;
        if (allowed) {
            branch = options.render(root);
        }
    };

    closers.add(() => {
        branch?.close();
        branch = null;
    });
    closers.add(bind(options.session.store, decide));

    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}

export type LoginOptions<Reason extends string> = {
    readonly form: Form<Reason>;
    readonly fields: readonly FormField[];

    // Two tables and not one intersection. `ClassNames<AuthPart> &
    // ClassNames<FormPart>` type-checks and is a trap: both unions carry `root`
    // and `error`, so one spread silently wins and the failure region ends up
    // wearing the same class as every field's error. Found by writing the test
    // that looked for it.
    readonly classes: ClassNames<AuthPart>;
    readonly formClasses: ClassNames<FormPart>;

    readonly copy: FormCopy;
    readonly reasons: Readonly<Record<Reason, string>>;

    // The words for a failure, keyed by the code the envelope decoded to. There
    // is no message on the wire to render: anvil's error responses carry a code
    // and never a sentence, for exactly the reason this parameter exists.
    readonly errors: Readonly<Record<string, string>>;

    // The call. It is the application's because `hammer/dom` may not import
    // `hammer/wire`, and because the route a sign-in is made to is the
    // application's route.
    readonly signIn: (
        body: Readonly<Record<string, unknown>>,
        signal: AbortSignal,
    ) => Promise<Result<unknown, HammerError<string, Reason>>>;
};

// The login form, and the credential discipline around it.
//
// The form itself is `renderForm`: a login form is a form, and rebuilding one
// here would be a second set of ARIA wiring to keep correct. What this adds is
// the part that is about a credential — a submit that cannot be sent twice, an
// abort when the screen goes away, and a secret that does not stay in memory
// after it has been sent.
export function renderLogin<Reason extends string>(
    mount: Element,
    options: LoginOptions<Reason>,
): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();
    const { form } = options;

    const root = elementIn(doc, "div", options.classes.root);

    // Aborted on close. A sign-in nothing can cancel is a request that outlives
    // the screen that wanted it, and this one is holding a secret while it runs.
    const lifetime = new AbortController();
    closers.add(() => lifetime.abort());

    const failure = elementIn(doc, "p", options.classes.error);
    // An alert: it answers a submit that has already happened, so announcing it
    // interrupts nothing the person was in the middle of.
    failure.role = "alert";

    const submit = async (): Promise<void> => {
        failure.textContent = "";
        form.submitting(true);

        const answer = await options.signIn(form.body(), lifetime.signal);

        if (lifetime.signal.aborted) {
            // The screen is gone. Publishing to a closed form would be a state
            // update after unmount, and there is nothing left to render it.
            return;
        }

        if (answer.ok) {
            // Clears every field's reason and re-empties every PII one, which is
            // where the secret was. hammer holds no credential and this is the
            // one place it could accidentally start: a password left in a store
            // after a successful sign-in is a credential in memory for as long
            // as the tab is open (`ENGINEERING_RULES.md` §5).
            form.accepted();
            return;
        }

        form.submitting(false);

        const error = answer.error;
        if (error.kind === "server" && error.fields !== null) {
            // Placed on the fields the server named. A reason for a field this
            // form does not have lands in the summary rather than being dropped.
            form.applyServerReasons(error.fields);
        }

        const code = error.kind === "server" ? error.code : error.kind;
        const words = Object.prototype.hasOwnProperty.call(options.errors, code)
            ? options.errors[code]
            : undefined;
        // Text, never markup, and never the submitted value echoed back: a
        // reflected value in an error is the encoding hazard anvil keeps out of
        // its own responses (`ENGINEERING_RULES.md` §5).
        failure.textContent = words ?? "";
    };

    const view = renderForm(root, {
        form,
        fields: options.fields,
        classes: options.formClasses,
        copy: options.copy,
        reasons: options.reasons,
        onSubmit: () => {
            // `renderForm` has already refused a submit while one is in flight
            // and refused one that does not validate. Every task body catches
            // (`docs/00-architecture.md` §3): an unhandled rejection here would
            // leave the form latched submitting with nothing to unlatch it.
            void submit().catch(() => {
                if (!lifetime.signal.aborted) {
                    form.submitting(false);
                }
            });
        },
    });
    closers.add(() => view.close());

    root.append(failure);
    mount.append(root);
    closers.add(() => detach(root));

    return { element: root, close: () => closers.run() };
}
