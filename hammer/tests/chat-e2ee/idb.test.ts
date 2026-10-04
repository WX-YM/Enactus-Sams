// The vault over the real IndexedDB and the real lock manager.
//
// Runs in a Chromium page, driven by `tests/dom/in_browser.test.ts`, in a browser
// context of its own, so the database each case opens is this file's alone. The
// unit suite proves the vault's logic over a stand-in; this proves the three
// properties that stand-in only models: a commit that is all or nothing, a key
// that survives the database as a key, and a deletion that every connection gets
// out of the way of.

import { describe, expect, it } from "../support/test.js";

import { openVaultStore, vaultDatabaseName } from "../../src/chat-e2ee/idb.js";
import type { VaultBacking } from "../../src/chat-e2ee/vault.js";
import { Vault } from "../../src/chat-e2ee/vault.js";
import { Uuid } from "../../src/core/uuid.js";
import { locksFrom } from "../../src/wire/leader.js";

function never(): AbortSignal {
    return new AbortController().signal;
}

async function backing(account: Uuid): Promise<VaultBacking> {
    const opened = await openVaultStore(indexedDB, account);
    if (!opened.ok) {
        throw new Error("the vault database did not open");
    }
    return opened.value;
}

async function vaultFor(account: Uuid): Promise<Vault> {
    const opened = await Vault.open({ backing: await backing(account), locks: locksFrom(navigator.locks), account, signal: never() });
    if (!opened.ok) {
        throw new Error(`the vault did not open: ${opened.error}`);
    }
    return opened.value;
}

async function databaseExists(name: string): Promise<boolean> {
    const all = await indexedDB.databases();
    return all.some((database) => database.name === name);
}

describe("the vault over IndexedDB", () => {
    it("reads back across a fresh connection what a step committed", async () => {
        const account = Uuid.random();
        const first = await vaultFor(account);
        const wrote = await first.step(never(), async () => ({
            value: null,
            changes: [{ kind: "bytes", store: "archive", id: "c:1", bytes: new TextEncoder().encode("a decrypted message") }],
        }));
        expect(wrote.ok).toBe(true);

        const second = await vaultFor(account);
        const read = await second.step(never(), async (reader) => ({ value: await reader.bytes("archive", "c:1"), changes: [] }));
        expect(read.ok && read.value.ok && new TextDecoder().decode(read.value.value ?? new Uint8Array(0))).toBe(
            "a decrypted message",
        );
    });

    it("keeps a key a key: usable, and still not extractable, after a reload", async () => {
        const account = Uuid.random();
        const signing = await crypto.subtle.generateKey({ name: "Ed25519" }, false, ["sign", "verify"]);
        if (!("privateKey" in signing)) {
            throw new Error("Ed25519 made one key");
        }
        const vault = await vaultFor(account);
        await vault.step(never(), async () => ({
            value: null,
            changes: [{ kind: "key", store: "identity", id: "signing", key: signing.privateKey }],
        }));

        const reopened = await vaultFor(account);
        const read = await reopened.step(never(), async (reader) => ({ value: await reader.key("identity", "signing"), changes: [] }));
        const key = read.ok && read.value.ok ? read.value.value : null;
        expect(key?.extractable).toBe(false);
        expect(key === null).toBe(false);
        if (key === null) return;
        const signature = await crypto.subtle.sign({ name: "Ed25519" }, key, new Uint8Array(8));
        expect(await crypto.subtle.verify({ name: "Ed25519" }, signing.publicKey, signature, new Uint8Array(8))).toBe(true);
        let exported = true;
        try {
            await crypto.subtle.exportKey("pkcs8", key);
        } catch {
            exported = false;
        }
        expect(exported).toBe(false);
    });

    it("lands nothing of a commit that fails partway", async () => {
        const account = Uuid.random();
        const store = await backing(account);
        let refused = false;
        try {
            await store.commit([
                { store: "sessions", id: "good", record: { kind: "sealed", nonce: new Uint8Array(12), sealed: new Uint8Array(4) } },
                // A function cannot be cloned into the database, so this put throws
                // after the first has been queued in the same transaction.
                { store: "sessions", id: "bad", record: (() => 0) as unknown as null },
            ]);
        } catch {
            refused = true;
        }
        expect(refused).toBe(true);
        expect(await store.read("sessions", "good")).toBe(null);
    });

    it("deletes the database past another open connection, and that connection then refuses", async () => {
        const account = Uuid.random();
        const mine = await vaultFor(account);
        const theirs = await vaultFor(account);
        await mine.step(never(), async () => ({
            value: null,
            changes: [{ kind: "bytes", store: "archive", id: "c:1", bytes: new Uint8Array(3) }],
        }));
        expect((await mine.wipe(never())).ok).toBe(true);
        expect(await databaseExists(vaultDatabaseName(account))).toBe(false);

        // The other connection closed itself on versionchange, so the deletion
        // was not blocked, and it does not quietly reopen an empty database.
        const after = await theirs.step(never(), async () => ({ value: null, changes: [] }));
        expect(after.ok).toBe(false);
        expect(await databaseExists(vaultDatabaseName(account))).toBe(false);
    });
});
