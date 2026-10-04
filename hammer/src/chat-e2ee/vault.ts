// The vault: the one thing in hammer that persists (`docs/05-chat.md` §9.3).
//
// Encryption cannot work without it. A device's identity keys must outlive a
// reload, its ratchet state must, and what it decrypted is unrecoverable once the
// message keys are deleted, which forward secrecy requires. So this module holds
// them, names what it protects and what it does not, and is destroyed with the
// session that the device belongs to.
//
// --- what is stored, and how -----------------------------------------------------
//
// Two kinds of record and no third:
//
//   A KEY is a non-extractable `CryptoKey`, stored as itself. Script can use it and
//   can never read it, which is what makes a compromise of the page end with the
//   page: the device cannot be cloned elsewhere. A key that IS extractable is
//   refused at the write, because persisting one would quietly undo that.
//
//   BYTES are everything else — ratchet chain keys, which must be raw to be
//   chained, and decrypted messages — sealed under the vault key, a
//   non-extractable AES-256-GCM key, with a fresh nonce per write and the record's
//   slot (store and id) as associated data, so a sealed record copied into another
//   slot does not open.
//
// The sealing is for ONE purpose, and it is stated so nobody relies on it for
// another: destruction. Deleting the vault key makes every sealed record
// unreadable at once, even if the browser deletes the database lazily or the tab
// closes halfway through the wipe. It is not protection against a copy of the
// browser profile, because the vault key is stored beside what it seals.
//
// --- one writer, across every tab and the service worker -------------------------
//
// The ratchet is a state machine that must never step twice from one state: two
// writers encrypting from one chain index reuse a message key, which breaks both
// messages. So every read-modify-write runs under ONE exclusive lock per account,
// in tabs and the service worker alike — one lock rather than one per session, so
// there is no lock order to get wrong. The contention is between the tabs of one
// person.
//
// IndexedDB commits a transaction the moment a task awaits anything that is not
// IndexedDB, and every WebCrypto call is such an await. So a step reads, computes,
// and then writes everything it changed in ONE atomic commit, all under the lock.
// A crash between leaves the old state and nothing written, and the work is done
// again from the old state.
//
// A deadline (the service worker has one) bounds the WAIT for the lock and
// nothing after it. A step that has started runs to its commit, so a caller is
// never told "aborted" about a step that committed.

import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import type { Uuid } from "../core/uuid.js";
import type { ExclusiveLocks } from "../wire/leader.js";

export type VaultStoreName =
    | "meta"
    | "identity"
    | "prekeys"
    | "sessions"
    | "sender-keys"
    | "pins"
    | "outbox"
    | "processed"
    | "archive";

// What the backing holds for one slot.
export type StoredRecord =
    | { readonly kind: "sealed"; readonly nonce: Uint8Array; readonly sealed: Uint8Array }
    | { readonly kind: "key"; readonly key: CryptoKey };

// One slot written, or deleted when `record` is null.
export type RecordWrite = {
    readonly store: VaultStoreName;
    readonly id: string;
    readonly record: StoredRecord | null;
};

// The persistence underneath, as narrow as it can be: IndexedDB in a browser
// (`idb.ts`), and a stand-in in the unit suite. `commit` is all or nothing and
// rejects when it is nothing; `read` and `commit` reject when the store has been
// destroyed underneath them.
export type VaultBacking = {
    readonly read: (store: VaultStoreName, id: string) => Promise<StoredRecord | null>;
    readonly commit: (writes: readonly RecordWrite[]) => Promise<void>;
    readonly destroy: () => Promise<void>;
};

// What a step changes. A key must be non-extractable; bytes are sealed by the
// vault before they reach the backing.
export type VaultChange =
    | { readonly kind: "bytes"; readonly store: VaultStoreName; readonly id: string; readonly bytes: Uint8Array }
    | { readonly kind: "key"; readonly store: VaultStoreName; readonly id: string; readonly key: CryptoKey }
    | { readonly kind: "delete"; readonly store: VaultStoreName; readonly id: string };

export type VaultError =
    // The deadline passed while waiting for the lock. Nothing was read or written.
    | "aborted"
    // The vault key is gone: another context wiped the vault. Never recreated by a
    // step, because a vault that silently started again would look like a device
    // that forgot everything.
    | "wiped"
    // A sealed record did not open: corrupted, or moved from another slot.
    | "tampered"
    // The backing refused a read or a commit.
    | "storage";

export type VaultReader = {
    readonly bytes: (store: VaultStoreName, id: string) => Promise<Result<Uint8Array | null, VaultError>>;
    readonly key: (store: VaultStoreName, id: string) => Promise<Result<CryptoKey | null, VaultError>>;
};

export type StepOutcome<T> = {
    readonly value: T;
    readonly changes: readonly VaultChange[];
};

const kVaultKeyId = "vault-key";
const kNonceBytes = 12;

function lockName(account: Uuid): string {
    return `hammer.chat.vault.${account.format()}`;
}

function slot(store: VaultStoreName, id: string): Uint8Array<ArrayBuffer> {
    // A NUL cannot appear in either half, so the pair is one unambiguous string.
    return new TextEncoder().encode(`${store}\u0000${id}`);
}

type Locked<T> = { readonly ran: true; readonly value: T } | { readonly ran: false };

export class Vault {
    private readonly backing: VaultBacking;
    private readonly locks: ExclusiveLocks;
    private readonly account: Uuid;

    private constructor(backing: VaultBacking, locks: ExclusiveLocks, account: Uuid) {
        this.backing = backing;
        this.locks = locks;
        this.account = account;
    }

    // Opens the account's vault, making its key on first use. Under the lock,
    // because two tabs opening a new vault at once would otherwise each make a
    // key, and every record the loser sealed would never open again.
    static async open(options: {
        readonly backing: VaultBacking;
        readonly locks: ExclusiveLocks;
        readonly account: Uuid;
        readonly signal: AbortSignal;
    }): Promise<Result<Vault, VaultError>> {
        const vault = new Vault(options.backing, options.locks, options.account);
        const opened = await vault.locked(options.signal, async (): Promise<Result<void, VaultError>> => {
            const existing = await vault.readRecord("meta", kVaultKeyId);
            if (!existing.ok) {
                return existing;
            }
            if (existing.value !== null) {
                return existing.value.kind === "key" ? ok() : fail("tampered");
            }
            const key = await crypto.subtle.generateKey({ name: "AES-GCM", length: 256 }, false, [
                "encrypt",
                "decrypt",
            ]);
            return vault.commitWrites([{ store: "meta", id: kVaultKeyId, record: { kind: "key", key } }]);
        });
        if (!opened.ok) {
            return opened;
        }
        return opened.value.ok ? ok(vault) : opened.value;
    }

    // One read-modify-write. `work` reads through the reader and answers what it
    // changed; the changes are sealed and committed in one atomic write before the
    // lock is released. A `work` that throws is a bug in its caller, and the
    // exception is the caller's, with nothing committed.
    async step<T>(
        signal: AbortSignal,
        work: (reader: VaultReader) => Promise<StepOutcome<T>>,
    ): Promise<Result<T, VaultError>> {
        const done = await this.locked(signal, async (): Promise<Result<T, VaultError>> => {
            const vaultKey = await this.vaultKey();
            if (!vaultKey.ok) {
                return vaultKey;
            }
            const reader = this.reader(vaultKey.value);
            const outcome = await work(reader);
            const writes: RecordWrite[] = [];
            for (const change of outcome.changes) {
                const write = await this.toWrite(vaultKey.value, change);
                writes.push(write);
            }
            const committed = await this.commitWrites(writes);
            return committed.ok ? ok(outcome.value) : committed;
        });
        return done.ok ? done.value : done;
    }

    // Destroys the vault: the key first, so every sealed record is unreadable from
    // that instant, then the database. The lock is waited for until the deadline
    // and no longer: a frozen tab holding it must not keep a signed-out person's
    // messages on the device, and a writer that commits after the key is gone
    // writes records nothing can open.
    async wipe(signal: AbortSignal): Promise<Result<void, VaultError>> {
        const wiped = await this.locked(signal, () => this.destroyNow());
        if (wiped.ok) {
            return wiped.value;
        }
        return this.destroyNow();
    }

    private async destroyNow(): Promise<Result<void, VaultError>> {
        const forgotten = await this.commitWrites([{ store: "meta", id: kVaultKeyId, record: null }]);
        try {
            await this.backing.destroy();
        } catch {
            return fail("storage");
        }
        return forgotten;
    }

    private async locked<T>(signal: AbortSignal, body: () => Promise<T>): Promise<Result<T, VaultError>> {
        if (signal.aborted) {
            return fail("aborted");
        }
        let started = false;
        let onAbort: (() => void) | null = null;
        const abandoned = new Promise<Locked<T>>((resolve) => {
            onAbort = () => {
                // Only while waiting. A step that has started runs to its commit.
                if (!started) {
                    resolve({ ran: false });
                }
            };
            signal.addEventListener("abort", onAbort, { once: true });
        });
        const granted = this.locks.request<Locked<T>>(lockName(this.account), { ifAvailable: false }, async () => {
            // Granted after the deadline: the caller has gone, so do nothing.
            if (signal.aborted) {
                return { ran: false };
            }
            started = true;
            return { ran: true, value: await body() };
        });
        try {
            const first = await Promise.race([granted, abandoned]);
            return first.ran ? ok(first.value) : fail("aborted");
        } finally {
            if (onAbort !== null) {
                signal.removeEventListener("abort", onAbort);
            }
        }
    }

    private async readRecord(store: VaultStoreName, id: string): Promise<Result<StoredRecord | null, VaultError>> {
        try {
            return ok(await this.backing.read(store, id));
        } catch {
            return fail("storage");
        }
    }

    private async commitWrites(writes: readonly RecordWrite[]): Promise<Result<void, VaultError>> {
        if (writes.length === 0) {
            return ok();
        }
        try {
            await this.backing.commit(writes);
            return ok();
        } catch {
            return fail("storage");
        }
    }

    private async vaultKey(): Promise<Result<CryptoKey, VaultError>> {
        const stored = await this.readRecord("meta", kVaultKeyId);
        if (!stored.ok) {
            return stored;
        }
        if (stored.value === null) {
            return fail("wiped");
        }
        return stored.value.kind === "key" ? ok(stored.value.key) : fail("tampered");
    }

    private reader(vaultKey: CryptoKey): VaultReader {
        return {
            bytes: async (store, id) => {
                if (store === "meta" && id === kVaultKeyId) {
                    throw new Error("the vault key is not a record a step reads");
                }
                const stored = await this.readRecord(store, id);
                if (!stored.ok) {
                    return stored;
                }
                if (stored.value === null) {
                    return ok(null);
                }
                if (stored.value.kind !== "sealed") {
                    return fail("tampered");
                }
                try {
                    const opened = await crypto.subtle.decrypt(
                        { name: "AES-GCM", iv: Uint8Array.from(stored.value.nonce), additionalData: slot(store, id) },
                        vaultKey,
                        Uint8Array.from(stored.value.sealed),
                    );
                    return ok(new Uint8Array(opened));
                } catch {
                    return fail("tampered");
                }
            },
            key: async (store, id) => {
                if (store === "meta" && id === kVaultKeyId) {
                    throw new Error("the vault key is not a record a step reads");
                }
                const stored = await this.readRecord(store, id);
                if (!stored.ok) {
                    return stored;
                }
                if (stored.value === null) {
                    return ok(null);
                }
                return stored.value.kind === "key" ? ok(stored.value.key) : fail("tampered");
            },
        };
    }

    private async toWrite(vaultKey: CryptoKey, change: VaultChange): Promise<RecordWrite> {
        if (change.store === "meta" && change.id === kVaultKeyId) {
            throw new Error("the vault key is not a record a step writes");
        }
        switch (change.kind) {
            case "delete":
                return { store: change.store, id: change.id, record: null };
            case "key":
                // Persisting an extractable key would make the page's compromise
                // outlive the page. This is the one place that could let it.
                if (change.key.extractable) {
                    throw new Error("only a non-extractable key is stored");
                }
                return { store: change.store, id: change.id, record: { kind: "key", key: change.key } };
            case "bytes": {
                const nonce = crypto.getRandomValues(new Uint8Array(kNonceBytes));
                const sealed = await crypto.subtle.encrypt(
                    { name: "AES-GCM", iv: nonce, additionalData: slot(change.store, change.id) },
                    vaultKey,
                    Uint8Array.from(change.bytes),
                );
                return {
                    store: change.store,
                    id: change.id,
                    record: { kind: "sealed", nonce, sealed: new Uint8Array(sealed) },
                };
            }
        }
    }
}
