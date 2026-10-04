// A vault backing that lives in one test process, shared by as many "tabs" as a
// test makes.
//
// A stand-in rather than a mock, for the test plan's reason: it models the two
// properties the vault rests on — a commit is all or nothing, and a destroyed
// store refuses every read and write — plus a way to make the next commit fail,
// which is the crash between a step's read and its write. The real IndexedDB, its
// transactions and its deletion run in the browser suite.

import type { RecordWrite, StoredRecord, VaultBacking, VaultStoreName } from "../../src/chat-e2ee/vault.js";

function copy(record: StoredRecord): StoredRecord {
    // IndexedDB stores by structured clone, so a caller mutating its array after a
    // commit must not change what was stored.
    return record.kind === "key"
        ? record
        : { kind: "sealed", nonce: Uint8Array.from(record.nonce), sealed: Uint8Array.from(record.sealed) };
}

export class MemoryVaultStore {
    readonly records = new Map<string, StoredRecord>();
    destroyed = false;
    failNextCommit = false;
    commits = 0;

    private static slot(store: VaultStoreName, id: string): string {
        return `${store}/${id}`;
    }

    backing(): VaultBacking {
        return {
            read: async (store, id) => {
                if (this.destroyed) {
                    throw new Error("destroyed");
                }
                const found = this.records.get(MemoryVaultStore.slot(store, id));
                return found === undefined ? null : copy(found);
            },
            commit: async (writes: readonly RecordWrite[]) => {
                if (this.destroyed) {
                    throw new Error("destroyed");
                }
                if (this.failNextCommit) {
                    this.failNextCommit = false;
                    throw new Error("the commit failed");
                }
                // All or nothing: nothing is applied until every write is known.
                const staged = writes.map((write) => [MemoryVaultStore.slot(write.store, write.id), write.record] as const);
                for (const [slot, record] of staged) {
                    if (record === null) {
                        this.records.delete(slot);
                    } else {
                        this.records.set(slot, copy(record));
                    }
                }
                this.commits += 1;
            },
            destroy: async () => {
                this.records.clear();
                this.destroyed = true;
            },
        };
    }

    // Moves one record into another slot, as somebody with the profile's files
    // could.
    move(from: [VaultStoreName, string], to: [VaultStoreName, string]): void {
        const record = this.records.get(MemoryVaultStore.slot(...from));
        if (record !== undefined) {
            this.records.set(MemoryVaultStore.slot(...to), record);
        }
    }
}
