// The vault's guarantees, each driven to the failure it exists to prevent.

import { describe, expect, it } from "../support/test.js";

import type { VaultChange, VaultReader } from "../../src/chat-e2ee/vault.js";
import { Vault } from "../../src/chat-e2ee/vault.js";
import { Uuid } from "../../src/core/uuid.js";
import { LockRoom } from "../support/fake_locks.js";
import { MemoryVaultStore } from "../support/memory_vault.js";

const kAccount = Uuid.random();

function never(): AbortSignal {
    return new AbortController().signal;
}

async function open(store: MemoryVaultStore, room: LockRoom): Promise<Vault> {
    const opened = await Vault.open({ backing: store.backing(), locks: room.tab(), account: kAccount, signal: never() });
    if (!opened.ok) {
        throw new Error(`the vault did not open: ${opened.error}`);
    }
    return opened.value;
}

function bytes(text: string): Uint8Array {
    return new TextEncoder().encode(text);
}

async function readText(vault: Vault, id: string): Promise<string | null | "error"> {
    const read = await vault.step(never(), async (reader: VaultReader) => {
        const found = await reader.bytes("sessions", id);
        return { value: found, changes: [] };
    });
    if (!read.ok || !read.value.ok) {
        return "error";
    }
    return read.value.value === null ? null : new TextDecoder().decode(read.value.value);
}

async function write(vault: Vault, id: string, text: string): Promise<boolean> {
    const done = await vault.step(never(), async () => ({
        value: true,
        changes: [{ kind: "bytes", store: "sessions", id, bytes: bytes(text) } satisfies VaultChange],
    }));
    return done.ok;
}

describe("opening", () => {
    it("makes one vault key, even when two tabs open a new vault at once", async () => {
        const store = new MemoryVaultStore();
        const room = new LockRoom();
        const [first, second] = await Promise.all([open(store, room), open(store, room)]);
        // Two keys would mean whatever the loser sealed never opens again.
        expect(await write(first, "s", "sealed by the first tab")).toBe(true);
        expect(await readText(second, "s")).toBe("sealed by the first tab");
        const keys = [...store.records.entries()].filter(([slot]) => slot === "meta/vault-key");
        expect(keys.length).toBe(1);
    });

    it("keeps the vault key non-extractable", async () => {
        const store = new MemoryVaultStore();
        await open(store, new LockRoom());
        const record = store.records.get("meta/vault-key");
        expect(record?.kind === "key" && record.key.extractable).toBe(false);
    });
});

describe("a step", () => {
    it("seals bytes, so nothing stored is the plaintext", async () => {
        const store = new MemoryVaultStore();
        const vault = await open(store, new LockRoom());
        expect(await write(vault, "s", "chain key bytes")).toBe(true);
        const record = store.records.get("sessions/s");
        expect(record?.kind).toBe("sealed");
        const stored = record?.kind === "sealed" ? new TextDecoder().decode(record.sealed) : "";
        expect(stored.includes("chain key")).toBe(false);
        expect(await readText(vault, "s")).toBe("chain key bytes");
    });

    it("refuses a sealed record moved into another slot", async () => {
        const store = new MemoryVaultStore();
        const vault = await open(store, new LockRoom());
        await write(vault, "alice", "alice's session");
        store.move(["sessions", "alice"], ["sessions", "mallory"]);
        const read = await vault.step(never(), async (reader) => ({
            value: await reader.bytes("sessions", "mallory"),
            changes: [],
        }));
        expect(read.ok && !read.value.ok && read.value.error).toBe("tampered");
    });

    it("refuses to store an extractable key, and commits nothing of that step", async () => {
        const store = new MemoryVaultStore();
        const vault = await open(store, new LockRoom());
        const extractable = await crypto.subtle.generateKey({ name: "AES-GCM", length: 256 }, true, ["encrypt"]);
        const commitsBefore = store.commits;
        let threw = false;
        try {
            await vault.step(never(), async () => ({
                value: null,
                changes: [
                    { kind: "bytes", store: "sessions", id: "x", bytes: bytes("x") },
                    { kind: "key", store: "identity", id: "signing", key: extractable },
                ],
            }));
        } catch {
            threw = true;
        }
        expect(threw).toBe(true);
        expect(store.commits).toBe(commitsBefore);
        expect(store.records.has("sessions/x")).toBe(false);
    });

    it("leaves the old state whole when the commit fails, which is the crash between", async () => {
        const store = new MemoryVaultStore();
        const vault = await open(store, new LockRoom());
        await write(vault, "s", "state 1");
        store.failNextCommit = true;
        const failed = await vault.step(never(), async () => ({
            value: null,
            changes: [
                { kind: "bytes", store: "sessions", id: "s", bytes: bytes("state 2") },
                { kind: "bytes", store: "outbox", id: "m", bytes: bytes("the body encrypted from state 2") },
            ],
        }));
        expect(failed.ok || failed.error).toBe("storage");
        // Neither half: the ratchet did not advance and no body exists for it.
        expect(await readText(vault, "s")).toBe("state 1");
        expect(store.records.has("outbox/m")).toBe(false);
    });

    it("releases the lock when the work throws", async () => {
        const store = new MemoryVaultStore();
        const room = new LockRoom();
        const vault = await open(store, room);
        let threw = false;
        try {
            await vault.step(never(), async () => {
                throw new Error("a bug in the caller");
            });
        } catch {
            threw = true;
        }
        expect(threw).toBe(true);
        expect(await write(vault, "s", "after")).toBe(true);
    });

    it("serialises two tabs, so no state is stepped from twice", async () => {
        // The ratchet's property in miniature: each step reads a counter, waits on
        // WebCrypto, and writes it back plus one. Unserialised, two tabs read the
        // same value and one increment is lost — which in a ratchet is one message
        // key used twice.
        const store = new MemoryVaultStore();
        const room = new LockRoom();
        const tabs = await Promise.all([open(store, room), open(store, room)]);
        const increment = (vault: Vault): Promise<unknown> =>
            vault.step(never(), async (reader) => {
                const read = await reader.bytes("sessions", "counter");
                const current = read.ok && read.value !== null ? (read.value[0] ?? 0) : 0;
                await crypto.subtle.digest("SHA-256", new Uint8Array(1));
                return { value: null, changes: [{ kind: "bytes", store: "sessions", id: "counter", bytes: Uint8Array.of(current + 1) }] };
            });
        const steps: Promise<unknown>[] = [];
        for (let i = 0; i < 20; i += 1) {
            steps.push(increment(tabs[i % 2] ?? tabs[0] as Vault));
        }
        await Promise.all(steps);
        const final = await tabs[0]?.step(never(), async (reader) => ({ value: await reader.bytes("sessions", "counter"), changes: [] }));
        expect(final?.ok && final.value.ok && final.value.value?.[0]).toBe(20);
    });

    it("does not let a step read or write the vault key", async () => {
        const vault = await open(new MemoryVaultStore(), new LockRoom());
        let threw = 0;
        for (const attempt of [
            () => vault.step(never(), async (reader) => ({ value: await reader.key("meta", "vault-key"), changes: [] })),
            () => vault.step(never(), async () => ({ value: null, changes: [{ kind: "delete", store: "meta", id: "vault-key" }] })),
        ]) {
            try {
                await attempt();
            } catch {
                threw += 1;
            }
        }
        expect(threw).toBe(2);
    });
});

describe("the deadline", () => {
    it("bounds the wait for the lock, and the work never runs after it", async () => {
        const store = new MemoryVaultStore();
        const room = new LockRoom();
        const vault = await open(store, room);
        // Another context holds the lock, as a frozen tab would.
        let release = (): void => {};
        const held = room.tab().request(`hammer.chat.vault.${kAccount.format()}`, { ifAvailable: false }, () => new Promise<void>((resolve) => {
            release = resolve;
        }));
        const deadline = new AbortController();
        let ran = false;
        const waiting = vault.step(deadline.signal, async () => {
            ran = true;
            return { value: null, changes: [] };
        });
        deadline.abort();
        const answer = await waiting;
        expect(answer.ok || answer.error).toBe("aborted");
        release();
        await held;
        // Granted after the caller left: nothing runs.
        await vault.step(never(), async () => ({ value: null, changes: [] }));
        expect(ran).toBe(false);
    });

    it("does not abort a step that has started", async () => {
        const vault = await open(new MemoryVaultStore(), new LockRoom());
        const deadline = new AbortController();
        const done = await vault.step(deadline.signal, async () => {
            deadline.abort();
            return { value: "committed", changes: [{ kind: "bytes", store: "sessions", id: "s", bytes: bytes("x") }] };
        });
        expect(done.ok && done.value).toBe("committed");
    });
});

describe("wiping", () => {
    it("forgets the key and destroys the store, and another tab's next step is wiped", async () => {
        const store = new MemoryVaultStore();
        const room = new LockRoom();
        const [mine, theirs] = await Promise.all([open(store, room), open(store, room)]);
        await write(mine, "s", "a decrypted message");
        expect((await mine.wipe(never())).ok).toBe(true);
        expect(store.destroyed).toBe(true);
        const after = await theirs.step(never(), async () => ({ value: null, changes: [] }));
        expect(after.ok).toBe(false);
    });

    it("never recreates a key a wipe forgot", async () => {
        // The key is deleted before the store is destroyed, so a step landing
        // between the two finds no key and stops, rather than sealing new records
        // under a new vault that looks like a device that forgot everything.
        const store = new MemoryVaultStore();
        const vault = await open(store, new LockRoom());
        store.records.delete("meta/vault-key");
        const step = await vault.step(never(), async () => ({ value: null, changes: [] }));
        expect(step.ok || step.error).toBe("wiped");
        expect(store.records.has("meta/vault-key")).toBe(false);
    });

    it("goes ahead past the deadline when another context holds the lock", async () => {
        const store = new MemoryVaultStore();
        const room = new LockRoom();
        const vault = await open(store, room);
        await write(vault, "s", "a decrypted message");
        let release = (): void => {};
        const held = room.tab().request(`hammer.chat.vault.${kAccount.format()}`, { ifAvailable: false }, () => new Promise<void>((resolve) => {
            release = resolve;
        }));
        const deadline = new AbortController();
        const wiping = vault.wipe(deadline.signal);
        deadline.abort();
        expect((await wiping).ok).toBe(true);
        expect(store.destroyed).toBe(true);
        release();
        await held;
    });
});
