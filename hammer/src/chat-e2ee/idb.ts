// The vault's backing in a browser: one IndexedDB database per account.
//
// This is the only file in hammer allowed IndexedDB, and `tools/check-source-bans.sh`
// holds it to that: every other file is refused the global and every `IDB*` type.
// The rest of the library persists nothing (`docs/00-architecture.md` §7); this
// persists a device's keys and what it decrypted, because nothing else can hold
// them, and the vault destroys it with the session (`docs/05-chat.md` §9.3).
//
// The factory is a parameter, as every platform singleton is (`CLAUDE.md` §3.3):
// a page passes `indexedDB`, a service worker passes its own, and a test passes a
// fresh one per browser context.
//
// --- three properties the vault relies on -----------------------------------------
//
//   A COMMIT IS ALL OR NOTHING. Every write a step made is one readwrite
//   transaction, resolved only on `complete`. An error or an abort rejects, and
//   IndexedDB has applied none of it.
//
//   A COMMIT IS ON DISK WHEN IT RESOLVES. `durability: "strict"`, because a
//   browser's default may report a commit before it is flushed, and a ratchet
//   step that is acknowledged, sent, and then lost to a crash is a chain index
//   the device will use a second time.
//
//   A DESTROYED STORE IS GONE FOR EVERY CONTEXT. A deletion is blocked while any
//   connection is open, so every connection closes itself on `versionchange`, and
//   reads and writes after that reject rather than reopening a fresh, empty
//   database under a vault that has just been wiped.

import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import type { Uuid } from "../core/uuid.js";

import type { RecordWrite, StoredRecord, VaultBacking, VaultStoreName } from "./vault.js";

const kVersion = 1;

const kStores: readonly VaultStoreName[] = [
    "meta",
    "identity",
    "prekeys",
    "sessions",
    "sender-keys",
    "pins",
    "outbox",
    "processed",
    "archive",
];

export function vaultDatabaseName(account: Uuid): string {
    return `hammer.chat.${account.format()}`;
}

// What came out of the database is `unknown` until it has the shape the vault
// wrote, because a database is a boundary like any other: an older build, a
// browser extension or a person with the profile's files may have written it.
function asRecord(value: unknown): StoredRecord | null | "malformed" {
    if (value === undefined) {
        return null;
    }
    if (typeof value !== "object" || value === null) {
        return "malformed";
    }
    const record = value as { readonly kind?: unknown; readonly key?: unknown; readonly nonce?: unknown; readonly sealed?: unknown };
    if (record.kind === "key" && record.key instanceof CryptoKey) {
        return { kind: "key", key: record.key };
    }
    if (record.kind === "sealed" && record.nonce instanceof Uint8Array && record.sealed instanceof Uint8Array) {
        return { kind: "sealed", nonce: record.nonce, sealed: record.sealed };
    }
    return "malformed";
}

// The rejections below carry a code and not a sentence: the vault turns every one
// into `storage`, so no person reads them, and the stack is for whoever debugs.
function settle<T>(request: IDBRequest<T>): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        request.onsuccess = () => {
            resolve(request.result);
        };
        request.onerror = () => {
            reject(request.error ?? new Error("vault.request"));
        };
    });
}

export async function openVaultStore(factory: IDBFactory, account: Uuid): Promise<Result<VaultBacking, "storage">> {
    const name = vaultDatabaseName(account);
    let database: IDBDatabase;
    try {
        database = await new Promise<IDBDatabase>((resolve, reject) => {
            const request = factory.open(name, kVersion);
            request.onupgradeneeded = () => {
                for (const store of kStores) {
                    if (!request.result.objectStoreNames.contains(store)) {
                        request.result.createObjectStore(store);
                    }
                }
            };
            request.onsuccess = () => {
                resolve(request.result);
            };
            request.onerror = () => {
                reject(request.error ?? new Error("vault.open"));
            };
            // Another context holds an older version open. It closes itself on
            // `versionchange`, and the open then succeeds.
        });
    } catch {
        return fail("storage");
    }

    let closed = false;
    const close = (): void => {
        if (!closed) {
            closed = true;
            database.close();
        }
    };
    // Another context is deleting this database: get out of its way, and refuse
    // everything from now on.
    database.onversionchange = close;
    database.onclose = () => {
        closed = true;
    };

    const backing: VaultBacking = {
        read: async (store, id) => {
            if (closed) {
                throw new Error("the vault database is closed");
            }
            const value: unknown = await settle(database.transaction(store, "readonly").objectStore(store).get(id));
            const record = asRecord(value);
            if (record === "malformed") {
                throw new Error("a vault record has a shape the vault never writes");
            }
            return record;
        },
        commit: async (writes: readonly RecordWrite[]) => {
            if (closed) {
                throw new Error("the vault database is closed");
            }
            const stores = [...new Set(writes.map((write) => write.store))];
            const transaction = database.transaction(stores, "readwrite", { durability: "strict" });
            const done = new Promise<void>((resolve, reject) => {
                transaction.oncomplete = () => {
                    resolve();
                };
                transaction.onabort = () => {
                    reject(transaction.error ?? new Error("vault.commit-aborted"));
                };
                transaction.onerror = () => {
                    reject(transaction.error ?? new Error("vault.commit-failed"));
                };
            });
            try {
                for (const write of writes) {
                    const store = transaction.objectStore(write.store);
                    if (write.record === null) {
                        store.delete(write.id);
                    } else {
                        store.put(write.record, write.id);
                    }
                }
            } catch (error) {
                // A put that throws synchronously (a value that cannot be cloned)
                // leaves the transaction open; abort it so nothing of this commit
                // lands.
                transaction.abort();
                await done.catch(() => undefined);
                throw error;
            }
            await done;
        },
        destroy: async () => {
            close();
            await new Promise<void>((resolve, reject) => {
                const request = factory.deleteDatabase(name);
                request.onsuccess = () => {
                    resolve();
                };
                request.onerror = () => {
                    reject(request.error ?? new Error("vault.delete"));
                };
                // `blocked` fires while another context still holds a connection.
                // Each closes itself on `versionchange`, and `success` follows.
            });
        },
    };
    return ok(backing);
}
