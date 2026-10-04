// The admin panel's one connection to the server, built from hammer and the
// client generated from the server's own descriptor (src/api).
//
// No token is held anywhere in this bundle. anvil's credentials are __Host-
// cookies with HttpOnly; every request is same-origin and carries them, and
// hammer owns the single refresh across tabs.

import type { Client } from "hammer/wire";
import { createClient } from "hammer/wire";
import { SessionStore } from "hammer/state";
import { Accounts, accountCall } from "hammer/accounts";
import type { PrehashBounds } from "hammer/prehash";
import { Argon2Pool, Prehasher } from "hammer/prehash";

import type { Api } from "../api/hammer.generated";
import {
    kAccounts,
    kApiTables,
    kPermissionBits,
    kTablesHash,
    routeAuthLogout,
    routeAuthRefresh,
    routeAuthSalt,
    routeSessionCurrent,
} from "../api/hammer.generated";
import "./responses";

// What a salt answer may ask of a device. The server asks Argon2id at 64 MiB and
// three passes; anything outside these bounds is a misconfigured server, and the
// prehash refuses it rather than hashing whatever was named.
const kPrehashBounds: PrehashBounds = {
    minMemoryKib: 19456,
    maxMemoryKib: 131072,
    minIterations: 2,
    maxIterations: 6,
    maxParallelism: 4,
};

export type Platform = {
    readonly api: Client<Api>;
    readonly session: SessionStore;
    readonly accounts: Accounts;
    // Derives the credential for SOMEONE ELSE's new password, when staff create
    // an account: the browser hashes it under the enrolment salt for that email,
    // so the password itself never leaves this tab either.
    readonly enrolment: Prehasher;
    readonly signOut: () => Promise<void>;
};

function build(): Platform {
    const session = new SessionStore({
        clientHash: kTablesHash,
        permissionBits: kPermissionBits,
    });

    const api = createClient<Api>({
        api: kApiTables,
        refreshRoute: routeAuthRefresh,
        origin: { pageOrigin: window.location.origin, apiOrigin: window.location.origin, site: null },
        fetch: (input, init) => window.fetch(input, init),
        session: session.source,
        onLogout: () => session.clear(),
    });
    session.readsFrom((signal) => api.call(routeSessionCurrent, { signal }));

    const argon2 = new Argon2Pool({
        create: () => new Worker(new URL("./prehash_worker.ts", import.meta.url), { type: "module" }),
    });
    if (kAccounts === null) {
        throw new Error("the server's descriptor declares no account flows");
    }
    const accounts = new Accounts({
        table: kAccounts,
        call: accountCall(api),
        prehash: { pool: argon2, bounds: kPrehashBounds },
    });
    const call = accountCall(api);
    const enrolment = new Prehasher({
        pool: argon2,
        bounds: kPrehashBounds,
        saltFor: async (identifier, purpose, signal) =>
            await call(routeAuthSalt, { identifier, purpose }, signal),
    });

    // The route first, then the local clear, unconditionally: clearing first
    // would leave a revoked-looking tab in front of a live session, and skipping
    // the clear on a failed call would leave a signed-in shell in front of a dead
    // one.
    const signOut = async () => {
        const controller = new AbortController();
        await api.call(routeAuthLogout, { body: {}, signal: controller.signal });
        api.logout();
        session.clear();
    };

    return { api, session, accounts, enrolment, signOut };
}

export const platform: Platform = build();
export const api = platform.api;
