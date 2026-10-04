//
// The gates, and the login flow's credential discipline.
//
// Nothing here asserts that a permission is enforced — the server does that, and
// a client-side test of a stand-in's refusal is worth nothing
// (`docs/16-test-plan.md`). What is asserted is what is ON SCREEN and what comes
// off it, which is the whole of what these components decide.

import { describe, expect, it } from "../support/test.js";

import type { ClassNames } from "../../src/core/tables.js";
import { fail, ok } from "../../src/core/result.js";
import { renderAccountForm, renderLogin, renderPermissionGate, renderSessionGate, renderSignup } from "../../src/dom/auth.js";
import type { AuthPart, Prepare } from "../../src/dom/auth.js";
import type { FormCopy, FormPart } from "../../src/dom/form.js";
import { Form } from "../../src/state/forms.js";
import type { FieldDefinition, FieldTypeSpec } from "../../src/state/forms.js";
import { SessionStore } from "../../src/state/session.js";
import { kServerHash, sessionPayload, kBits,} from "../support/session.js";

type Reason = "REQUIRED" | "TOO_LONG" | "BAD_FORMAT" | "NOT_ALLOWED";

const kAuthClasses: ClassNames<AuthPart> = { root: "a", pending: "a-pending", error: "a-error" };

const kFormClasses: ClassNames<FormPart> = {
    root: "f",
    field: "f-field",
    label: "f-label",
    required: "f-required",
    control: "f-control",
    description: "f-description",
    counter: "f-counter",
    error: "f-error",
    summary: "f-summary",
    choices: "f-choices",
    choice: "f-choice",
    submit: "f-submit",
};

const kCopy: FormCopy = {
    required: "required",
    remaining: (n) => `${n} left`,
    summary: "problems",
    submit: "sign in",
};

const kReasons: Readonly<Record<Reason, string>> = {
    REQUIRED: "Fill this in.",
    TOO_LONG: "That is too long.",
    BAD_FORMAT: "Check the format.",
    NOT_ALLOWED: "That value is not allowed.",
};

function spec(answer: FieldTypeSpec["answer"] = "text"): FieldTypeSpec {
    return {
        answer,
        defaultCodePoints: 64,
        flags: {
            options: false,
            attachment: false,
            ranged: false,
            codePointCapped: false,
            multiLine: false,
            multiSelect: false,
        },
    };
}

// A branch that records whether it is currently mounted, which is the property
// the gate is actually responsible for.
function branch(log: string[], name: string) {
    return (mount: Element) => {
        log.push(`open:${name}`);
        const element = document.createElement("div");
        element.dataset["branch"] = name;
        mount.append(element);
        return {
            element,
            close: () => {
                log.push(`close:${name}`);
                element.remove();
            },
        };
    };
}

function session() {
    return new SessionStore({ clientHash: kServerHash, permissionBits: kBits });
}

describe("the session gate", () => {
    // Rendering the signed-out surface before the first read has answered is a
    // login form that flashes at everybody who is already signed in, on every
    // cold load.
    it("renders neither surface before the first read answers", () => {
        const log: string[] = [];
        const host = document.createElement("div");
        const view = renderSessionGate(host, {
            session: session(),
            classes: kAuthClasses,
            whenActive: branch(log, "in"),
            whenAnonymous: branch(log, "out"),
        });
        expect(log).toEqual([]);
        expect(host.querySelector("[data-branch]")).toBeNull();
        view.close();
    });

    it("renders what the application gave it for the wait, where it gave one", () => {
        const log: string[] = [];
        const host = document.createElement("div");
        const view = renderSessionGate(host, {
            session: session(),
            classes: kAuthClasses,
            whenActive: branch(log, "in"),
            whenAnonymous: branch(log, "out"),
            whenUnknown: branch(log, "wait"),
        });
        expect(log).toEqual(["open:wait"]);
        view.close();
    });

    it("swaps to the signed-in surface when a session arrives", async () => {
        const log: string[] = [];
        const store = session();
        store.readsFrom(async () => ok(sessionPayload()));

        const host = document.createElement("div");
        const view = renderSessionGate(host, {
            session: store,
            classes: kAuthClasses,
            whenActive: branch(log, "in"),
            whenAnonymous: branch(log, "out"),
            whenUnknown: branch(log, "wait"),
        });

        await store.load(new AbortController().signal);
        expect(log).toEqual(["open:wait", "close:wait", "open:in"]);
        expect(host.querySelector('[data-branch="in"]')).not.toBeNull();
        view.close();
    });

    // A signed-in shell left in the document after another tab signed out is one
    // shared device away from being a disclosure, and a hidden subtree still
    // holds its subscriptions and its resource handles.
    it("closes the signed-in surface rather than hiding it", async () => {
        const log: string[] = [];
        const store = session();
        store.readsFrom(async () => ok(sessionPayload()));

        const host = document.createElement("div");
        const view = renderSessionGate(host, {
            session: store,
            classes: kAuthClasses,
            whenActive: branch(log, "in"),
            whenAnonymous: branch(log, "out"),
        });

        await store.load(new AbortController().signal);
        store.clear();

        expect(log).toEqual(["open:in", "close:in", "open:out"]);
        expect(host.querySelector('[data-branch="in"]')).toBeNull();
        view.close();
    });

    it("does not redraw when the session changes without changing status", async () => {
        const log: string[] = [];
        const store = session();
        store.readsFrom(async () => ok(sessionPayload()));

        const host = document.createElement("div");
        const view = renderSessionGate(host, {
            session: store,
            classes: kAuthClasses,
            whenActive: branch(log, "in"),
            whenAnonymous: branch(log, "out"),
        });

        await store.load(new AbortController().signal);
        await store.load(new AbortController().signal);
        expect(log).toEqual(["open:in"]);
        view.close();
    });

    it("closes the branch it is showing when it closes", async () => {
        const log: string[] = [];
        const store = session();
        store.readsFrom(async () => ok(sessionPayload()));
        const host = document.createElement("div");
        const view = renderSessionGate(host, {
            session: store,
            classes: kAuthClasses,
            whenActive: branch(log, "in"),
            whenAnonymous: branch(log, "out"),
        });
        await store.load(new AbortController().signal);
        view.close();
        expect(log).toEqual(["open:in", "close:in"]);
    });
});

describe("the permission gate", () => {
    it("draws nothing while the affordance is not held", () => {
        const log: string[] = [];
        const host = document.createElement("div");
        const view = renderPermissionGate(host, {
            session: session(),
            allows: () => false,
            render: branch(log, "admin"),
        });
        expect(log).toEqual([]);
        view.close();
    });

    // Re-asked on every session change rather than once at mount: a 403
    // refetches the session precisely because the local copy was stale, so what
    // this answered a moment ago was wrong by definition.
    it("re-asks when the session changes", async () => {
        const log: string[] = [];
        const store = session();
        store.readsFrom(async () => ok(sessionPayload()));

        let allowed = false;
        const host = document.createElement("div");
        const view = renderPermissionGate(host, {
            session: store,
            allows: () => allowed,
            render: branch(log, "admin"),
        });

        allowed = true;
        await store.load(new AbortController().signal);
        expect(log).toEqual(["open:admin"]);

        allowed = false;
        store.clear();
        expect(log).toEqual(["open:admin", "close:admin"]);
        view.close();
    });
});

describe("the login flow", () => {
    function login(
        signIn: (
            body: Readonly<Record<string, unknown>>,
            signal: AbortSignal,
        ) => Promise<ReturnType<typeof ok<unknown>> | ReturnType<typeof fail<never>>>,
    ) {
        const fields: readonly FieldDefinition[] = [
            { key: "email", type: spec(), required: true },
            // A PII type: its value never comes back, which is what makes it the
            // right shape for a secret.
            { key: "secret", type: spec(null), required: true },
        ];
        const form = new Form<Reason>({
            fields,
            reasons: {
                required: "REQUIRED",
                tooLong: "TOO_LONG",
                badFormat: "BAD_FORMAT",
                notAllowed: "NOT_ALLOWED",
            },
        });

        const host = document.createElement("div");
        document.body.append(host);
        const view = renderLogin(host, {
            form,
            fields: [
                { key: "email", label: "Email" },
                { key: "secret", label: "Password" },
            ],
            classes: kAuthClasses,
            formClasses: kFormClasses,
            copy: kCopy,
            reasons: kReasons,
            errors: { UNAUTHENTICATED: "Those details did not match.", transport: "No connection." },
            signIn: signIn as never,
        });

        return { form, host, view, close: () => { view.close(); host.remove(); } };
    }

    function fill(app: ReturnType<typeof login>) {
        app.form.set("email", "a@b.test");
        app.form.set("secret", "hunter2");
    }

    it("sends what the form holds", async () => {
        const sent: Record<string, unknown>[] = [];
        const app = login(async (body) => {
            sent.push({ ...body });
            return ok(undefined);
        });
        fill(app);
        app.host.querySelector("form")?.dispatchEvent(new Event("submit", { cancelable: true }));
        await Promise.resolve();
        expect(sent[0]).toEqual({ email: "a@b.test", secret: "hunter2" });
        app.close();
    });

    // hammer holds no credential, and this is the one place it could
    // accidentally start: a password left in a store after a successful sign-in
    // is a credential in memory for as long as the tab is open.
    it("does not keep the secret after it has been sent", async () => {
        const app = login(async () => ok(undefined));
        fill(app);
        app.host.querySelector("form")?.dispatchEvent(new Event("submit", { cancelable: true }));
        await Promise.resolve();
        await Promise.resolve();

        expect(app.form.field("secret")?.value).toBeNull();
        app.close();
    });

    it("places the words for the code the envelope carried", async () => {
        const app = login(async () =>
            fail({ kind: "server", code: "UNAUTHENTICATED", status: 401, requestId: null, fields: null } as never),
        );
        fill(app);
        app.host.querySelector("form")?.dispatchEvent(new Event("submit", { cancelable: true }));
        await Promise.resolve();
        await Promise.resolve();

        expect(app.host.querySelector(".a-error")?.textContent).toBe("Those details did not match.");
        app.close();
    });

    it("announces a failure, because it answers a submit that already happened", () => {
        const app = login(async () => ok(undefined));
        expect(app.host.querySelector(".a-error")?.getAttribute("role")).toBe("alert");
        app.close();
    });

    it("places a reason the server named on the field it named", async () => {
        const app = login(async () =>
            fail({
                kind: "server",
                code: "VALIDATION_FAILED",
                status: 400,
                requestId: null,
                fields: new Map([["email", "BAD_FORMAT"]]),
            } as never),
        );
        fill(app);
        app.host.querySelector("form")?.dispatchEvent(new Event("submit", { cancelable: true }));
        await Promise.resolve();
        await Promise.resolve();

        expect(app.form.field("email")?.reason).toBe("BAD_FORMAT");
        app.close();
    });

    it("unlatches so a failed attempt can be made again", async () => {
        const app = login(async () =>
            fail({ kind: "transport", cause: "network" } as never),
        );
        fill(app);
        app.host.querySelector("form")?.dispatchEvent(new Event("submit", { cancelable: true }));
        await Promise.resolve();
        await Promise.resolve();

        expect(app.form.store.get().submitting).toBe(false);
        expect(app.host.querySelector(".a-error")?.textContent).toBe("No connection.");
        app.close();
    });

    // A sign-in nothing can cancel is a request that outlives the screen that
    // wanted it, and this one is holding a secret while it runs.
    it("aborts the call when the screen goes away", async () => {
        let seen: AbortSignal | null = null;
        const app = login(async (_body, signal) => {
            seen = signal;
            return ok(undefined);
        });
        fill(app);
        app.host.querySelector("form")?.dispatchEvent(new Event("submit", { cancelable: true }));
        await Promise.resolve();

        app.view.close();
        expect(seen).not.toBeNull();
        expect((seen as unknown as AbortSignal).aborted).toBe(true);
        app.host.remove();
    });

    // Publishing to a closed form would be a state update after unmount, with
    // nothing left to render it.
    it("publishes nothing after the screen has gone", async () => {
        let release: () => void = () => undefined;
        const held = new Promise<void>((resolve) => {
            release = resolve;
        });
        const app = login(async () => {
            await held;
            return ok(undefined);
        });
        fill(app);
        app.host.querySelector("form")?.dispatchEvent(new Event("submit", { cancelable: true }));
        await Promise.resolve();

        app.view.close();
        release();
        await Promise.resolve();
        await Promise.resolve();

        // Still holding what it had: `accepted()` never ran, because the answer
        // arrived after the abort.
        expect(app.form.field("email")?.value).toBe("a@b.test");
        app.host.remove();
    });
});

// The step between a form and its call, where a client-prehash application
// swaps the password for a credential (`docs/01-seams.md` §21). Driven with a
// stand-in `prepare` here: what matters in this layer is what is SENT, what is
// kept, and what is said — the derivation itself is `hammer/prehash`'s suite.
describe("the prepare step, on the login and the signup form", () => {
    type Send = (
        body: Readonly<Record<string, unknown>>,
        signal: AbortSignal,
    ) => Promise<ReturnType<typeof ok<unknown>> | ReturnType<typeof fail<never>>>;

    function screen(which: "login" | "signup", send: Send, prepare?: Prepare<Reason>) {
        const form = new Form<Reason>({
            fields: [
                { key: "email", type: spec(), required: true },
                { key: "secret", type: spec(null), required: true },
            ],
            reasons: {
                required: "REQUIRED",
                tooLong: "TOO_LONG",
                badFormat: "BAD_FORMAT",
                notAllowed: "NOT_ALLOWED",
            },
        });
        const host = document.createElement("div");
        document.body.append(host);
        const shared = {
            form,
            fields: [
                { key: "email", label: "Email" },
                { key: "secret", label: "Password" },
            ],
            classes: kAuthClasses,
            formClasses: kFormClasses,
            copy: kCopy,
            reasons: kReasons,
            errors: { prehash: "Could not prepare your password.", UNAUTHENTICATED: "No match." },
            ...(prepare === undefined ? {} : { prepare }),
        };
        const view =
            which === "login"
                ? renderLogin(host, { ...shared, signIn: send as never })
                : renderSignup(host, { ...shared, signUp: send as never });
        form.set("email", "a@b.test");
        form.set("secret", "hunter2");
        return {
            form,
            host,
            view,
            submit: () => host.querySelector("form")?.dispatchEvent(new Event("submit", { cancelable: true })),
            close: () => {
                view.close();
                host.remove();
            },
        };
    }

    async function turns(n = 6): Promise<void> {
        for (let i = 0; i < n; i += 1) {
            await Promise.resolve();
        }
    }

    // Replaces the secret with a credential, as `Prehasher.prepare` does.
    const swap: Prepare<Reason> = async (body) => {
        const { secret: _secret, ...rest } = body;
        return ok({ ...rest, credential: "derived" });
    };

    for (const which of ["login", "signup"] as const) {
        it(`${which}: sends the prepared body, never the secret`, async () => {
            const sent: Record<string, unknown>[] = [];
            const app = screen(
                which,
                async (body) => {
                    sent.push({ ...body });
                    return ok(undefined);
                },
                swap,
            );
            app.submit();
            await turns();
            expect(sent).toEqual([{ email: "a@b.test", credential: "derived" }]);
            // And the form no longer holds it either, once the call succeeded.
            expect(app.form.field("secret")?.value).toBeNull();
            app.close();
        });

        it(`${which}: a failed prepare sends nothing, says so, and keeps the input to retry`, async () => {
            let calls = 0;
            const app = screen(
                which,
                async () => {
                    calls += 1;
                    return ok(undefined);
                },
                async () => fail({ kind: "prehash", cause: "out-of-memory" }),
            );
            app.submit();
            await turns();
            expect(calls).toBe(0);
            expect(app.host.querySelector(".a-error")?.textContent).toBe("Could not prepare your password.");
            expect(app.form.store.get().submitting).toBe(false);
            expect(app.form.field("secret")?.value).toBe("hunter2");
            app.close();
        });
    }

    it("hands prepare the screen's signal, and sends nothing once the screen has gone", async () => {
        let seen: AbortSignal | null = null;
        let release: () => void = () => undefined;
        const held = new Promise<void>((resolve) => {
            release = resolve;
        });
        let calls = 0;
        const app = screen(
            "login",
            async () => {
                calls += 1;
                return ok(undefined);
            },
            async (body, signal) => {
                seen = signal;
                await held;
                return swap(body, signal);
            },
        );
        app.submit();
        await turns(2);
        app.view.close();
        release();
        await turns();

        expect((seen as unknown as AbortSignal).aborted).toBe(true);
        expect(calls).toBe(0);
        app.host.remove();
    });

    it("signup reports the server's words exactly as the login does", async () => {
        const app = screen("signup", async () =>
            fail({ kind: "server", code: "UNAUTHENTICATED", status: 401, requestId: null, fields: null } as never),
        );
        app.submit();
        await turns();
        expect(app.host.querySelector(".a-error")?.textContent).toBe("No match.");
        expect(app.host.querySelector(".a-error")?.getAttribute("role")).toBe("alert");
        app.close();
    });
});

describe("the account form, and a refusal made before anything was sent", () => {
    function screen(send: Parameters<typeof renderAccountForm<Reason>>[1]["send"]) {
        const form = new Form<Reason>({
            fields: [
                { key: "identifier", type: spec(), required: true },
                { key: "code", type: spec(null), required: true },
            ],
            reasons: { required: "REQUIRED", tooLong: "TOO_LONG", badFormat: "BAD_FORMAT", notAllowed: "NOT_ALLOWED" },
        });
        const host = document.createElement("div");
        document.body.append(host);
        const view = renderAccountForm(host, {
            form,
            fields: [
                { key: "identifier", label: "Email" },
                { key: "code", label: "Code" },
            ],
            classes: kAuthClasses,
            formClasses: kFormClasses,
            copy: kCopy,
            reasons: kReasons,
            errors: {
                account: "That did not work.",
                "account.secret-too-short": "Use at least twelve characters.",
            },
            send,
        });
        form.set("identifier", "a@b.test");
        form.set("code", "123456");
        return {
            form,
            host,
            submit: () => host.querySelector("form")?.dispatchEvent(new Event("submit", { cancelable: true })),
            close: () => {
                view.close();
                host.remove();
            },
        };
    }

    async function turns(n = 6): Promise<void> {
        for (let i = 0; i < n; i += 1) {
            await Promise.resolve();
        }
    }

    it("sends what the form holds and drops the code once it has been used", async () => {
        const sent: Record<string, unknown>[] = [];
        const app = screen(async (body) => {
            sent.push({ ...body });
            return ok(undefined);
        });
        app.submit();
        await turns();
        expect(sent).toEqual([{ identifier: "a@b.test", code: "123456" }]);
        expect(app.form.field("code")?.value).toBeNull();
        app.close();
    });

    it("words a refusal by its cause before its kind", async () => {
        const app = screen(async () => fail({ kind: "account", cause: "secret-too-short" }));
        app.submit();
        await turns();
        expect(app.host.querySelector(".a-error")?.textContent).toBe("Use at least twelve characters.");
        app.close();
    });

    it("falls back to the kind when the cause has no words of its own", async () => {
        const app = screen(async () => fail({ kind: "account", cause: "secret-too-long" }));
        app.submit();
        await turns();
        expect(app.host.querySelector(".a-error")?.textContent).toBe("That did not work.");
        app.close();
    });
});

