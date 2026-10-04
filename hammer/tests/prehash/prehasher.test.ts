// Client-side prehashing, with the worker's own body running.
//
// The pool here is the real `Argon2Pool`, and behind it is the real
// `serveArgon2Pool` — not a stand-in that answers for it. A worker side no test
// executes is how `serveImagePool` shipped uncalled; `inProcessWorkers` carries
// each message through `structuredClone` with its transfer list, so a buffer the
// pool transfers is detached on the sending side exactly as a browser would
// detach it.
//
// The credential asserted in the first test is the contract's: libargon2
// computed it, and anvil's suite asserts the same string. A browser that derives
// it and a server that verifies it agree byte for byte, or a person cannot sign
// in.

import { describe, expect, it } from "../support/test.js";

import type { HammerError } from "../../src/core/errors.js";
import type { Result } from "../../src/core/result.js";
import { fail, ok } from "../../src/core/result.js";
import { Argon2Pool, kArgon2Workers } from "../../src/prehash/pool.js";
import type { PrehashBounds } from "../../src/prehash/prehasher.js";
import { Prehasher, kMaxSecretBytes } from "../../src/prehash/prehasher.js";
import { serveArgon2Pool } from "../../src/prehash/worker.js";

import type { InProcessWorkers } from "../support/in_process_worker.js";
import { inProcessWorkers } from "../support/in_process_worker.js";

// --- the salt route, as an application's generated client would call it -----------

type Answer = Readonly<Record<string, unknown>>;

// The contract's vector: salt ASCII "anvil-prehash-v1", m=64, t=3, p=1.
const kContractAnswer: Answer = {
    algorithm: "argon2id",
    version: 19,
    salt: "YW52aWwtcHJlaGFzaC12MQ",
    memory_kib: 64,
    iterations: 3,
    parallelism: 1,
    hash_bytes: 32,
};
const kContractCredential = "V1EeBHtz6lHNNUKmMShuhj4wfvEXJoEiJTv0fExs5uk";
// "pässwörd كلمة 🔑", decomposed, as a paste from some keyboards.
const kTypedPassword = new TextDecoder().decode(
    Buffer.from("7061cc887373776fcc88726420d983d984d985d8a920f09f9491", "hex"),
);

class SaltRoute {
    readonly asked: string[] = [];
    readonly purposes: string[] = [];
    readonly signals: AbortSignal[] = [];
    answer: Result<unknown, HammerError> = ok(kContractAnswer);
    // Holds the answer until released, for the tests that race it.
    private gate: Promise<void> = Promise.resolve();

    hold(): () => void {
        let release = (): void => undefined;
        this.gate = new Promise((resolve) => {
            release = resolve;
        });
        return release;
    }

    saltFor = async (
        identifier: string,
        purpose: string,
        signal: AbortSignal,
    ): Promise<Result<unknown, HammerError>> => {
        this.asked.push(identifier);
        this.purposes.push(purpose);
        this.signals.push(signal);
        await this.gate;
        if (signal.aborted) {
            return fail({ kind: "transport", cause: "aborted" });
        }
        return this.answer;
    };
}

const kBounds: PrehashBounds = {
    minMemoryKib: 8,
    maxMemoryKib: 262144,
    minIterations: 1,
    maxIterations: 10,
    maxParallelism: 4,
};

const kFields = { identifier: "email", secret: "password", credential: "credential" } as const;

function setup(bounds: PrehashBounds = kBounds): {
    readonly bridge: InProcessWorkers;
    readonly route: SaltRoute;
    readonly pool: Argon2Pool;
    readonly prehasher: Prehasher;
} {
    const bridge = inProcessWorkers(serveArgon2Pool);
    const route = new SaltRoute();
    const pool = new Argon2Pool({ create: bridge.create });
    const prehasher = new Prehasher({ pool, saltFor: route.saltFor, bounds, fields: kFields });
    return { bridge, route, pool, prehasher };
}

function live(): AbortSignal {
    return new AbortController().signal;
}

describe("Prehasher: the credential", () => {
    it("derives exactly the contract's credential from a decomposed password", async () => {
        const { prehasher, route, pool } = setup();
        const derived = await prehasher.credential("user@example.com", kTypedPassword, live());
        expect(derived).toEqual({ ok: true, value: kContractCredential });
        expect(route.asked).toEqual(["user@example.com"]);
        pool.close();
    });

    it("prepares a body with the password REMOVED and the credential in its place", async () => {
        const { prehasher, pool } = setup();
        const body = { email: "user@example.com", password: kTypedPassword, remember: true } as const;
        const prepared = await prehasher.prepare(body, live());

        expect(prepared).toEqual({
            ok: true,
            value: { email: "user@example.com", remember: true, credential: kContractCredential },
        });
        if (prepared.ok) {
            expect(Object.prototype.hasOwnProperty.call(prepared.value, "password")).toBe(false);
        }
        // The form's own body is untouched: a failure after this point must
        // leave the person's input where it was.
        expect(body.password).toBe(kTypedPassword);
        pool.close();
    });

    it("transfers the password to the worker, leaving no copy on the main thread", async () => {
        const { pool } = setup();
        const password = new TextEncoder().encode("hunter2 hunter2");
        const hashed = await pool.hash(
            { password, salt: new Uint8Array(16), memoryKib: 64, iterations: 1, parallelism: 1, hashBytes: 32 },
            live(),
        );
        expect(hashed.ok).toBe(true);
        expect(password.byteLength).toBe(0);
        pool.close();
    });
});

describe("Prehasher: which salt", () => {
    it("asks for the stored salt to sign in and the enrolment salt for a new credential", async () => {
        const { prehasher, route, pool } = setup();
        expect((await prehasher.credential("a@b.c", "pw", live())).ok).toBe(true);
        expect((await prehasher.credential("a@b.c", "pw", live(), "enroll")).ok).toBe(true);
        expect(route.purposes).toEqual(["sign_in", "enroll"]);
        pool.close();
    });

    it("never hands a prefetched sign-in salt to an enrolment", async () => {
        // A new password derived under the STORED salt is a credential the server
        // will never match again, and the failure would be silent.
        const { prehasher, route, pool } = setup();
        prehasher.prefetch("a@b.c");
        expect((await prehasher.credential("a@b.c", "pw", live(), "enroll")).ok).toBe(true);
        expect(route.purposes).toEqual(["sign_in", "enroll"]);
        expect(route.signals[0]?.aborted).toBe(true);
        pool.close();
    });
});

describe("Prehasher: refusals", () => {
    it("refuses a body without the configured fields, or with an empty one", async () => {
        const { prehasher, route, pool } = setup();
        const missing = { kind: "prehash", cause: "missing-field" };
        expect(await prehasher.prepare({ email: "a@b.c" }, live())).toEqual({ ok: false, error: missing });
        expect(await prehasher.prepare({ password: "x" }, live())).toEqual({ ok: false, error: missing });
        expect(await prehasher.prepare({ email: "a@b.c", password: 5 }, live())).toEqual({
            ok: false,
            error: missing,
        });
        expect(await prehasher.prepare({ email: "a@b.c", password: "" }, live())).toEqual({
            ok: false,
            error: missing,
        });
        // Refused before a round trip.
        expect(route.asked).toEqual([]);
        pool.close();
    });

    it("counts the byte ceiling in UTF-8 after NFC, and refuses before asking the server", async () => {
        const { prehasher, route, pool } = setup();
        // 512 × "é" is 1024 bytes and fits; 513 is 1026 and does not. A limit in
        // `.length` would have let 1024 of them through as 1024 "characters".
        expect(kMaxSecretBytes).toBe(1024);
        const tooLong = await prehasher.credential("a@b.c", "é".repeat(513), live());
        expect(tooLong).toEqual({ ok: false, error: { kind: "prehash", cause: "secret-too-long" } });
        expect(route.asked).toEqual([]);

        // Decomposed input counts AFTER composition: 512 × (e + U+0301) is 1536
        // bytes as typed and 1024 once composed, and anvil counts the latter.
        const fits = await prehasher.credential("a@b.c", "é".repeat(512), live());
        expect(fits.ok).toBe(true);
        pool.close();
    });

    it("passes a failure from the salt route through unchanged", async () => {
        const { prehasher, route, pool } = setup();
        const limited: HammerError = { kind: "server", code: "RATE_LIMITED", status: 429, requestId: null, fields: null };
        route.answer = fail(limited);
        expect(await prehasher.credential("a@b.c", "pw", live())).toEqual({ ok: false, error: limited });
        pool.close();
    });

    it("refuses an answer that is not the contract's shape", async () => {
        const bad: readonly Answer[] = [
            { ...kContractAnswer, algorithm: "argon2i" },
            { ...kContractAnswer, version: 16 },
            { ...kContractAnswer, hash_bytes: 64 },
            { ...kContractAnswer, salt: "YW52aWwtcHJlaGFzaC12" },
            { ...kContractAnswer, salt: "YW52aWwtcHJlaGFzaC12MR" },
            { ...kContractAnswer, salt: "YW52aWwtcHJlaGFzaC12M=" },
            { ...kContractAnswer, memory_kib: "64" },
            { ...kContractAnswer, iterations: 0 },
            { ...kContractAnswer, parallelism: 1.5 },
        ];
        const { prehasher, route, pool } = setup();
        for (const answer of bad) {
            route.answer = ok(answer);
            expect(await prehasher.credential("a@b.c", "pw", live())).toEqual({
                ok: false,
                error: { kind: "prehash", cause: "bad-answer" },
            });
        }
        route.answer = ok(null);
        expect((await prehasher.credential("a@b.c", "pw", live())).ok).toBe(false);
        pool.close();
    });

    it("refuses a cost outside the application's bounds, in either direction", async () => {
        const { prehasher, route, bridge, pool } = setup();
        const outside: readonly Answer[] = [
            { ...kContractAnswer, memory_kib: 262145 },
            { ...kContractAnswer, iterations: 11 },
            { ...kContractAnswer, parallelism: 5 },
        ];
        for (const answer of outside) {
            route.answer = ok(answer);
            expect(await prehasher.credential("a@b.c", "pw", live())).toEqual({
                ok: false,
                error: { kind: "prehash", cause: "out-of-bounds" },
            });
        }
        // Nothing reached a worker.
        expect(bridge.spawned()).toBe(0);
        pool.close();

        const strict = setup({ ...kBounds, minMemoryKib: 19456, minIterations: 2 });
        expect(await strict.prehasher.credential("a@b.c", "pw", live())).toEqual({
            ok: false,
            error: { kind: "prehash", cause: "out-of-bounds" },
        });
        strict.pool.close();
    });

    it("reports a device that cannot find the memory by name, across the thread boundary", async () => {
        const { prehasher, route, pool } = setup({ ...kBounds, maxMemoryKib: 0xffffffff });
        route.answer = ok({ ...kContractAnswer, memory_kib: 0xffffffff });
        expect(await prehasher.credential("a@b.c", "pw", live())).toEqual({
            ok: false,
            error: { kind: "prehash", cause: "out-of-memory" },
        });
        pool.close();
    });

    it("refuses a second hash while one runs, rather than queueing a second matrix", async () => {
        const { prehasher, route, pool } = setup();
        // 64 MiB, so the first is certainly still hashing when the second asks.
        route.answer = ok({ ...kContractAnswer, memory_kib: 65536 });
        const first = prehasher.credential("a@b.c", "one", live());
        // Let the first reach the worker before the second asks.
        await new Promise((resolve) => setTimeout(resolve, 5));
        const second = await prehasher.credential("a@b.c", "two", live());
        expect(second).toEqual({ ok: false, error: { kind: "prehash", cause: "busy" } });
        // One matrix exists, and one is the bound: 64 MiB each.
        expect(kArgon2Workers).toBe(1);
        expect(pool.inFlight).toBe(kArgon2Workers);
        expect((await first).ok).toBe(true);
        pool.close();
    });

    it("refuses bounds that cannot be right, at construction", () => {
        const { pool } = setup();
        const make = (bounds: PrehashBounds): Prehasher =>
            new Prehasher({ pool, saltFor: new SaltRoute().saltFor, bounds, fields: kFields });
        expect(() => make({ ...kBounds, minMemoryKib: 9, maxMemoryKib: 8 })).toThrow();
        expect(() => make({ ...kBounds, minIterations: 0 })).toThrow();
        expect(() => make({ ...kBounds, maxParallelism: 0 })).toThrow();
        expect(() => make({ ...kBounds, maxMemoryKib: 1.5 })).toThrow();
        pool.close();
    });
});

describe("Prehasher: cancelling", () => {
    it("answers aborted when the screen goes during the salt round trip", async () => {
        const { prehasher, route, pool } = setup();
        const release = route.hold();
        const controller = new AbortController();
        const pending = prehasher.credential("a@b.c", "pw", controller.signal);
        controller.abort();
        release();
        expect(await pending).toEqual({ ok: false, error: { kind: "prehash", cause: "aborted" } });
        pool.close();
    });

    it("terminates the worker when the screen goes mid-hash, which is what frees the matrix", async () => {
        const { prehasher, route, bridge, pool } = setup();
        route.answer = ok({ ...kContractAnswer, memory_kib: 65536 });
        const controller = new AbortController();
        const pending = prehasher.credential("a@b.c", "pw", controller.signal);
        await new Promise((resolve) => setTimeout(resolve, 5));
        controller.abort();
        expect(await pending).toEqual({ ok: false, error: { kind: "prehash", cause: "aborted" } });
        expect(bridge.terminated()).toBe(1);
        pool.close();
    });
});

describe("Prehasher: prefetching the salt", () => {
    it("uses a prefetched answer once, for the identifier it was fetched for", async () => {
        const { prehasher, route, pool } = setup();
        prehasher.prefetch("user@example.com");
        prehasher.prefetch("user@example.com");
        expect(route.asked).toEqual(["user@example.com"]);

        expect((await prehasher.credential("user@example.com", "pw", live())).ok).toBe(true);
        expect(route.asked).toEqual(["user@example.com"]);

        // Used once: a second sign-in asks again, because a password change can
        // move an account to new parameters.
        expect((await prehasher.credential("user@example.com", "pw", live())).ok).toBe(true);
        expect(route.asked).toEqual(["user@example.com", "user@example.com"]);
        pool.close();
    });

    it("aborts a prefetch for an identifier that changed, and never uses it", async () => {
        const { prehasher, route, pool } = setup();
        prehasher.prefetch("first@example.com");
        prehasher.prefetch("second@example.com");
        expect(route.signals[0]?.aborted).toBe(true);

        expect((await prehasher.credential("third@example.com", "pw", live())).ok).toBe(true);
        expect(route.signals[1]?.aborted).toBe(true);
        expect(route.asked).toEqual(["first@example.com", "second@example.com", "third@example.com"]);
        pool.close();
    });

    it("aborts an outstanding prefetch on close", () => {
        const { prehasher, route, pool } = setup();
        prehasher.prefetch("user@example.com");
        prehasher.close();
        expect(route.signals[0]?.aborted).toBe(true);
        pool.close();
    });
});
