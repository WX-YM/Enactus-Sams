// The account flows' client, with the real Argon2 worker body behind it.
//
// The route layer is a recording stand-in for the network: what matters here
// is what each flow SENDS — which fields, under which names — and, under client
// hashing, which salt each credential was derived under, for which identifier.
// That last is the property no application is trusted to get right, and the
// failure it prevents is silent: a new password derived under the stored salt
// is a credential the server never matches again.

import { describe, expect, it } from "../support/test.js";

import type { HammerError } from "../../src/core/errors.js";
import type { Result } from "../../src/core/result.js";
import { fail, ok } from "../../src/core/result.js";
import type { CallableRoute } from "../../src/wire/client.js";
import { Argon2Pool } from "../../src/prehash/pool.js";
import { serveArgon2Pool } from "../../src/prehash/worker.js";
import type { AccountsSpec } from "../../src/accounts/accounts.js";
import { Accounts } from "../../src/accounts/accounts.js";
import { accountFields } from "../../src/accounts/fields.js";

import { inProcessWorkers } from "../support/in_process_worker.js";

function route(id: string): CallableRoute {
    return { id, visibility: "public", method: "POST", path: `/${id}`, capability: null, rateLimit: null, idempotent: false };
}

const kRoutes = {
    salt: route("auth.prehash"),
    register: route("auth.signup"),
    verify: route("auth.verify"),
    resend: route("auth.resend"),
    signIn: route("auth.login"),
    resetRequest: route("auth.reset"),
    resetConfirm: route("auth.reset_confirm"),
    change: route("auth.password"),
    signOut: route("auth.logout"),
} as const;

function table(hashing: "client" | "server"): AccountsSpec {
    return {
        hashing,
        activation: "verify",
        contact: "email",
        identifiers: [
            { kind: "email", required: true, signIn: true },
            { kind: "username", required: true, signIn: true },
            { kind: "phone", required: false, signIn: true },
        ],
        profile: [
            { key: "given_name", required: true, minCodePoints: 1, maxCodePoints: 80, text: "prose", lineBreaks: false },
        ],
        secret: { minCodePoints: 12, maxCodePoints: 128, maxBytes: 1024 },
        codeDigits: 6,
        routes: hashing === "client" ? kRoutes : withoutSalt(),
    };
}

function withoutSalt(): AccountsSpec["routes"] {
    const { salt: _salt, ...rest } = kRoutes;
    return rest;
}

// The contract's salt answer (anvil docs/05 §12): salt "anvil-prehash-v1",
// at the cheapest cost, so the suite measures the flows rather than Argon2.
const kSaltAnswer = {
    algorithm: "argon2id",
    version: 19,
    salt: "YW52aWwtcHJlaGFzaC12MQ",
    memory_kib: 64,
    iterations: 3,
    parallelism: 1,
    hash_bytes: 32,
};

type Sent = { readonly route: string; readonly body: Readonly<Record<string, unknown>> };

function setup(hashing: "client" | "server", answer: Result<unknown, HammerError> = ok({})) {
    const sent: Sent[] = [];
    const pool = new Argon2Pool({ create: inProcessWorkers(serveArgon2Pool).create });
    const accounts = new Accounts({
        table: table(hashing),
        call: async (called, body) => {
            sent.push({ route: called.id, body });
            return called.id === "auth.prehash" ? ok(kSaltAnswer) : answer;
        },
        prehash: {
            pool,
            bounds: { minMemoryKib: 8, maxMemoryKib: 1024, minIterations: 1, maxIterations: 4, maxParallelism: 1 },
        },
    });
    return { sent, accounts, close: () => pool.close() };
}

function live(): AbortSignal {
    return new AbortController().signal;
}

const kPassword = "a long passphrase";

describe("Accounts under server hashing", () => {
    it("sends the password itself, under the field anvil reads", async () => {
        const { sent, accounts, close } = setup("server");
        expect((await accounts.signIn({ identifier: "a@b.test", password: kPassword }, live())).ok).toBe(true);
        expect(sent).toEqual([{ route: "auth.login", body: { identifier: "a@b.test", password: kPassword } }]);
        close();
    });

    it("sends a change as current_password and new_password", async () => {
        const { sent, accounts, close } = setup("server");
        await accounts.changePassword(
            { identifier: "a@b.test", currentPassword: kPassword, newPassword: "another passphrase" },
            live(),
        );
        expect(sent[0]?.body).toEqual({
            identifier: "a@b.test",
            current_password: kPassword,
            new_password: "another passphrase",
        });
        close();
    });

    it("refuses a secret outside the published bounds before sending anything", async () => {
        const { sent, accounts, close } = setup("server");
        expect(await accounts.signIn({ identifier: "a@b.test", password: "short" }, live())).toEqual({
            ok: false,
            error: { kind: "account", cause: "secret-too-short" },
        });
        // Twelve code points of an astral script is 24 UTF-16 units: counted as
        // code points, it fits.
        expect((await accounts.signIn({ identifier: "a@b.test", password: "𝒜".repeat(12) }, live())).ok).toBe(true);
        expect(await accounts.signIn({ identifier: "a@b.test", password: "x".repeat(129) }, live())).toEqual({
            ok: false,
            error: { kind: "account", cause: "secret-too-long" },
        });
        expect(sent).toHaveLength(1);
        close();
    });

    it("passes the server's refusal through unchanged", async () => {
        const refused: HammerError = { kind: "server", code: "UNAUTHENTICATED", status: 401, requestId: null, fields: null };
        const { accounts, close } = setup("server", fail(refused));
        expect(await accounts.signIn({ identifier: "a@b.test", password: kPassword }, live())).toEqual({
            ok: false,
            error: refused,
        });
        close();
    });
});

describe("Accounts under client hashing, the default", () => {
    it("sends a credential and never the password", async () => {
        const { sent, accounts, close } = setup("client");
        expect((await accounts.signIn({ identifier: "a@b.test", password: kPassword }, live())).ok).toBe(true);
        const login = sent.find((one) => one.route === "auth.login");
        expect(login?.body["identifier"]).toBe("a@b.test");
        expect(typeof login?.body["credential"]).toBe("string");
        expect(String(login?.body["credential"])).toHaveLength(43);
        expect(JSON.stringify(sent)).not.toContain(kPassword);
        close();
    });

    it("derives each credential under the salt its flow needs, for the identifier the server uses", async () => {
        const { sent, accounts, close } = setup("client");
        const salts = (): unknown[] => sent.filter((one) => one.route === "auth.prehash").map((one) => one.body);

        await accounts.signIn({ identifier: "user", password: kPassword }, live());
        expect(salts()).toEqual([{ identifier: "user", purpose: "sign_in" }]);

        sent.length = 0;
        // A registration enrols under the CONTACT, whatever else it carries.
        await accounts.register(
            { email: "new@b.test", username: "newcomer", profile: { given_name: "ليلى" }, password: kPassword },
            live(),
        );
        expect(salts()).toEqual([{ identifier: "new@b.test", purpose: "enroll" }]);

        sent.length = 0;
        await accounts.confirmReset({ identifier: "user", code: "123456", password: kPassword }, live());
        expect(salts()).toEqual([{ identifier: "user", purpose: "enroll" }]);

        sent.length = 0;
        // The current password under the STORED salt, the new one enrolled.
        await accounts.changePassword(
            { identifier: "user", currentPassword: kPassword, newPassword: "another passphrase" },
            live(),
        );
        expect(salts()).toEqual([
            { identifier: "user", purpose: "sign_in" },
            { identifier: "user", purpose: "enroll" },
        ]);
        const change = sent.find((one) => one.route === "auth.password");
        expect(Object.keys(change?.body ?? {}).sort()).toEqual(["current_credential", "identifier", "new_credential"]);
        close();
    });

    it("refuses to construct without an Argon2 pool, which it would need for every flow", () => {
        expect(
            () =>
                new Accounts({
                    table: table("client"),
                    call: async () => ok({}),
                    prehash: null,
                }),
        ).toThrow();
    });
});

describe("a form body, sent as its flow", () => {
    it("reads the keys accountFields gave each screen", async () => {
        const { sent, accounts, close } = setup("server");
        await accounts.submit(
            "register",
            { email: "n@b.test", username: "newcomer", phone: "", given_name: "Layla", password: kPassword },
            live(),
        );
        expect(sent[0]).toEqual({
            route: "auth.signup",
            body: { password: kPassword, email: "n@b.test", username: "newcomer", profile: { given_name: "Layla" } },
        });

        await accounts.submit("verify", { identifier: "n@b.test", code: "123456" }, live());
        expect(sent[1]).toEqual({ route: "auth.verify", body: { identifier: "n@b.test", code: "123456" } });
        close();
    });

    it("throws for a flow the application declared no route for", async () => {
        const sent: Sent[] = [];
        const accounts = new Accounts({
            table: { ...table("server"), routes: { signIn: route("auth.login") } },
            call: async (called, body) => {
                sent.push({ route: called.id, body });
                return ok({});
            },
            prehash: null,
        });
        let threw = false;
        try {
            await accounts.resend({ identifier: "a@b.test" }, live());
        } catch {
            threw = true;
        }
        expect(threw).toBe(true);
        expect(sent).toEqual([]);
    });
});

describe("accountFields", () => {
    it("gives a registration every declared identifier and profile field, bounded as the server bounds them", () => {
        const fields = accountFields(table("client"), "register");
        expect(fields.map((field) => field.key)).toEqual(["email", "username", "phone", "given_name", "password"]);
        expect(fields.find((field) => field.key === "phone")?.required).toBe(false);
        expect(fields.find((field) => field.key === "given_name")?.maxCodePoints).toBe(80);
        expect(fields.find((field) => field.key === "password")?.purpose).toBe("new-password");
        expect(fields.find((field) => field.key === "email")?.purpose).toBe("email");
    });

    it("gives a sign-in one identifier box and the current password", () => {
        const fields = accountFields(table("client"), "sign_in");
        expect(fields.map((field) => [field.key, field.purpose])).toEqual([
            ["identifier", "username"],
            ["password", "current-password"],
        ]);
    });

    it("draws secrets and codes as values that are never read back", () => {
        for (const flow of ["sign_in", "register", "reset_confirm", "change", "verify"] as const) {
            for (const field of accountFields(table("client"), flow)) {
                if (field.key.includes("password") || field.key === "code") {
                    expect(field.type.answer).toBeNull();
                }
            }
        }
        expect(accountFields(table("client"), "verify").find((field) => field.key === "code")?.purpose).toBe(
            "one-time-code",
        );
    });

    it("gives a change the identifier, the current password and the new one", () => {
        expect(accountFields(table("client"), "change").map((field) => field.key)).toEqual([
            "identifier",
            "current_password",
            "new_password",
        ]);
    });
});
