// The client of anvil's built-in account flows (anvil `docs/05` §13):
// registration, contact verification, sign-in, password reset and change, and
// sign-out, driven from the `kAccounts` table the descriptor publishes.
//
// An application writes none of the flow. It hands over its generated table,
// its client, and — when the server has the client hash, the default — the
// Argon2 pool and the cost its devices can afford. What it keeps is the only
// part it should: the screens, their words and their look (`CLAUDE.md` §9).
//
// --- what is fixed here and nowhere else ------------------------------------
//
// Which salt each credential is derived under. A sign-in and the CURRENT
// password of a change derive under the account's stored salt; a registration,
// a reset and the NEW password of a change derive under the enrolment salt, for
// exactly the identifier the server will derive it from (the contact for a
// registration, the named identifier otherwise). Get one wrong and the new
// password is a credential the server never matches, silently — which is why
// no application is asked to get it right.
//
// --- the client is not a security boundary ----------------------------------
//
// The secret bounds checked here are the server's own, published so a person
// is told at once. Under client hashing they are the ONLY place those bounds
// are applied, because the server receives 32 opaque bytes; a modified client
// that skips them weakens only its own account. Nothing here refuses anything
// the server would not.

import type { AccountError, HammerError, PrehashError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import { codePointLength, toNfc } from "../core/text.js";

import type { ApiTypes, CallableRoute, CallOptions, Client } from "../wire/client.js";

import type { Argon2Pool } from "../prehash/pool.js";
import type { PrehashBounds, SaltPurpose } from "../prehash/prehasher.js";
import { Prehasher } from "../prehash/prehasher.js";

export type IdentifierKind = "email" | "username" | "phone";

// The part of a generated `kAccounts` this reads. Structural, so no
// application's table reaches this layer (`CLAUDE.md` §1).
export type AccountsSpec = {
    readonly hashing: "client" | "server";
    readonly activation: "verify" | "immediate";
    readonly contact: "email" | "phone";
    readonly identifiers: readonly {
        readonly kind: IdentifierKind;
        readonly required: boolean;
        readonly signIn: boolean;
    }[];
    readonly profile: readonly {
        readonly key: string;
        readonly required: boolean;
        readonly minCodePoints: number;
        readonly maxCodePoints: number;
        readonly text: "prose" | "identifier";
        readonly lineBreaks: boolean;
    }[];
    readonly secret: {
        readonly minCodePoints: number;
        readonly maxCodePoints: number;
        readonly maxBytes: number;
    };
    readonly codeDigits: number;
    readonly routes: {
        readonly salt?: CallableRoute;
        readonly register?: CallableRoute;
        readonly verify?: CallableRoute;
        readonly resend?: CallableRoute;
        readonly signIn: CallableRoute;
        readonly resetRequest?: CallableRoute;
        readonly resetConfirm?: CallableRoute;
        readonly change?: CallableRoute;
        readonly refresh?: CallableRoute;
        readonly signOut?: CallableRoute;
    };
};

export type AccountRole = Exclude<keyof AccountsSpec["routes"], "salt" | "refresh">;

// A screen: the fields `accountFields` gives it, and what `submit` does with
// what was typed into them.
export type AccountFlow = "sign_in" | "register" | "verify" | "reset_request" | "reset_confirm" | "change";

export type AccountFailure = HammerError | PrehashError | AccountError;

// One request to one account route. What `accountCall` builds from a client,
// and what a test supplies directly.
export type AccountCall = (
    route: CallableRoute,
    body: Readonly<Record<string, unknown>>,
    signal: AbortSignal,
) => Promise<Result<unknown, HammerError>>;

// A client's `call`, for the account routes.
//
// The one place those routes are called without their generated types: the
// table hands them over as `CallableRoute`, and no account route declares a
// path parameter or a capability (anvil registers them with neither), so the
// options a call needs are exactly a body and a signal. Asserted once, here,
// rather than at every call site in every application.
export function accountCall<A extends ApiTypes>(client: Client<A>): AccountCall {
    return async (route, body, signal) =>
        await client.call(route, { body, signal } as unknown as CallOptions<A, typeof route>);
}

export type AccountsConfig = {
    readonly table: AccountsSpec;
    readonly call: AccountCall;

    // Required when the table says the client hashes, which is the default,
    // and ignored when it does not. The bounds are what this application's
    // devices can afford; hammer has no default for them.
    readonly prehash: {
        readonly pool: Argon2Pool;
        readonly bounds: PrehashBounds;
    } | null;
};

export type RegisterInput = {
    readonly email?: string;
    readonly username?: string;
    readonly phone?: string;
    // Keyed by the table's profile keys.
    readonly profile?: Readonly<Record<string, string>>;
    // A tag from the descriptor's locale table, when the application knows it.
    readonly locale?: string;
    readonly password: string;
};

const kTooShort: AccountError = { kind: "account", cause: "secret-too-short" };
const kTooLong: AccountError = { kind: "account", cause: "secret-too-long" };

export class Accounts {
    private readonly table: AccountsSpec;
    private readonly call: AccountCall;
    private readonly prehasher: Prehasher | null;

    constructor(config: AccountsConfig) {
        this.table = config.table;
        this.call = config.call;
        if (config.table.hashing === "client") {
            const salt = config.table.routes.salt;
            if (config.prehash === null || salt === undefined) {
                // The server expects credentials; a client that sent passwords
                // would be refused on every flow, so this is a misconfiguration
                // to report at construction rather than at the first sign-in.
                throw new Error("this server hashes on the client: an Argon2 pool and bounds are required");
            }
            const call = config.call;
            this.prehasher = new Prehasher({
                pool: config.prehash.pool,
                bounds: config.prehash.bounds,
                saltFor: async (identifier, purpose, signal) =>
                    await call(salt, { identifier, purpose }, signal),
            });
        } else {
            this.prehasher = null;
        }
    }

    // Asks for the sign-in salt ahead of the submit — call it when the
    // identifier field loses focus. A no-op under server hashing.
    prefetch(identifier: string): void {
        this.prehasher?.prefetch(identifier);
    }

    async signIn(
        input: { readonly identifier: string; readonly password: string },
        signal: AbortSignal,
    ): Promise<Result<void, AccountFailure>> {
        const secret = await this.secret("", input.identifier, input.password, "sign_in", signal);
        if (!secret.ok) {
            return secret;
        }
        return await this.send("signIn", { identifier: input.identifier, ...secret.value }, signal);
    }

    async register(input: RegisterInput, signal: AbortSignal): Promise<Result<void, AccountFailure>> {
        // Enrolled under the CONTACT, which is the identifier the server
        // derives a registration's salt from.
        const contact = this.table.contact === "email" ? input.email : input.phone;
        const secret = await this.secret("", contact ?? "", input.password, "enroll", signal);
        if (!secret.ok) {
            return secret;
        }
        const body: Record<string, unknown> = { ...secret.value };
        if (input.email !== undefined && input.email !== "") {
            body["email"] = input.email;
        }
        if (input.username !== undefined && input.username !== "") {
            body["username"] = input.username;
        }
        if (input.phone !== undefined && input.phone !== "") {
            body["phone"] = input.phone;
        }
        if (input.profile !== undefined) {
            body["profile"] = { ...input.profile };
        }
        if (input.locale !== undefined) {
            body["locale"] = input.locale;
        }
        return await this.send("register", body, signal);
    }

    async verify(
        input: { readonly identifier: string; readonly code: string },
        signal: AbortSignal,
    ): Promise<Result<void, AccountFailure>> {
        return await this.send("verify", { identifier: input.identifier, code: input.code }, signal);
    }

    async resend(input: { readonly identifier: string }, signal: AbortSignal): Promise<Result<void, AccountFailure>> {
        return await this.send("resend", { identifier: input.identifier }, signal);
    }

    async requestReset(
        input: { readonly identifier: string },
        signal: AbortSignal,
    ): Promise<Result<void, AccountFailure>> {
        return await this.send("resetRequest", { identifier: input.identifier }, signal);
    }

    async confirmReset(
        input: { readonly identifier: string; readonly code: string; readonly password: string },
        signal: AbortSignal,
    ): Promise<Result<void, AccountFailure>> {
        const secret = await this.secret("", input.identifier, input.password, "enroll", signal);
        if (!secret.ok) {
            return secret;
        }
        return await this.send(
            "resetConfirm",
            { identifier: input.identifier, code: input.code, ...secret.value },
            signal,
        );
    }

    // `identifier` is any one this account signs in with: the current
    // credential is derived under the account's stored salt, found by it, and
    // the new one under the enrolment salt derived from it.
    async changePassword(
        input: { readonly identifier: string; readonly currentPassword: string; readonly newPassword: string },
        signal: AbortSignal,
    ): Promise<Result<void, AccountFailure>> {
        const current = await this.secret("current_", input.identifier, input.currentPassword, "sign_in", signal);
        if (!current.ok) {
            return current;
        }
        const replacement = await this.secret("new_", input.identifier, input.newPassword, "enroll", signal);
        if (!replacement.ok) {
            return replacement;
        }
        return await this.send(
            "change",
            { identifier: input.identifier, ...current.value, ...replacement.value },
            signal,
        );
    }

    async signOut(signal: AbortSignal): Promise<Result<void, AccountFailure>> {
        return await this.send("signOut", {}, signal);
    }

    // A form body — keyed as `accountFields` keyed it — sent as the flow it
    // belongs to. The one-line seam between a form and this client: a screen's
    // submit is `(body, signal) => accounts.submit("sign_in", body, signal)`.
    async submit(
        flow: AccountFlow,
        body: Readonly<Record<string, unknown>>,
        signal: AbortSignal,
    ): Promise<Result<void, AccountFailure>> {
        const field = (key: string): string => {
            const value = body[key];
            return typeof value === "string" ? value : "";
        };
        switch (flow) {
            case "sign_in":
                return await this.signIn({ identifier: field("identifier"), password: field("password") }, signal);
            case "register": {
                const profile: Record<string, string> = {};
                for (const spec of this.table.profile) {
                    const value = field(spec.key);
                    if (value !== "") {
                        profile[spec.key] = value;
                    }
                }
                return await this.register(
                    {
                        email: field("email"),
                        username: field("username"),
                        phone: field("phone"),
                        profile,
                        password: field("password"),
                    },
                    signal,
                );
            }
            case "verify":
                return await this.verify({ identifier: field("identifier"), code: field("code") }, signal);
            case "reset_request":
                return await this.requestReset({ identifier: field("identifier") }, signal);
            case "reset_confirm":
                return await this.confirmReset(
                    { identifier: field("identifier"), code: field("code"), password: field("password") },
                    signal,
                );
            case "change":
                return await this.changePassword(
                    {
                        identifier: field("identifier"),
                        currentPassword: field("current_password"),
                        newPassword: field("new_password"),
                    },
                    signal,
                );
        }
    }

    // Aborts a prefetch nobody will use. For the screen's close.
    close(): void {
        this.prehasher?.close();
    }

    // The secret under the field name the server's hashing gives it: the
    // password itself, or the credential derived from it under `purpose`.
    private async secret(
        prefix: string,
        identifier: string,
        password: string,
        purpose: SaltPurpose,
        signal: AbortSignal,
    ): Promise<Result<Readonly<Record<string, string>>, AccountFailure>> {
        // Code points after NFC, which is what the server counts (`CLAUDE.md`
        // §8): a `.length` bound would halve the allowance for any script
        // written outside the Basic Multilingual Plane.
        const normalised = toNfc(password);
        const length = codePointLength(normalised);
        if (length < this.table.secret.minCodePoints) {
            return fail(kTooShort);
        }
        if (
            length > this.table.secret.maxCodePoints ||
            new TextEncoder().encode(normalised).length > this.table.secret.maxBytes
        ) {
            return fail(kTooLong);
        }

        if (this.prehasher === null) {
            return ok({ [`${prefix}password`]: password });
        }
        const credential = await this.prehasher.credential(identifier, password, signal, purpose);
        if (!credential.ok) {
            return credential;
        }
        return ok({ [`${prefix}credential`]: credential.value });
    }

    private async send(
        role: AccountRole,
        body: Readonly<Record<string, unknown>>,
        signal: AbortSignal,
    ): Promise<Result<void, AccountFailure>> {
        const route = this.table.routes[role];
        if (route === undefined) {
            // The application's table has no route for this flow, and a screen
            // offering it anyway is a programming error, not a network one.
            throw new Error(`this application declares no route for the account role "${role}"`);
        }
        const answered = await this.call(route, body, signal);
        return answered.ok ? ok(undefined) : answered;
    }
}
