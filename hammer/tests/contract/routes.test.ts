// Every route anvil describes, built by this client.
//
// The addresses in the descriptor are the ones the reference application serves,
// and half of them are never emitted as values: a holder route's path is
// withheld (`docs/01-seams.md` §4.1) and arrives with the session, as one string
// per route id. So this drives the path a real client drives — the session
// table, through the real decode, through `resolveRoute`, through `buildRoute` —
// over every route the server actually has, rather than over the handful a unit
// test happens to name.
//
// What it is looking for is narrow: a pattern this client cannot parse, a
// parameter this client would concatenate rather than encode, and a route
// described with a method that is not one.

import { readFileSync } from "node:fs";

import { describe, expect, it } from "../support/test.js";

import { buildRoute, requestPath } from "../../src/wire/route.js";
import { decodeSessionView } from "../../src/wire/session_view.js";
import { resolveRoute } from "../../src/wire/resolve.js";
import { kBits, kServerHash, sessionPayload } from "../support/session.js";

type DescribedRoute = {
    readonly id: string;
    readonly method: string;
    readonly path: string;
    readonly visibility: "public" | "holder";
    readonly idempotent: boolean;
    readonly page: unknown;
};

const kRoutes = (
    JSON.parse(
        readFileSync(new URL("../testapp/hammer.descriptor.json", import.meta.url), "utf8"),
    ) as { readonly tables: { readonly routes: readonly DescribedRoute[] } }
).tables.routes;

// The methods a client can be told to use. `ANY` is anvil's own word for a
// description that forgot to name one, emitted so that a generated client fails
// loudly rather than defaulting to GET.
const kUsable = kRoutes.filter((route) => route.method !== "ANY");
const kUnusable = kRoutes.filter((route) => route.method === "ANY");

// The parameters in a pattern, and a value for each that is hostile on purpose:
// a slash would escape the path segment, a space and an Arabic letter are what
// the encoder is for, and a `?` would begin a query string that nothing put
// there.
const kHostile = "a/b c?d#e ت";

function parametersFor(path: string): Record<string, string> {
    const params: Record<string, string> = {};
    for (const match of path.matchAll(/\{([^}]+)\}/g)) {
        params[match[1] ?? ""] = kHostile;
    }
    return params;
}

// The session table a server builds: one entry per route id, `"<METHOD> <path>"`.
// It is the shape anvil's reference application records over a real listener
// (`docs/15-tasks.md` §Cross-repo), and every route goes in it — including the
// ones described with no usable method, because a real table carries one the day
// a description forgets a method.
function sessionTable(): Readonly<Record<string, string>> {
    const table: Record<string, string> = {};
    for (const route of kRoutes) {
        table[route.id] = `${route.method} ${route.path}`;
    }
    return table;
}

const kView = (() => {
    const decoded = decodeSessionView(
        sessionPayload({ routes: sessionTable(), hash: kServerHash }),
        kBits,
    );
    if (!decoded.ok) {
        throw new Error(`the descriptor's own route table does not decode: ${decoded.error}`);
    }
    return decoded.value;
})();

describe("the route table anvil describes", () => {
    it("has routes to build", () => {
        expect(kUsable.length).toBeGreaterThan(0);
    });

    // The whole table, through the decode a session response goes through. A
    // pattern this client cannot parse would be an address a person cannot
    // reach, and it would arrive at run time on a deploy rather than at
    // generation.
    it("decodes every usable route out of a session table", () => {
        for (const route of kUsable) {
            const target = kView.routes.get(route.id);
            expect(target, route.id).toBeDefined();
            expect(target?.method, route.id).toBe(route.method);
            expect(target?.path, route.id).toBe(route.path);
        }
    });

    // `ANY` is kept out of `routes` and in `unusable`, so nothing can reach a
    // target there is no method for. The reference application describes
    // `auth.logout` that way, which means it has a logout route no client can
    // call — recorded here rather than worked around, and the row is in
    // `docs/15-tasks.md` §Cross-repo.
    it("keeps a route described with no usable method out of reach", async () => {
        // The session is what a resolve reads, and it is handed over as the
        // client hands it over: a source with a current view and a refetch that
        // cannot invent one.
        const source = {
            current: () => kView,
            refetch: async () => kView,
        };

        for (const route of kUnusable) {
            expect(kView.routes.has(route.id), route.id).toBe(false);
            expect(kView.unusable.has(route.id), route.id).toBe(true);

            const resolved = await resolveRoute(
                // The shape the generated module emits for a holder route: an
                // id and a visibility, and no address at all.
                { id: route.id, visibility: route.visibility, method: null, path: null },
                source,
                new AbortController().signal,
            );
            expect(resolved.ok, route.id).toBe(false);
            if (!resolved.ok) {
                // Uncallable, which is a different fact from absent: retrying,
                // refetching and hiding are three responses and only one is
                // right.
                expect(resolved.error, route.id).toEqual({
                    kind: "client",
                    cause: "unusable-route",
                    retryAfterMs: null,
                });
            }
        }
    });

    it("builds every route, with every parameter encoded", () => {
        for (const route of kUsable) {
            const target = kView.routes.get(route.id);
            if (target === undefined) {
                continue;
            }

            const built = buildRoute(target, { params: parametersFor(route.path), query: {} });
            expect(built.ok, route.id).toBe(true);
            if (!built.ok) {
                continue;
            }

            // Nothing left unsubstituted, and nothing the pattern named still
            // spelled as a parameter.
            expect(built.value.path, route.id).not.toContain("{");
            expect(built.value.path, route.id).not.toContain("}");

            // The hostile value is present exactly as its encoding and never as
            // itself: a path built by concatenation would carry the slash, and a
            // slash in a segment is a different route.
            if (route.path.includes("{")) {
                expect(built.value.path, route.id).toContain("a%2Fb%20c%3Fd%23e%20%D8%AA");
                expect(built.value.path, route.id).not.toContain(kHostile);
            }

            expect(built.value.method, route.id).toBe(route.method);
        }
    });

    // A paged route's query, built by the thing that encodes it. The names are
    // anvil's vocabulary and the application supplies them; what is asserted
    // here is that the cursor and the limit survive the encoder intact and that
    // nothing put an offset in.
    it("builds a paged route's query with a cursor and no offset", () => {
        const paged = kUsable.find((route) => route.page !== null);
        expect(paged, "the descriptor carries a paged route").toBeDefined();
        if (paged === undefined) {
            return;
        }

        const target = kView.routes.get(paged.id);
        expect(target).toBeDefined();
        if (target === undefined) {
            return;
        }

        const built = buildRoute(target, {
            params: parametersFor(paged.path),
            query: { limit: 100, after: "cursor value" },
        });
        expect(built.ok).toBe(true);
        if (!built.ok) {
            return;
        }

        const address = requestPath(built.value);
        expect(address).toContain("limit=100");
        expect(address).toContain("after=cursor%20value");
        expect(address).not.toMatch(/[?&](offset|skip|page)=/);
    });

    // The other half of §4.1: a public route's address is a compile-time value
    // and a holder route's is not. Asserted against the descriptor rather than
    // against the generator's output, because this is the claim a consumer
    // cares about — the privileged paths are not in the bundle.
    it("emits an address only for the routes anvil calls public", () => {
        const publics = kRoutes.filter((route) => route.visibility === "public");
        const holders = kRoutes.filter((route) => route.visibility === "holder");
        expect(publics.length).toBeGreaterThan(0);
        expect(holders.length).toBeGreaterThan(0);

        const generated = readFileSync(
            new URL("../testapp/api/hammer.generated.ts", import.meta.url),
            "utf8",
        );
        for (const route of publics) {
            expect(generated, route.id).toContain(`"${route.path}"`);
        }
        for (const route of holders) {
            expect(generated, route.id).not.toContain(`"${route.path}"`);
        }
    });
});
