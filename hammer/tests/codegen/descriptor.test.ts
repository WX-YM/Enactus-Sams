// Every case here is driven against the REAL descriptor, mutated one field at a
// time. A hand-written malformed example asserts what we believe a broken
// descriptor looks like; a mutation of anvil's own output asserts what one
// actually looks like, and it keeps working when the format grows a key.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "../support/test.js";

import type { DescriptorProblem } from "../../src/codegen/descriptor.js";
import { readDescriptor, readDescriptorJson } from "../../src/codegen/descriptor.js";

type Mutable = Record<string, unknown>;

const kDescriptorPath = fileURLToPath(new URL("../testapp/hammer.descriptor.json", import.meta.url));
const kText = readFileSync(kDescriptorPath, "utf8");
const kFixture: unknown = JSON.parse(kText);

function clone(): Mutable {
    return structuredClone(kFixture) as Mutable;
}

function tables(descriptor: Mutable): Mutable {
    return descriptor["tables"] as Mutable;
}

function list(descriptor: Mutable, name: string): Mutable[] {
    return tables(descriptor)[name] as Mutable[];
}

function entry(descriptor: Mutable, name: string, index: number): Mutable {
    const found = list(descriptor, name)[index];
    if (found === undefined) {
        throw new Error(`${name}[${index}] is not in the reference descriptor`);
    }
    return found;
}

function namespace(descriptor: Mutable, at: number): Mutable {
    const namespaces = (tables(descriptor)["media"] as Mutable)["namespaces"] as Mutable[];
    const found = namespaces[at];
    if (found === undefined) {
        throw new Error(`the reference descriptor has no media namespace ${at}`);
    }
    return found;
}

function accountsTable(descriptor: Mutable): Mutable {
    const accounts = tables(descriptor)["accounts"];
    if (accounts === null || accounts === undefined) {
        throw new Error("the reference descriptor has no accounts table");
    }
    return accounts as Mutable;
}

function route(descriptor: Mutable, id: string): Mutable {
    const found = list(descriptor, "routes").find((candidate) => candidate["id"] === id);
    if (found === undefined) {
        throw new Error(`${id} is not in the reference descriptor`);
    }
    return found;
}

// The key a problem is reported against, derived from the route's position in
// the table rather than written down beside the expectation.
//
// The indices were hand-kept until anvil declared two routes in the middle of
// the table, at which point nine assertions failed for a reason none of them
// was about. The key is still asserted exactly — it is what a person opens the
// file at — and it is now the one the route actually has.
function routeKey(descriptor: Mutable, id: string, field: string): string {
    const at = list(descriptor, "routes").findIndex((candidate) => candidate["id"] === id);
    if (at < 0) {
        throw new Error(`${id} is not in the reference descriptor`);
    }
    return `tables.routes[${at}].${field}`;
}

function problems(descriptor: Mutable): readonly DescriptorProblem[] {
    const reading = readDescriptor(descriptor);
    expect(reading.ok).toBe(false);
    return reading.ok ? [] : reading.error;
}

describe("the reference descriptor", () => {
    it("reads", () => {
        const reading = readDescriptorJson(kText);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        expect(reading.value.descriptor.tables.routes).toHaveLength(23);
        expect(reading.value.descriptor.tables.permissions).toHaveLength(11);
    });

    it("reports a dead scope and a dead bucket without failing", () => {
        // anvil limits by keys that are not route buckets and mints scopes for
        // operations whose routes its emitter does not carry, so this is what
        // its own output looks like. Failing here would reject it; saying
        // nothing would let a table of entries nothing references read as
        // coverage.
        const reading = readDescriptorJson(kText);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;

        const issues = reading.value.warnings.map((warning) => warning.issue);
        expect(issues).toContain("no-route-requires-it");
        expect(issues).toContain("no-route-names-it");
        expect(reading.value.warnings.map((warning) => warning.key)).toContain(
            "tables.capability_scopes[2].name",
        );
    });

    it("is not JSON when it is not JSON", () => {
        expect(readDescriptorJson("{").ok).toBe(false);
        expect(readDescriptorJson("{")).toMatchObject({
            ok: false,
            error: [{ key: "", issue: "not-json" }],
        });
    });
});

describe("the checks anvil's well_formed() makes", () => {
    it("refuses a duplicate permission bit", () => {
        const descriptor = clone();
        entry(descriptor, "permissions", 1)["bit"] = 0;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.permissions[1].bit",
            issue: "duplicate-bit",
        });
    });

    it("refuses a duplicate permission name", () => {
        const descriptor = clone();
        entry(descriptor, "permissions", 1)["name"] = "ContentRead";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.permissions[1].name",
            issue: "duplicate-name",
        });
    });

    it("refuses an empty name", () => {
        const descriptor = clone();
        entry(descriptor, "permissions", 3)["name"] = "";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.permissions[3].name",
            issue: "empty",
        });
    });

    it("refuses a bit above the hundred and twenty-eight anvil stores", () => {
        const descriptor = clone();
        entry(descriptor, "permissions", 10)["bit"] = 128;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.permissions[10].bit",
            issue: "out-of-range",
        });
    });

    it("refuses an error code value above the bound the table states", () => {
        const descriptor = clone();
        (tables(descriptor)["error_codes"] as Mutable)["max"] = 12;
        const keys = problems(descriptor).map((problem) => problem.key);
        expect(keys).toContain("tables.error_codes.codes[13].value");
    });

    it("refuses a duplicate route id", () => {
        const descriptor = clone();
        const key = routeKey(descriptor, "identity.me", "id");
        route(descriptor, "identity.me")["id"] = "auth.login";
        expect(problems(descriptor)).toContainEqual({ key, issue: "duplicate-name" });
    });
});

describe("a route that names something that is not there", () => {
    it("refuses a permission no table declares", () => {
        const descriptor = clone();
        route(descriptor, "content.get")["perms"] = ["ContentReed"];
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "content.get", "perms[0]"),
            issue: "unknown-permission",
        });
    });

    // The reverse of the warning, and the one that matters: the emitted type
    // would require a Capability of a scope with no minting call, so nothing
    // downstream of it would compile.
    it("refuses a capability scope no table declares", () => {
        const descriptor = clone();
        route(descriptor, "content.delete")["capability"] = "ContentPublish";
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "content.delete", "capability"),
            issue: "unknown-capability-scope",
        });
    });

    it("refuses a rate-limit bucket no table declares", () => {
        const descriptor = clone();
        route(descriptor, "auth.login")["rate_limit"] = "signin";
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "auth.login", "rate_limit"),
            issue: "unknown-rate-limit-bucket",
        });
    });

    it("refuses a page limit above the global maximum", () => {
        const descriptor = clone();
        (route(descriptor, "media.list")["page"] as Mutable)["limit_max"] = 500;
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "media.list", "page.limit_max"),
            issue: "above-maximum",
        });
    });

    it("refuses a method it cannot emit", () => {
        const descriptor = clone();
        route(descriptor, "auth.login")["method"] = "SUBSCRIBE";
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "auth.login", "method"),
            issue: "unknown-member",
        });
    });
});

describe("a method that is not a method", () => {
    // `ANY` is a declaration that every method shares one policy. anvil emits it
    // so a client fails loudly on a description that forgot to name one, rather
    // than defaulting to GET and silently calling the wrong thing.
    it("fails generation for a public route, whose method is emitted as a value", () => {
        const descriptor = clone();
        route(descriptor, "content.preview")["method"] = "ANY";
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "content.preview", "method"),
            issue: "unusable-method",
        });
    });

    it("warns for a holder route, whose method is not emitted at all", () => {
        // It cannot fail here: the method arrives with the session, so the
        // failure is a run-time one and this is the only place the build can say
        // it is coming.
        //
        // Driven against a MUTATION rather than against the fixture. anvil's
        // reference application described `auth.logout` with `ANY` until format
        // 3 — hammer refused it, which is what the row asking for a method on
        // every route description was about — and this case read the warning off
        // the real descriptor. It now describes POST, so there is no `ANY` route
        // left to read: a case that asserts the absence of a defect the other
        // repository fixed is a case that deletes itself, and what has to keep
        // being true is that the warning still fires.
        const descriptor = clone();
        route(descriptor, "auth.logout")["method"] = "ANY";
        const reading = readDescriptor(descriptor);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        expect(reading.value.warnings).toContainEqual({
            key: routeKey(descriptor, "auth.logout", "method"),
            issue: "unusable-method",
        });
    });
});

describe("a path the emitter cannot turn into parameters", () => {
    it("refuses a path that is not absolute", () => {
        const descriptor = clone();
        route(descriptor, "auth.login")["path"] = "login";
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "auth.login", "path"),
            issue: "path-not-absolute",
        });
    });

    it("refuses an unclosed parameter", () => {
        const descriptor = clone();
        route(descriptor, "content.get")["path"] = "/content/{id";
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "content.get", "path"),
            issue: "malformed-path-parameter",
        });
    });

    it("refuses the same parameter twice", () => {
        const descriptor = clone();
        route(descriptor, "media.list")["path"] = "/media/{id}/{id}";
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "media.list", "path"),
            issue: "duplicate-path-parameter",
        });
    });
});

describe("a string that would reach the emitted file", () => {
    // Every string the generator writes goes through JSON.stringify, so an
    // emitted module cannot be broken out of. These are the checks that run
    // BEFORE that one, so the guarantee does not rest on a single call a later
    // refactor could drop.
    it("refuses a route pattern holding a character a route pattern cannot", () => {
        for (const hostile of ['/content/{id}"', "/content/\n", "/content/\\", "/a b"]) {
            const descriptor = clone();
            route(descriptor, "content.get")["path"] = hostile;
            expect(problems(descriptor).map((problem) => problem.issue)).toContain(
                "path-not-a-route-pattern",
            );
        }
    });

    it("refuses a locale tag or collation that is not one", () => {
        const tagged = clone();
        entry(tagged, "locales", 1)["tag"] = 'ar"; drop';
        expect(problems(tagged)).toContainEqual({
            key: "tables.locales[1].tag",
            issue: "not-spellable",
        });

        const collated = clone();
        entry(collated, "locales", 1)["collation"] = "ar\n*/";
        expect(problems(collated)).toContainEqual({
            key: "tables.locales[1].collation",
            issue: "not-spellable",
        });
    });

    it("refuses a cursor field that is not a field name", () => {
        const descriptor = clone();
        (route(descriptor, "media.list")["page"] as Mutable)["cursor"] = '_id", x: "';
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "media.list", "page.cursor"),
            issue: "not-spellable",
        });
    });
});

describe("a name that cannot be spelled", () => {
    it("refuses a name TypeScript cannot carry", () => {
        const descriptor = clone();
        entry(descriptor, "permissions", 4)["name"] = "Media Delete";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.permissions[4].name",
            issue: "not-spellable",
        });
    });

    // Two names that differ only by separator or case land on one `const`, and
    // the second would silently shadow the first in the emitted module.
    it("refuses two names that land on one identifier", () => {
        const descriptor = clone();
        entry(descriptor, "permissions", 1)["name"] = "CONTENT_READ";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.permissions[1].name",
            issue: "identifier-collision",
        });
    });
});

describe("the content tables", () => {
    it("reads the ones anvil emits", () => {
        const reading = readDescriptorJson(kText);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        const tables = reading.value.descriptor.tables;
        expect(tables.field_types.length).toBeGreaterThan(0);
        expect(tables.sections.length).toBeGreaterThan(0);
        expect(tables.events.length).toBeGreaterThan(0);
        expect(tables.media.namespaces.length).toBeGreaterThan(0);
    });

    it("refuses a PII type that also declares an answer shape", () => {
        // The two say the same thing and a renderer branches on both, so a
        // descriptor where they disagree is one whose form draws a control for a
        // value it can never read back.
        const descriptor = clone();
        const at = list(descriptor, "field_types").findIndex((type) => type["pii"] === true);
        expect(at).toBeGreaterThanOrEqual(0);
        entry(descriptor, "field_types", at)["answer"] = "text";
        expect(problems(descriptor)).toContainEqual({
            key: `tables.field_types[${at}].answer`,
            issue: "unknown-member",
        });
    });

    it("refuses a non-PII type with no answer shape", () => {
        const descriptor = clone();
        entry(descriptor, "field_types", 0)["answer"] = null;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.field_types[0].answer",
            issue: "unknown-member",
        });
    });

    it("refuses a duplicate field type code", () => {
        const descriptor = clone();
        entry(descriptor, "field_types", 1)["code"] = 0;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.field_types[1].code",
            issue: "duplicate-value",
        });
    });

    it("refuses a label list that is not one per declared locale", () => {
        // The array is indexed by the locale table's order, so a short one is an
        // index out of range the moment an editor switches language — which is
        // the defect nobody sees in the locale they develop in.
        const descriptor = clone();
        const section = entry(descriptor, "sections", 0);
        const field = (section["fields"] as Mutable[])[0];
        if (field === undefined) {
            throw new Error("the reference descriptor has no section fields");
        }
        field["labels"] = ["Address"];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.sections[0].fields[0].labels",
            issue: "out-of-range",
        });
    });

    it("refuses a label holding something that is not text", () => {
        // A label is the one descriptor string that cannot have a character
        // class — it is a word in whatever language the application is written
        // in — so what it is held to is the characters that are not text at all.
        // U+2028 is the one that matters: JSON.stringify does not escape it, and
        // a parser older than the JSON-superset rule reads it as a line break
        // inside a string literal.
        for (const hostile of ["Addr\u2028ess", "Addr\u0007ess"]) {
            const descriptor = clone();
            const section = entry(descriptor, "sections", 0);
            const field = (section["fields"] as Mutable[])[0];
            if (field === undefined) {
                throw new Error("the reference descriptor has no section fields");
            }
            field["labels"] = [hostile, hostile];
            expect(problems(descriptor).map((problem) => problem.issue)).toContain("not-plain-text");
        }
    });

    it("refuses a site path that is not a path", () => {
        // It is a value the client navigates to, so it is checked the way a
        // route's path is: `//evil.example` is a host and not a path on this
        // origin.
        const descriptor = clone();
        entry(descriptor, "sections", 0)["site_path"] = "contact";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.sections[0].site_path",
            issue: "path-not-absolute",
        });
    });

    it("refuses two section keys that land on one identifier", () => {
        const descriptor = clone();
        entry(descriptor, "sections", 1)["key"] = "contact_info";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.sections[1].key",
            issue: "identifier-collision",
        });
    });

    it("refuses the same field key twice in one section", () => {
        const descriptor = clone();
        const fields = entry(descriptor, "sections", 0)["fields"] as Mutable[];
        const second = fields[1];
        const first = fields[0];
        if (second === undefined || first === undefined) {
            throw new Error("the reference descriptor has no second section field");
        }
        second["key"] = first["key"];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.sections[0].fields[1].key",
            issue: "duplicate-name",
        });
    });

    it("refuses a topic that calls itself public while requiring a permission", () => {
        // anvil DERIVES visibility from the permission set, so a disagreement is
        // not anvil's output. This direction is the one that matters: the key
        // would be emitted as a value into a bundle that the permission was
        // supposed to gate, which is the failure §4.1 exists to stop.
        const descriptor = clone();
        const at = list(descriptor, "topics").findIndex(
            (topic) => (topic["perms"] as string[]).length !== 0,
        );
        expect(at).toBeGreaterThanOrEqual(0);
        entry(descriptor, "topics", at)["visibility"] = "public";
        expect(problems(descriptor)).toContainEqual({
            key: `tables.topics[${at}].visibility`,
            issue: "visibility-contradicts-perms",
        });
    });

    it("refuses a topic that withholds its key with nothing gating it", () => {
        // The other direction. It leaks nothing, and it is still refused: a key
        // withheld by a gate that is not there reads as a gate that is.
        const descriptor = clone();
        const at = list(descriptor, "topics").findIndex(
            (topic) => (topic["perms"] as string[]).length === 0,
        );
        expect(at).toBeGreaterThanOrEqual(0);
        entry(descriptor, "topics", at)["visibility"] = "holder";
        expect(problems(descriptor)).toContainEqual({
            key: `tables.topics[${at}].visibility`,
            issue: "visibility-contradicts-perms",
        });
    });

    it("refuses a topic naming a permission no table declares", () => {
        const descriptor = clone();
        entry(descriptor, "topics", 1)["perms"] = ["FormReed"];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.topics[1].perms[0]",
            issue: "unknown-permission",
        });
    });

    it("refuses a duplicate topic code", () => {
        // A code is a bit position in every preference mask already written, so a
        // duplicate silently reinterprets them.
        const descriptor = clone();
        entry(descriptor, "topics", 1)["code"] = 0;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.topics[1].code",
            issue: "duplicate-value",
        });
    });

    it("refuses a dimension value declared twice", () => {
        // The row stores the INDEX into the set, so two names for one stored
        // number is two readings of every row already written.
        const descriptor = clone();
        const dimensions = entry(descriptor, "events", 0)["dimensions"] as Mutable[];
        const dimension = dimensions[0];
        if (dimension === undefined) {
            throw new Error("the reference descriptor has no dimensions");
        }
        dimension["values"] = ["web", "web"];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.events[0].dimensions[0].values[1]",
            issue: "duplicate-name",
        });
    });

    it("reads a dimension with no kind at all as an enum", () => {
        // anvil's format did not move: a descriptor from before the
        // distinction existed is not a malformed one, and every dimension it
        // ever emitted was a closed set.
        const descriptor = clone();
        const dimensions = entry(descriptor, "events", 0)["dimensions"] as Mutable[];
        const dimension = dimensions[0];
        if (dimension === undefined) {
            throw new Error("the reference descriptor has no dimensions");
        }
        delete dimension["kind"];

        const reading = readDescriptor(descriptor);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        expect(reading.value.descriptor.tables.events[0]?.dimensions[0]).toMatchObject({
            kind: "enum",
        });
    });

    it("refuses an entity dimension that carries values", () => {
        // The server stores no index for a foreign id, so a value here is a
        // column nothing reads rather than a row the ingest path drops.
        const descriptor = clone();
        const dimensions = entry(descriptor, "events", 3)["dimensions"] as Mutable[];
        const dimension = dimensions[0];
        if (dimension === undefined) {
            throw new Error("the reference descriptor has no entity dimension");
        }
        expect(dimension["kind"]).toBe("entity");
        dimension["values"] = ["some-project"];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.events[3].dimensions[0].values",
            issue: "not-empty",
        });
    });

    it("refuses an enum dimension with no values", () => {
        const descriptor = clone();
        const dimensions = entry(descriptor, "events", 0)["dimensions"] as Mutable[];
        const dimension = dimensions[0];
        if (dimension === undefined) {
            throw new Error("the reference descriptor has no dimensions");
        }
        dimension["values"] = [];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.events[0].dimensions[0].values",
            issue: "empty",
        });
    });

    it("refuses a second entity dimension on one event", () => {
        // The row an event ingests has one column for a foreign id; a second
        // entity dimension is a column the ingest path has nowhere to put.
        const descriptor = clone();
        const dimensions = entry(descriptor, "events", 3)["dimensions"] as Mutable[];
        const dimension = dimensions[0];
        if (dimension === undefined) {
            throw new Error("the reference descriptor has no entity dimension");
        }
        dimensions.push({ ...dimension, name: "organization" });
        expect(problems(descriptor)).toContainEqual({
            key: "tables.events[3].dimensions",
            issue: "duplicate-entity-dimension",
        });
    });

    it("refuses a dimension kind the format does not name", () => {
        const descriptor = clone();
        const dimensions = entry(descriptor, "events", 0)["dimensions"] as Mutable[];
        const dimension = dimensions[0];
        if (dimension === undefined) {
            throw new Error("the reference descriptor has no dimensions");
        }
        dimension["kind"] = "computed";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.events[0].dimensions[0].kind",
            issue: "unknown-member",
        });
    });

    it("refuses a default media role a namespace does not serve", () => {
        // A request with no role segment resolves to it, so a namespace that
        // does not serve it turns forgetting the segment into a 404 the caller
        // finds out about in production.
        const descriptor = clone();
        (tables(descriptor)["media"] as Mutable)["default_role"] = "thumbnail";
        const keys = problems(descriptor).map((problem) => problem.key);
        expect(keys).toContain("tables.media.namespaces[0].roles");
    });

    it("refuses a role width that is not a width", () => {
        const descriptor = clone();
        const namespaces = (tables(descriptor)["media"] as Mutable)["namespaces"] as Mutable[];
        const namespace = namespaces[0];
        if (namespace === undefined) {
            throw new Error("the reference descriptor has no media namespaces");
        }
        const role = (namespace["roles"] as Mutable[])[0];
        if (role === undefined) {
            throw new Error("the reference descriptor has no media roles");
        }
        role["width"] = 0;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.media.namespaces[0].roles[0].width",
            issue: "not-positive",
        });
    });

    it("reads the media types a namespace accepts", () => {
        const reading = readDescriptorJson(kText);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        for (const namespace of reading.value.descriptor.tables.media.namespaces) {
            expect(namespace.accepts.length).toBeGreaterThan(0);
        }
    });

    it("refuses a namespace that accepts nothing", () => {
        // Not a policy — an emitter that forgot. The accept list exists so the
        // client's check and the server's are one list, and an empty one makes
        // the client's refuse every upload.
        const descriptor = clone();
        namespace(descriptor, 0)["accepts"] = [];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.media.namespaces[0].accepts",
            issue: "empty",
        });
    });

    it("refuses a media type that is not one a File.type can equal", () => {
        // A browser puts the lowercase essence string in `File.type`, so a value
        // with a parameter or a capital never equals the thing it is compared
        // against — and the upload is refused for a reason nobody can see.
        for (const spelling of ["image/jpeg; charset=binary", "Image/JPEG", "image", "/jpeg"]) {
            const descriptor = clone();
            namespace(descriptor, 0)["accepts"] = [spelling];
            expect(problems(descriptor)).toContainEqual({
                key: "tables.media.namespaces[0].accepts[0]",
                issue: "not-a-media-type",
            });
        }
    });

    it("refuses one media type named twice", () => {
        const descriptor = clone();
        namespace(descriptor, 0)["accepts"] = ["image/png", "image/png"];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.media.namespaces[0].accepts[1]",
            issue: "duplicate-name",
        });
    });
});

// --- accounts (anvil's built-in registration, verification and sign-in) -----

describe("accounts", () => {
    it("reads the table the reference application declares", () => {
        const reading = readDescriptorJson(kText);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        const accounts = reading.value.descriptor.tables.accounts;
        expect(accounts).not.toBeNull();
        if (accounts === null) return;
        expect(accounts.hashing).toBe("client");
        expect(accounts.contact).toBe("email");
        expect(accounts.routes.sign_in).toBe("auth.login");
        expect(accounts.routes.salt).toBe("auth.prehash");
    });

    it("reads an absent key as null, the way a descriptor from before this table existed does", () => {
        const descriptor = clone();
        delete tables(descriptor)["accounts"];
        const reading = readDescriptor(descriptor);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        expect(reading.value.descriptor.tables.accounts).toBeNull();
    });

    it("reads an explicit null the same way, for an application that declares none", () => {
        const descriptor = clone();
        tables(descriptor)["accounts"] = null;
        const reading = readDescriptor(descriptor);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        expect(reading.value.descriptor.tables.accounts).toBeNull();
    });

    it("refuses a table with no identifier a person can sign in with", () => {
        const descriptor = clone();
        for (const identifier of accountsTable(descriptor)["identifiers"] as Mutable[]) {
            identifier["sign_in"] = false;
        }
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.identifiers",
            issue: "no-sign-in-identifier",
        });
    });

    it("refuses the same identifier kind declared twice", () => {
        const descriptor = clone();
        const identifiers = accountsTable(descriptor)["identifiers"] as Mutable[];
        const second = identifiers[1];
        if (second === undefined) {
            throw new Error("the reference descriptor has no second identifier");
        }
        second["kind"] = "email";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.identifiers[1].kind",
            issue: "duplicate-name",
        });
    });

    it("refuses a contact that does not name a declared, required identifier", () => {
        const descriptor = clone();
        accountsTable(descriptor)["contact"] = "phone";
        // The reference descriptor's phone identifier is optional, so naming it
        // as the contact channel is the failure being tested — a reset flow
        // with no address it can rely on being there.
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.contact",
            issue: "unknown-identifier",
        });
    });

    it("refuses a profile key with a character the format does not allow", () => {
        const descriptor = clone();
        const profile = accountsTable(descriptor)["profile"] as Mutable[];
        const field = profile[0];
        if (field === undefined) {
            throw new Error("the reference descriptor has no profile field");
        }
        field["key"] = "Given Name";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.profile[0].key",
            issue: "not-a-profile-key",
        });
    });

    it("refuses a profile key that is one of hammer's own words", () => {
        const descriptor = clone();
        const profile = accountsTable(descriptor)["profile"] as Mutable[];
        const field = profile[0];
        if (field === undefined) {
            throw new Error("the reference descriptor has no profile field");
        }
        field["key"] = "password";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.profile[0].key",
            issue: "reserved-word",
        });
    });

    it("refuses two profile fields with one key", () => {
        const descriptor = clone();
        const profile = accountsTable(descriptor)["profile"] as Mutable[];
        const second = profile[1];
        const first = profile[0];
        if (second === undefined || first === undefined) {
            throw new Error("the reference descriptor has no second profile field");
        }
        second["key"] = first["key"];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.profile[1].key",
            issue: "duplicate-name",
        });
    });

    it("refuses a profile bound with the minimum above the maximum", () => {
        const descriptor = clone();
        const profile = accountsTable(descriptor)["profile"] as Mutable[];
        const field = profile[0];
        if (field === undefined) {
            throw new Error("the reference descriptor has no profile field");
        }
        field["min_code_points"] = 100;
        field["max_code_points"] = 80;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.profile[0].min_code_points",
            issue: "out-of-range",
        });
    });

    it("refuses a profile maximum that admits nothing", () => {
        const descriptor = clone();
        const profile = accountsTable(descriptor)["profile"] as Mutable[];
        const field = profile[0];
        if (field === undefined) {
            throw new Error("the reference descriptor has no profile field");
        }
        field["max_code_points"] = 0;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.profile[0].max_code_points",
            issue: "not-positive",
        });
    });

    it("refuses a secret bound with the minimum above the maximum", () => {
        const descriptor = clone();
        const secret = accountsTable(descriptor)["secret"] as Mutable;
        secret["min_code_points"] = 200;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.secret.min_code_points",
            issue: "out-of-range",
        });
    });

    it("refuses a secret bound that is not positive", () => {
        const descriptor = clone();
        const secret = accountsTable(descriptor)["secret"] as Mutable;
        secret["max_bytes"] = 0;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.secret.max_bytes",
            issue: "not-positive",
        });
    });

    it("refuses fewer than four code digits", () => {
        const descriptor = clone();
        accountsTable(descriptor)["code_digits"] = 3;
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.code_digits",
            issue: "out-of-range",
        });
    });

    it("refuses a route role naming a route the route table does not declare", () => {
        const descriptor = clone();
        const routes = accountsTable(descriptor)["routes"] as Mutable;
        routes["verify"] = "auth.verify_email";
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.routes.verify",
            issue: "unknown-route",
        });
    });

    it("refuses a table with no sign-in route", () => {
        const descriptor = clone();
        const routes = accountsTable(descriptor)["routes"] as Mutable;
        delete routes["sign_in"];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.routes.sign_in",
            issue: "missing-role",
        });
    });

    it("refuses client hashing with no salt route", () => {
        const descriptor = clone();
        const routes = accountsTable(descriptor)["routes"] as Mutable;
        delete routes["salt"];
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.routes.salt",
            issue: "missing-role",
        });
    });

    it("refuses server hashing with a salt route", () => {
        const descriptor = clone();
        accountsTable(descriptor)["hashing"] = "server";
        // The salt route stays declared — the failure being tested is that
        // server hashing has nothing to prehash against, so a salt route on
        // this table would be a route nothing calls.
        expect(problems(descriptor)).toContainEqual({
            key: "tables.accounts.routes.salt",
            issue: "not-empty",
        });
    });
});

// --- what a route answers with (§4.3) ---------------------------------------
//
// These describe a table anvil only emits for the handlers written through its
// response BINDER, where the schema is enforced by `static_assert` rather than
// stated in a comment. That is the whole reason it is read at all — a type
// nothing enforces is a lie that type-checks — so the checks here are about the
// one thing this side can still get wrong: emitting a type from a schema that is
// itself malformed.

describe("a declared response", () => {
    it("reads the one the reference application declares", () => {
        const reading = readDescriptorJson(kText);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        const described = reading.value.descriptor.tables.routes.filter(
            (candidate) => candidate.response !== null,
        );
        expect(described.length).toBeGreaterThan(0);

        const me = reading.value.descriptor.tables.routes.find(
            (candidate) => candidate.id === "identity.me",
        );
        expect(me?.response?.kind).toBe("object");
        // Nullable, and it is the case the flag exists for: a superadmin's
        // permission set is deliberately not all-ones, so `null` and `[]` mean
        // opposite things on this field.
        expect(me?.response?.fields).toContainEqual({
            name: "permissions",
            type: "strings",
            nullable: true,
        });
    });

    it("reads a list of them", () => {
        const descriptor = clone();
        route(descriptor, "audit.list")["response"] = {
            kind: "array",
            fields: [{ name: "at", type: "time", nullable: false }],
        };
        const reading = readDescriptor(descriptor);
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        const audit = reading.value.descriptor.tables.routes.find(
            (candidate) => candidate.id === "audit.list",
        );
        expect(audit?.response?.kind).toBe("array");
    });

    it("refuses a field type this generator has no type for", () => {
        // A seventh kind anvil appends is one route hammer cannot type yet, and
        // it says which. Emitting `unknown` for it would be a client that
        // compiles against a shape nobody checked.
        const descriptor = clone();
        route(descriptor, "identity.me")["response"] = {
            kind: "object",
            fields: [{ name: "id", type: "decimal", nullable: false }],
        };
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "identity.me", "response.fields[0].type"),
            issue: "unknown-member",
        });
    });

    it("refuses a kind that is neither an object nor a list of them", () => {
        const descriptor = clone();
        route(descriptor, "identity.me")["response"] = { kind: "map", fields: [] };
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "identity.me", "response.kind"),
            issue: "unknown-member",
        });
    });

    it("refuses a declared response with no field in it", () => {
        // anvil writes `null` for "no declaration", so this cannot be that: it
        // is a declaration somebody started and did not finish.
        const descriptor = clone();
        route(descriptor, "identity.me")["response"] = { kind: "object", fields: [] };
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "identity.me", "response.fields"),
            issue: "empty",
        });
    });

    it("refuses two fields with one name", () => {
        // The second value overwrites the first in every JSON parser there is,
        // and the emitted type would not compile — so it fails here, naming the
        // field, rather than in the consumer's build naming a generated line.
        const descriptor = clone();
        route(descriptor, "identity.me")["response"] = {
            kind: "object",
            fields: [
                { name: "id", type: "uuid", nullable: false },
                { name: "id", type: "string", nullable: false },
            ],
        };
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "identity.me", "response.fields[1].name"),
            issue: "duplicate-field",
        });
    });

    it("refuses a field with no name", () => {
        const descriptor = clone();
        route(descriptor, "identity.me")["response"] = {
            kind: "object",
            fields: [{ name: "", type: "string", nullable: false }],
        };
        expect(problems(descriptor)).toContainEqual({
            key: routeKey(descriptor, "identity.me", "response.fields[0].name"),
            issue: "empty",
        });
    });
});

describe("the image edit bounds", () => {
    function editLimits(descriptor: Mutable): Mutable {
        return (tables(descriptor)["limits"] as Mutable)["edit"] as Mutable;
    }

    it("reads them, and reads a descriptor that predates them as having none", () => {
        const reading = readDescriptor(clone());
        expect(reading.ok).toBe(true);
        if (!reading.ok) return;
        expect(reading.value.descriptor.tables.limits.edit).toEqual({
            max_strokes: 64,
            max_points: 4096,
            max_edge_px: 2560,
            min_edge_px: 320,
        });

        // An older server's descriptor: no bounds, and no `kEditLimits` emitted,
        // so an editor against it fails to type-check rather than guessing.
        const older = clone();
        delete (tables(older)["limits"] as Mutable)["edit"];
        const read = readDescriptor(older);
        expect(read.ok).toBe(true);
        if (!read.ok) return;
        expect(read.value.descriptor.tables.limits.edit).toBeNull();
    });

    it("refuses a bound the recipe's wire format cannot carry", () => {
        // A stroke count is one byte in the recipe, and an edge is sixteen bits.
        const strokes = clone();
        editLimits(strokes)["max_strokes"] = 256;
        expect(problems(strokes)).toContainEqual({
            key: "tables.limits.edit.max_strokes",
            issue: "out-of-range",
        });

        const inverted = clone();
        editLimits(inverted)["min_edge_px"] = 4000;
        expect(problems(inverted)).toContainEqual({
            key: "tables.limits.edit.min_edge_px",
            issue: "out-of-range",
        });

        const zero = clone();
        editLimits(zero)["max_points"] = 0;
        expect(problems(zero)).toContainEqual({
            key: "tables.limits.edit.max_points",
            issue: "not-positive",
        });
    });
});

describe("the file itself", () => {
    it("refuses a format this generator cannot read", () => {
        // A format this generator was not written against, in the direction it
        // cannot recover from: 5 is whatever anvil adds next, and reading it
        // for the tables it recognises is guessing at a shape nobody checked.
        const descriptor = clone();
        descriptor["descriptor"] = 5;
        expect(problems(descriptor)).toContainEqual({
            key: "descriptor",
            issue: "unsupported-format",
        });
    });

    it("reads format 4 when its one addition, limits.chat, is null", () => {
        const descriptor = clone();
        descriptor["descriptor"] = 4;
        ((descriptor["tables"] as Mutable)["limits"] as Mutable)["chat"] = null;
        expect(readDescriptor(descriptor).ok).toBe(true);
    });

    it("refuses format 4 carrying chat limits it does not read", () => {
        const descriptor = clone();
        descriptor["descriptor"] = 4;
        ((descriptor["tables"] as Mutable)["limits"] as Mutable)["chat"] = { kinds: [] };
        expect(problems(descriptor)).toContainEqual({
            key: "descriptor",
            issue: "unsupported-format",
        });
    });

    it("refuses a hash that is not a sha256", () => {
        const descriptor = clone();
        descriptor["hash"] = "1e867d6c";
        expect(problems(descriptor)).toContainEqual({ key: "hash", issue: "not-a-sha256" });
    });

    it("refuses anything that is not an object", () => {
        expect(problems([] as unknown as Mutable)).toContainEqual({
            key: "",
            issue: "not-an-object",
        });
    });
});
