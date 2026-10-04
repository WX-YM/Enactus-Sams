// Client-side password prehashing: the step between a form and the request a
// sign-in or a registration sends (`docs/01-seams.md` §21, anvil `docs/05` §12).
//
//   secret     → NFC → UTF-8                          (never trimmed, never folded)
//   identifier → the salt route → salt and cost
//   Argon2id(secret, salt, cost) on the pool          → credential, base64url
//   the body, with the secret REMOVED and the credential in its place
//
// One component for every screen that sends a password: a login, a
// registration, a password change. The field names, the salt route, the cost
// bounds and the worker are the application's; the order of operations, the
// normalisation and the byte ceiling are anvil's contract and are fixed here.
//
// The client is not a security boundary, and nothing here is one. What this
// buys is that the password never leaves the device — not in the request body,
// not through a TLS-terminating proxy, not in a server log — and that the server
// no longer pays for the hash. Every control is still on the server.

import type { HammerError, PrehashError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";

import { decodeSaltAnswer, kPrehashKeyBytes } from "./answer.js";
import type { SaltAnswer } from "./answer.js";
import { encodeBase64Url } from "../core/base64url.js";
import type { Argon2Pool } from "./pool.js";

// anvil refuses a password over this many UTF-8 bytes before it hashes one
// (`kMaxPasswordBytes`). The same number, so a person is told here rather than
// after seconds of hashing a credential the server will never have been able to
// match — which is the form that accepts what the server then refuses
// (`CLAUDE.md` §8).
export const kMaxSecretBytes = 1024;

export type PrehashFailure = HammerError | PrehashError;

// The cost a salt answer may name. No defaults: what a phone of this
// application's audience can afford, and what the server is configured to ask,
// are both the application's to know.
export type PrehashBounds = {
    readonly minMemoryKib: number;
    readonly maxMemoryKib: number;
    readonly minIterations: number;
    readonly maxIterations: number;
    readonly maxParallelism: number;
};

// Which keys of the form's body hold what. The application's names, because the
// routes are the application's.
export type PrehashFields = {
    readonly identifier: string;
    readonly secret: string;
    readonly credential: string;
};

// Which salt a credential is derived under (anvil `docs/05` §13).
//
//   sign_in  the account's STORED salt and parameters, which is what the
//            credential it enrolled was derived under;
//   enroll   the salt a NEW credential is enrolled under — a registration, a
//            reset, a password change — derived from the identifier alone.
//
// Getting this wrong fails closed and silently: a new password derived under
// the stored salt is a credential the server will never match again.
export type SaltPurpose = "sign_in" | "enroll";

export type PrehasherConfig = {
    readonly pool: Argon2Pool;

    // The salt route, as the application's generated client calls it, with the
    // purpose sent as the body's `purpose`. Whatever it answers is decoded here,
    // in one place, and never by the caller.
    readonly saltFor: (
        identifier: string,
        purpose: SaltPurpose,
        signal: AbortSignal,
    ) => Promise<Result<unknown, HammerError>>;

    readonly bounds: PrehashBounds;

    // Only `prepare` reads these. A caller that derives credentials with
    // `credential` — `hammer/accounts` — has no form body to rename.
    readonly fields?: PrehashFields;
};

type Pending = {
    readonly identifier: string;
    readonly purpose: SaltPurpose;
    readonly controller: AbortController;
    readonly answer: Promise<Result<unknown, HammerError>>;
};

function failure(cause: PrehashError["cause"]): PrehashError {
    return { kind: "prehash", cause };
}

export class Prehasher {
    private readonly config: PrehasherConfig;

    // At most one salt answer, for at most one identifier, and only until it is
    // used. It is the whole cache: bounded by construction, and keyed by the one
    // identity allowed to read it (`CLAUDE.md` §2.3).
    private pending: Pending | null;

    constructor(config: PrehasherConfig) {
        const { bounds } = config;
        const sane =
            Number.isInteger(bounds.minMemoryKib) &&
            Number.isInteger(bounds.maxMemoryKib) &&
            Number.isInteger(bounds.minIterations) &&
            Number.isInteger(bounds.maxIterations) &&
            Number.isInteger(bounds.maxParallelism) &&
            bounds.minMemoryKib >= 8 &&
            bounds.minMemoryKib <= bounds.maxMemoryKib &&
            bounds.minIterations >= 1 &&
            bounds.minIterations <= bounds.maxIterations &&
            bounds.maxParallelism >= 1;
        if (!sane) {
            throw new Error("prehash bounds must be positive integers with min <= max");
        }
        this.config = config;
        this.pending = null;
    }

    // Asks for the salt before it is needed — when the identifier field loses
    // focus, say. The round trip then happens while the person types their
    // password instead of after they press the button, which on a mobile network
    // is 60 to 200 ms taken off every sign-in. Harmless to call often: a new
    // identifier replaces and aborts the previous one.
    prefetch(identifier: string, purpose: SaltPurpose = "sign_in"): void {
        if (
            identifier === "" ||
            (this.pending?.identifier === identifier && this.pending.purpose === purpose)
        ) {
            return;
        }
        this.drop();
        const controller = new AbortController();
        this.pending = {
            identifier,
            purpose,
            controller,
            // Caught into a value by contract (`saltFor` returns a Result), and
            // a rejection anyway is a programmer error that surfaces when the
            // answer is awaited — never as an unhandled rejection here.
            answer: this.config.saltFor(identifier, purpose, controller.signal).catch(
                (): Result<unknown, HammerError> => fail({ kind: "transport", cause: "network" }),
            ),
        };
    }

    // Derives the credential for one identifier and secret, under the salt the
    // purpose names. `sign_in` unless a new credential is being enrolled.
    async credential(
        identifier: string,
        secret: string,
        signal: AbortSignal,
        purpose: SaltPurpose = "sign_in",
    ): Promise<Result<string, PrehashFailure>> {
        if (identifier === "" || secret === "") {
            return fail(failure("missing-field"));
        }
        // NFC and nothing else — the same normalisation anvil applies in plain
        // mode, or one passphrase typed on two keyboards derives two credentials
        // and the person cannot sign in (anvil `docs/03-i18n-utf8.md` §6).
        const password = new TextEncoder().encode(secret.normalize("NFC"));
        if (password.length > kMaxSecretBytes) {
            password.fill(0);
            return fail(failure("secret-too-long"));
        }

        const answered = await this.saltAnswer(identifier, purpose, signal);
        if (!answered.ok) {
            password.fill(0);
            return answered;
        }
        const salt = answered.value;
        if (!this.withinBounds(salt)) {
            password.fill(0);
            return fail(failure("out-of-bounds"));
        }
        if (signal.aborted) {
            password.fill(0);
            return fail(failure("aborted"));
        }

        // `password` is transferred to the worker and detached here.
        const hashed = await this.config.pool.hash(
            {
                password,
                salt: salt.salt,
                memoryKib: salt.memoryKib,
                iterations: salt.iterations,
                parallelism: salt.parallelism,
                hashBytes: kPrehashKeyBytes,
            },
            signal,
        );
        if (!hashed.ok) {
            return hashed;
        }
        const key = hashed.value;
        const encoded = encodeBase64Url(key);
        key.fill(0);
        return ok(encoded);
    }

    // The form's body, ready to send: the secret removed, the credential added,
    // everything else untouched. A new object — the form's own body is not
    // mutated, so a failure here leaves the person's input where it was.
    async prepare(
        body: Readonly<Record<string, unknown>>,
        signal: AbortSignal,
        purpose: SaltPurpose = "sign_in",
    ): Promise<Result<Readonly<Record<string, unknown>>, PrehashFailure>> {
        const { fields } = this.config;
        if (fields === undefined) {
            throw new Error("prepare needs the field names a Prehasher was configured with");
        }
        const identifier = body[fields.identifier];
        const secret = body[fields.secret];
        if (typeof identifier !== "string" || typeof secret !== "string") {
            return fail(failure("missing-field"));
        }

        const derived = await this.credential(identifier, secret, signal, purpose);
        if (!derived.ok) {
            return derived;
        }

        const prepared: Record<string, unknown> = {};
        for (const key of Object.keys(body)) {
            if (key !== fields.secret) {
                prepared[key] = body[key];
            }
        }
        prepared[fields.credential] = derived.value;
        return ok(prepared);
    }

    // Aborts a prefetch nobody will use. For the screen's close.
    close(): void {
        this.drop();
    }

    private drop(): void {
        this.pending?.controller.abort();
        this.pending = null;
    }

    private async saltAnswer(
        identifier: string,
        purpose: SaltPurpose,
        signal: AbortSignal,
    ): Promise<Result<SaltAnswer, PrehashFailure>> {
        let answered: Result<unknown, HammerError>;
        const pending = this.pending;
        if (
            pending !== null &&
            pending.identifier === identifier &&
            pending.purpose === purpose &&
            !pending.controller.signal.aborted
        ) {
            // Adopted, and taken out of the cache: an answer is used once. The
            // next sign-in asks again, because a salt answer can change — a
            // password change moves an account to new parameters. The screen's
            // signal now governs the request it adopted.
            this.pending = null;
            const onAbort = (): void => pending.controller.abort();
            signal.addEventListener("abort", onAbort, { once: true });
            try {
                answered = await pending.answer;
            } finally {
                signal.removeEventListener("abort", onAbort);
            }
        } else {
            this.drop();
            answered = await this.config.saltFor(identifier, purpose, signal);
        }

        if (signal.aborted) {
            return fail(failure("aborted"));
        }
        if (!answered.ok) {
            return answered;
        }
        return decodeSaltAnswer(answered.value);
    }

    private withinBounds(answer: SaltAnswer): boolean {
        const { bounds } = this.config;
        return (
            answer.memoryKib >= bounds.minMemoryKib &&
            answer.memoryKib <= bounds.maxMemoryKib &&
            answer.iterations >= bounds.minIterations &&
            answer.iterations <= bounds.maxIterations &&
            answer.parallelism <= bounds.maxParallelism &&
            answer.memoryKib >= 8 * answer.parallelism
        );
    }
}
