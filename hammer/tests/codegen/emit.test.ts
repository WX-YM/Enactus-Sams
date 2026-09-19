// What the generator emits, asserted against the module it actually wrote.
//
// The committed client under tests/testapp/api/ was produced by a previous run
// of the CLI, in a different process, on whatever machine ran it. Comparing this
// run's output against those bytes is therefore the determinism assertion as
// well as the staleness one: a generator whose output depends on key order
// produces a diff on every run and is ignored inside a week.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { transformSync } from "esbuild";
import { describe, expect, it } from "vitest";

import type { Descriptor } from "../../src/codegen/descriptor.js";
import { readDescriptorJson } from "../../src/codegen/descriptor.js";
import { emitClient, kOutputFileName } from "../../src/codegen/emit.js";
import {
    eventIdentifier,
    sectionIdentifier,
    topicIdentifier,
} from "../../src/codegen/names.js";

const kDescriptorPath = fileURLToPath(new URL("../testapp/hammer.descriptor.json", import.meta.url));
const kClientPath = fileURLToPath(new URL(`../testapp/api/${kOutputFileName}`, import.meta.url));

const kText = readFileSync(kDescriptorPath, "utf8");

function reference(): Descriptor {
    const reading = readDescriptorJson(kText);
    if (!reading.ok) {
        throw new Error("the reference descriptor does not read");
    }
    return reading.value.descriptor;
}

const kDescriptor = reference();
const kEmitted = emitClient(kDescriptor);

// The members of `kApiTables`, without the comment above it: the block is
// asserted for what it does NOT contain as well as what it does, and prose
// about routes sits directly above a table that must name none.
function apiTablesBody(): string {
    const found = /export const kApiTables = \{\n([\s\S]*?)\n\} as const;/.exec(kEmitted);
    if (found?.[1] === undefined) {
        throw new Error("the generated module has no kApiTables");
    }
    return found[1];
}

describe("the committed client", () => {
    it("is what this generator emits", () => {
        expect(kEmitted).toBe(readFileSync(kClientPath, "utf8"));
    });

    it("is the same bytes on a second run", () => {
        expect(emitClient(reference())).toBe(kEmitted);
    });

    it("carries the hash of the tables it came from, and nothing above them", () => {
        expect(kEmitted).toContain(`export const kTablesHash = "${kDescriptor.hash}";`);
        expect(kEmitted).not.toContain(kDescriptor.app.version);
        expect(kEmitted).not.toContain(kDescriptor.emitted_by);
    });
});

describe("the visibility split", () => {
    // Quoted, because that is how a path would be emitted. An unquoted match
    // would fire on prose about paths, and a check that reports its own
    // documentation is a check somebody turns off.
    const holder = kDescriptor.tables.routes.filter((route) => route.visibility !== "public");
    const open = kDescriptor.tables.routes.filter((route) => route.visibility === "public");

    it("emits no holder route's path", () => {
        expect(holder.length).toBeGreaterThan(0);
        for (const route of holder) {
            expect(kEmitted).not.toContain(`"${route.path}"`);
        }
    });

    it("emits no holder route's method", () => {
        for (const route of holder) {
            expect(kEmitted).toContain(`id: "${route.id}",\n    access:`);
        }
        expect(kEmitted).toContain("    method: null,");
    });

    it("emits every public route's method and path", () => {
        expect(open.length).toBeGreaterThan(0);
        for (const route of open) {
            expect(kEmitted).toContain(`path: "${route.path}",`);
            expect(kEmitted).toContain(`method: "${route.method}",`);
        }
    });

    it("emits every route's id and shape, whatever its visibility", () => {
        for (const route of kDescriptor.tables.routes) {
            expect(kEmitted).toContain(`id: "${route.id}",`);
            expect(kEmitted).toContain(`readonly "${route.id}":`);
        }
    });
});

describe("the unions", () => {
    it("leaves the success member out of the failure unions", () => {
        // An application would otherwise owe a sentence for a case no error
        // surface can reach.
        const codes = kEmitted.slice(kEmitted.indexOf("export type ErrorCode ="));
        expect(codes.slice(0, codes.indexOf(";"))).not.toContain('"OK"');
        expect(codes.slice(0, codes.indexOf(";"))).toContain('"Unknown"');
        expect(kEmitted).toContain('export const kErrorCodeOk = "OK";');
        expect(kEmitted).toContain("    OK: 0,");
    });

    it("gives a validation reason the same unknown member a code has", () => {
        // Both vocabularies are append-only server-side, so both can answer an
        // open tab with a member its bundle predates. Dropping the field from
        // the map instead is a form that refuses to submit with nothing marked
        // on it.
        const reasons = kEmitted.slice(kEmitted.indexOf("export type ValidationReason ="));
        expect(reasons.slice(0, reasons.indexOf(";"))).not.toContain('"OK"');
        expect(reasons.slice(0, reasons.indexOf(";"))).toContain('"Unknown"');
        // The member is hammer's and never anvil's: it is in the union and not
        // in the value map, because the server stores no number for it.
        const values = kEmitted.slice(kEmitted.indexOf("export const kValidationReasonValues"));
        expect(values.slice(0, values.indexOf("} as const;"))).not.toContain("Unknown");
    });

    it("states the bound the decode can rely on", () => {
        expect(kEmitted).toContain(
            `export const kErrorCodeMaxValue = ${kDescriptor.tables.error_codes.max};`,
        );
    });

    it("emits one const per permission bit", () => {
        for (const permission of kDescriptor.tables.permissions) {
            expect(kEmitted).toContain(`= ${permission.bit};`);
        }
    });

    it("keeps the one table of names out of kApiTables", () => {
        // This case used to assert that `kPermissionBits` did not exist at all,
        // and the reason it had to change is anvil's: a holder's permissions
        // arrive on the session response as NAMES in bit order, and nothing can
        // map a name to a bit without the map.
        //
        // What survives intact is the property the original was protecting. The
        // table is its own `const`, so a chunk that decodes no session
        // references it and ships none of these names — and it is NOT a member
        // of `kApiTables`, which every bundle that constructs a client carries.
        // `built_output.test.ts` asserts the consequence over the built bundle,
        // which is the form of the claim a bundler cannot silently invalidate.
        expect(kEmitted).toContain("export const kPermissionBits = {");
        for (const permission of kDescriptor.tables.permissions) {
            expect(kEmitted).toContain(`    ${permission.name}: kPerm`);
        }
        expect(apiTablesBody()).not.toContain("kPermissionBits");
    });

    it("binds each one to the name the server uses", () => {
        // Written out rather than derived, because an expectation computed the
        // way the emitter computes it agrees with the emitter's bugs. This one
        // read `kPermContentread` once, and every assertion in place at the time
        // checked the bit rather than the name.
        expect(kEmitted).toContain("export const kPermContentRead = 0;");
        expect(kEmitted).toContain("export const kPermFormPii = 18;");
        expect(kEmitted).toContain("export const kPermSystemAnnounce = 120;");
        expect(kEmitted).toContain("export const routeAuthLogin = {");
        expect(kEmitted).toContain("export const routeContentPreview = {");
    });
});

describe("the visibility split over topics", () => {
    // Quoted, for the reason the route half is: an unquoted match fires on the
    // prose explaining the rule, and a check that reports its own documentation
    // is a check somebody turns off.
    const holder = kDescriptor.tables.topics.filter((topic) => topic.visibility !== "public");
    const open = kDescriptor.tables.topics.filter((topic) => topic.visibility === "public");

    it("emits no holder topic's key as a value, and keeps it as a type", () => {
        // Subscribing is the disclosure the way calling is, so the key is the
        // half withheld — the same rule as a holder route's path, applied to the
        // other table that has one. The union still carries it, and that is the
        // whole shape of the trade: a type is erased, so a call site can SPELL a
        // holder topic and no bundle can hold one.
        expect(holder.length).toBeGreaterThan(0);
        const union = kEmitted.slice(kEmitted.indexOf("export type HolderTopicKey ="));
        for (const topic of holder) {
            expect(kEmitted).not.toContain(`key: "${topic.key}",`);
            expect(union.slice(0, union.indexOf(";"))).toContain(`"${topic.key}"`);
        }
        expect(kEmitted).toContain("    key: null,");
    });

    it("emits every public topic's key", () => {
        expect(open.length).toBeGreaterThan(0);
        for (const topic of open) {
            expect(kEmitted).toContain(`key: "${topic.key}",`);
        }
    });

    it("emits every topic's const and code, whatever its visibility", () => {
        // The code is the identity a holder topic is matched on, since its key is
        // not there: a preferences entry the server sends carries both, and the
        // client re-attaches it by the half it has.
        for (const topic of kDescriptor.tables.topics) {
            expect(kEmitted).toContain(`export const ${topicIdentifier(topic.key)} = {`);
            expect(kEmitted).toContain(`    code: ${topic.code},`);
        }
    });

    it("aggregates the public tier and nothing else", () => {
        // A holder topic has no key to put in a table, and an aggregate that had
        // one would put every gated topic's key into any bundle that touched a
        // single one.
        const table = kEmitted.slice(kEmitted.indexOf("export const publicTopics = {"));
        const body = table.slice(0, table.indexOf("} as const;"));
        for (const topic of open) {
            expect(body).toContain(`"${topic.key}":`);
        }
        for (const topic of holder) {
            expect(body).not.toContain(topic.key);
        }
        expect(kEmitted).not.toContain("export const holderTopics");
    });

    it("emits a topic's permissions as bits and never as names", () => {
        for (const topic of kDescriptor.tables.topics) {
            for (const perm of topic.perms) {
                expect(kEmitted).toContain(`perms: [kPerm`);
                expect(kEmitted).not.toContain(`perms: ["${perm}"]`);
            }
        }
    });
});

describe("the two names a client is built from", () => {
    it("assembles the type parameter from the generated names", () => {
        // The four members were written by hand in every application until the
        // first one copied a stale version of them out of a guide. They are
        // derived here, from names this same file emits, so the type cannot
        // describe a table the module does not have.
        expect(kEmitted).toContain("export type Api = {");
        expect(kEmitted).toContain("readonly params: RouteParams;");
        expect(kEmitted).toContain(
            "readonly responses: { readonly [Id in RouteId]: ResponseOf<Id> };",
        );
        expect(kEmitted).toContain("readonly code: ErrorCode;");
        expect(kEmitted).toContain("readonly reason: ValidationReason;");
    });

    it("hands the decode tables over as one value", () => {
        expect(kEmitted).toContain("export const kApiTables = {");
        for (const member of [
            "codes: kErrorCodeValues,",
            "reasons: kValidationReasonValues,",
            "rateLimits: kRateLimits,",
            "singleUse: kCapabilitySingleUse,",
            "bodyMaxBytes: kBodyMaxBytes,",
            "hash: kTablesHash,",
        ]) {
            expect(kEmitted).toContain(member);
        }
    });

    it("pairs the two by a hash both halves carry", () => {
        // What stops one application's tables being read under another's unions.
        // The type demands a LITERAL — `typeof kTablesHash`, not `string` — so
        // the tables satisfy it only when they came from the same descriptor.
        //
        // A brand would have been the obvious spelling and cannot be used here:
        // a `unique symbol` has to be imported to be named, the generated module
        // imports nothing, and a phantom that has to be optional to compile
        // constrains no object literal at all. The hash is the identity the
        // descriptor already publishes, and a generator can write it down.
        expect(kEmitted).toContain("readonly hash: typeof kTablesHash;");

        const value = /export const kTablesHash = "([0-9a-f]{64})";/.exec(kEmitted);
        expect(value?.[1]).toBe(kDescriptor.hash);
    });

    it("names every constant it references", () => {
        // A member pointing at a name this file does not emit is a module that
        // does not compile, and it would only be discovered in an application.
        for (const line of apiTablesBody().split("\n")) {
            const referenced = /^\s*\w+: (\w+),$/.exec(line);
            expect(referenced).not.toBeNull();
            expect(kEmitted).toContain(`export const ${referenced?.[1] ?? ""} `);
        }
    });

    it("leaves the refresh route to the application", () => {
        // The descriptor carries the route and nothing that marks it as the
        // refresh, so a generator that put one here would be guessing at an id —
        // and guessing wrong is silent, in the one call that decides whether a
        // session survives.
        expect(apiTablesBody()).not.toContain("route");
    });
});

describe("the content tables", () => {
    it("derives every vocabulary from the table rather than from a list here", () => {
        // The point of the assertion is the SECOND half: a union that is exactly
        // what the descriptor holds is a union that grows the day anvil appends
        // a member, and one written down on this side is a second copy of an
        // enum — which is the thing the whole seam exists to remove.
        const kinds = new Set(
            kDescriptor.tables.field_types
                .map((type) => type.answer)
                .filter((answer): answer is string => answer !== null),
        );
        const emitted = kEmitted.slice(kEmitted.indexOf("export type AnswerKind ="));
        const union = emitted.slice(0, emitted.indexOf(";"));
        for (const kind of kinds) {
            expect(union).toContain(`"${kind}"`);
        }
        expect(union.split("|").length - 1).toBe(kinds.size);
    });

    it("gives a PII field type a null answer and not a shape", () => {
        // `"text"` would be the shape of a value that has silently emptied
        // itself: a PII answer is never echoed back, so a renderer told it was
        // text renders a control for a value it can never read.
        const pii = kDescriptor.tables.field_types.filter((type) => type.pii);
        expect(pii.length).toBeGreaterThan(0);
        for (const type of pii) {
            const at = kEmitted.indexOf(`${type.name}: {`);
            expect(at).toBeGreaterThan(0);
            expect(kEmitted.slice(at, at + 200)).toContain("answer: null,");
        }
    });

    it("names the unit on every bound it emits", () => {
        // A bound whose unit lives in a comment is a bound that gets read as the
        // other one, and here the two are code points and CSS pixels.
        expect(kEmitted).toContain("defaultCodePoints: 200,");
        expect(kEmitted).toContain("maxCodePoints: 80,");
        expect(kEmitted).toContain("minWidth: 1920,");
        expect(kEmitted).not.toContain("defaultLength");
    });

    it("emits a section's labels in the locale table's order", () => {
        // Positional, because anvil stores a locale as a one-byte index into
        // that table. A client that keyed them by tag would have to know which
        // index means what, which is the renumbering this order exists to
        // refuse.
        const section = kDescriptor.tables.sections.find((entry) => entry.key === "home.hero");
        const field = section?.fields[0];
        expect(field).toBeDefined();
        if (field === undefined) return;
        expect(field.labels).toHaveLength(kDescriptor.tables.locales.length);
        expect(kEmitted).toContain(
            `labels: [${field.labels.map((label) => JSON.stringify(label)).join(", ")}],`,
        );
    });

    it("emits one const per section and per event, and one table per lookup", () => {
        // The split is not a style: a row a call site SPELLS is a const, so a
        // screen that edits one section ships one section's words. A row that
        // arrives at run time — a field type by its stored code, a width by the
        // namespace an object was stored in — has no useful subset, so it is a
        // table and the consumer is told it is paying for all of it.
        for (const section of kDescriptor.tables.sections) {
            expect(kEmitted).toContain(`export const ${sectionIdentifier(section.key)} = {`);
        }
        for (const event of kDescriptor.tables.events) {
            expect(kEmitted).toContain(`export const ${eventIdentifier(event.name)} = {`);
        }
        expect(kEmitted).toContain("export const kFieldTypes = {");
        expect(kEmitted).toContain("export const kMediaWidths = {");
        // And no aggregate of events: an event is always named at its call site,
        // so a table of them would put every name into any bundle that reported
        // one.
        expect(kEmitted).not.toContain("export const events = {");
    });

    it("publishes a width beside its role and no ladder to build a path from", () => {
        // Knowing a width was never the hazard; building a URL out of one was.
        // There is no extension and no rule back from a width to an address
        // anywhere in the emitted module.
        expect(kEmitted).toContain('export const kMediaDefaultRole = "card";');
        expect(kEmitted).toContain("content: { thumb: 320, card: 1024, hero: 1600, full: 2560 },");
        expect(kEmitted).not.toContain("w640");
        expect(kEmitted).not.toContain(".webp");
        expect(kEmitted).not.toContain(".avif");
    });

    it("publishes the accept list as media types and not as extensions", () => {
        // This case is the other half of the one above, and it exists because
        // that one asserted the absence of the string `avif` — which was true
        // while a namespace's accepted TYPES were something an application wrote
        // down itself, and stopped being true the moment anvil emitted them.
        //
        // The distinction the original was reaching for survives: a media type
        // is what a `File.type` comparison and an `accept` attribute are made
        // of, and an EXTENSION is a step on the ladder from a width back to an
        // address. The first is published; the second still is not.
        const namespace = kDescriptor.tables.media.namespaces[0];
        expect(namespace).toBeDefined();
        if (namespace === undefined) return;
        expect(namespace.accepts.length).toBeGreaterThan(0);
        expect(kEmitted).toContain(
            `${namespace.ns}: [${namespace.accepts.map((type) => JSON.stringify(type)).join(", ")}],`,
        );
        for (const type of namespace.accepts) {
            expect(type).toMatch(/^[a-z0-9][a-z0-9!#$&^_.+-]*\/[a-z0-9][a-z0-9!#$&^_.+-]*$/);
        }
    });

    it("emits a dimension's values as the closed set the server indexes", () => {
        const event = kDescriptor.tables.events[0];
        expect(event).toBeDefined();
        if (event === undefined) return;
        const dimension = event.dimensions[0];
        expect(dimension).toBeDefined();
        if (dimension === undefined) return;
        expect(kEmitted).toContain(
            `${dimension.name}: [${dimension.values.map((value) => JSON.stringify(value)).join(", ")}],`,
        );
    });
});

describe("what a route answers with", () => {
    // The emitted types are a translation of a table anvil's own `static_assert`s
    // hold the server to, so what can go wrong on this side is the translation:
    // a nullable field emitted as though it were always present is the exact
    // failure the flag exists to prevent, one layer down.
    function emittedFor(response: unknown): string {
        const mutated: unknown = structuredClone(kDescriptor);
        const tables = (mutated as { tables: { routes: { id: string; response: unknown }[] } })
            .tables;
        const route = tables.routes.find((candidate) => candidate.id === "identity.me");
        if (route === undefined) {
            throw new Error("the reference descriptor has no identity.me");
        }
        route.response = response;
        return emitClient(mutated as Descriptor);
    }

    it("emits the shape anvil declares, one field per line", () => {
        expect(kEmitted).toContain("export interface RouteResponses {");
        expect(kEmitted).toContain('    "identity.me": {');
        expect(kEmitted).toContain("        readonly id: UuidText;");
        expect(kEmitted).toContain("        readonly locale: string;");
    });

    it("emits a nullable field as a different type", () => {
        // `null` and `[]` mean opposite things on this field: a superadmin's
        // permission set is deliberately not all-ones, so the empty list would
        // read as "holds nothing". A consumer told the field is always present
        // crashes on the field it was told it could trust.
        expect(kEmitted).toContain("        readonly permissions: readonly string[] | null;");
    });

    it("names what turns a uuid and a time into a value rather than branding them", () => {
        // Plain aliases, because what a call site holds is the output of
        // `JSON.parse` and a brand asserts work nobody has done. The alias
        // carries the provenance and points at the constructor.
        expect(kEmitted).toContain("export type UuidText = string;");
        expect(kEmitted).toContain("export type ServerTimeText = string;");
        expect(kEmitted).toContain("`Uuid.parse`");
        expect(kEmitted).toContain("`ServerInstant.fromServerIso`");
    });

    it("emits a list of them as a readonly array", () => {
        const emitted = emittedFor({
            kind: "array",
            fields: [{ name: "at", type: "time", nullable: false }],
        });
        expect(emitted).toContain('    "identity.me": readonly {');
        expect(emitted).toContain("        readonly at: ServerTimeText;");
        expect(emitted).toContain("    }[];");
    });

    it("emits every field type as the type a decoded body has", () => {
        const emitted = emittedFor({
            kind: "object",
            fields: [
                { name: "a", type: "string", nullable: false },
                { name: "b", type: "int", nullable: false },
                { name: "c", type: "bool", nullable: false },
                { name: "d", type: "uuid", nullable: false },
                { name: "e", type: "time", nullable: false },
                { name: "f", type: "strings", nullable: false },
            ],
        });
        expect(emitted).toContain("        readonly a: string;");
        expect(emitted).toContain("        readonly b: number;");
        expect(emitted).toContain("        readonly c: boolean;");
        expect(emitted).toContain("        readonly d: UuidText;");
        expect(emitted).toContain("        readonly e: ServerTimeText;");
        expect(emitted).toContain("        readonly f: readonly string[];");
    });

    it("leaves a route anvil declared nothing for out, so it resolves to unknown", () => {
        // The binder is opt-in per handler, so most routes emit no type — and
        // module augmentation stays the way an application declares one.
        // Against the interface BODY and not the whole file: the block comment
        // above it shows an augmentation, and it uses `content.get` as the
        // example. An assertion that fires on the sentence explaining a seam is
        // one somebody turns off.
        const found = /export interface RouteResponses \{\n([\s\S]*?)\n\}/.exec(kEmitted);
        expect(found?.[1]).toBeDefined();
        expect(found?.[1] ?? "").not.toContain("content.get");
        expect(kEmitted).toContain("export type ResponseOf<Id extends RouteId> = Id extends keyof RouteResponses");
        expect(kEmitted).toContain("    : unknown;");
    });

    it("emits an empty interface when nothing is declared at all", () => {
        // A descriptor from an application that uses no binder, which is what
        // every one of them looked like before format 3. The augmentation path
        // has to keep working, so the interface is still there and still empty.
        const mutated: unknown = structuredClone(kDescriptor);
        for (const route of (mutated as { tables: { routes: { response: unknown }[] } }).tables
            .routes) {
            route.response = null;
        }
        const emitted = emitClient(mutated as Descriptor);
        expect(emitted).toContain("export interface RouteResponses {}");
        expect(emitted).not.toContain("export type UuidText");
    });
});

describe("the module a bundler sees", () => {
    it("imports nothing and runs nothing", () => {
        // Side-effect-free is a promise `"sideEffects": false` makes on this
        // module's behalf. Everything at column zero is a comment, an export, or
        // the close of one — including the bare `}` that ends `RouteResponses`,
        // which is an `interface` and so takes no `as const` and no semicolon.
        for (const line of kEmitted.split("\n")) {
            if (line.length === 0 || line.startsWith(" ")) {
                continue;
            }
            expect(line).toMatch(/^(\/\/|export |\] as const;$|\} as const;$|\};$|\}$)/);
        }
        expect(kEmitted).not.toContain("\nimport ");
        expect(kEmitted).not.toContain("export default");
    });

    it("says it is generated, on the first line", () => {
        expect(kEmitted.split("\n")[0]).toContain("DO NOT EDIT");
    });
});

describe("a string the reader would have refused", () => {
    // readDescriptor refuses a path or a collation holding a quote or a
    // newline, so this descriptor is built by hand to get past it. The point is
    // that the emitter does not RELY on that: every string it writes goes
    // through JSON.stringify, and the day somebody loosens a character class
    // the output is still a module rather than whatever the descriptor said.
    const kHostilePath = '/login";\nexport const leakedByPath = 1;//';
    const kHostileCollation = 'en";\nexport const leakedByCollation = 1;//';

    const login = kDescriptor.tables.routes[0];
    if (login === undefined) {
        throw new Error("the reference descriptor has no routes");
    }

    const hostile: Descriptor = {
        ...kDescriptor,
        tables: {
            ...kDescriptor.tables,
            locales: [{ tag: "en", collation: kHostileCollation, rtl: false }],
            routes: [{ ...login, path: kHostilePath }],
        },
    };
    const text = emitClient(hostile);

    it("is escaped into a string literal rather than emitted as source", () => {
        expect(text).toContain(`path: ${JSON.stringify(kHostilePath)},`);
        expect(text).toContain(`collation: ${JSON.stringify(kHostileCollation)}`);
    });

    it("adds no statement of its own", () => {
        const lines = text.split("\n");
        expect(lines.some((line) => line.startsWith("export const leakedByPath"))).toBe(false);
        expect(lines.some((line) => line.startsWith("export const leakedByCollation"))).toBe(false);
    });

    it("still parses as TypeScript", () => {
        // The proof that the escaping produced a module and not a string that
        // merely looks like one.
        expect(() => transformSync(text, { loader: "ts" })).not.toThrow();
    });
});

describe("a table with nothing in it", () => {
    it("emits never rather than a union of nothing", () => {
        // `never` is the honest type, and it makes every use of it fail to
        // compile rather than accept anything.
        const stripped = JSON.parse(kText) as Record<string, unknown>;
        const tables = stripped["tables"] as Record<string, unknown>;
        tables["permissions"] = [];
        tables["routes"] = [];
        tables["capability_scopes"] = [];
        tables["rate_limits"] = [];
        // Topics go too, because a topic names permissions and there are none
        // left to name: an empty table is the case being tested, not a dangling
        // reference into one.
        tables["topics"] = [];

        const reading = readDescriptorJson(JSON.stringify(stripped));
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;

        const text = emitClient(reading.value.descriptor);
        expect(text).toContain("export type Permission = never;");
        expect(text).toContain("export type RouteId = never;");
        expect(text).toContain("export type TopicKey = never;");
        expect(text).toContain("export const publicRoutes = {} as const;");
        expect(text).toContain("export const publicTopics = {} as const;");
    });
});
