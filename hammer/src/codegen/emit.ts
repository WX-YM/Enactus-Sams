// The descriptor, as TypeScript.
//
// Two properties decide almost every line below, and both are structural rather
// than stylistic:
//
//   THE SHAPE IS THE CONTROL. A row a call site SPELLS is emitted as its own
//   `const` — a permission, a route, a section, an event. A bundler can drop an
//   export nothing references; it cannot drop one member of an object something
//   imported. That is what makes "this bundle does not contain it" true rather
//   than hoped for, and it applies to permission names as much as to routes
//   (`docs/01-seams.md` §4.1).
//
//   A row that arrives at RUN TIME is the other case, and it is a table: a field
//   type resolved by the code a definition carries, a media width resolved by the
//   namespace an object was stored in. There is no bundle in which a subset of
//   those is the useful part, so a table is what is offered — and a consumer that
//   takes it pays for all of it, which is the choice being made explicit rather
//   than made for them. `publicRoutes` and `sections` are the same offer over
//   tables whose rows can also be spelled.
//
//   WHAT IS ERASED IS FREE. A route's shape — its id, its parameters, the
//   capability it consumes — is a `type`, so it cannot leak: at run time it does
//   not exist. A holder route's method and path are the only facts that would
//   have to be values to be useful, and they are the ones not emitted at all.
//   They arrive with the session, scoped by the server to what that holder's
//   permissions reach. Code-splitting was never the control here: a lazily
//   loaded chunk is a public URL that `curl` fetches.
//
// The output is deterministic. Every table is walked in the descriptor's own
// order and nothing is sorted, because `tools/check-descriptor.sh` diffs a
// regeneration against the committed file — and a generator whose output depends
// on key order produces a diff on every run and is ignored inside a week.

import type {
    CapabilityScopeSpec,
    Descriptor,
    ErrorCodeTable,
    EventSpec,
    FieldTypeSpec,
    LocaleSpec,
    MediaTable,
    PermissionSpec,
    RateLimitSpec,
    ResponseFieldType,
    ResponseSpec,
    RouteSpec,
    SectionFieldSpec,
    SectionImageSpec,
    SectionSpec,
    Tables,
    TopicSpec,
} from "./descriptor.js";
import { pathParametersOf } from "./descriptor.js";
import {
    eventIdentifier,
    permissionIdentifier,
    routeIdentifier,
    sectionIdentifier,
    topicIdentifier,
} from "./names.js";

// Written once, and never adjacent to anything that would make this file look
// like it is building a URL out of pieces — which is the one thing
// tools/check-wire-discipline.sh is right to be suspicious of in a file whose
// whole job is to write paths into another file.
const kComment = "//";
const kIndent = "    ";
const kWidth = 79;

// The success member of an anvil enum is the zero. It is excluded from the
// failure unions so that an application's copy table does not have to hold a
// sentence for a case no error surface can reach.
const kSuccessValue = 0;

// hammer's own member, added to both wire vocabularies because both are
// append-only server-side. It is PascalCase against anvil's SCREAMING_SNAKE so
// that no enumerator anvil appends can ever collide with it.
const kUnknownMember = "Unknown";

const kIdentifier = /^[A-Za-z_][A-Za-z0-9_]*$/;

class Lines {
    private readonly out: string[] = [];

    raw(text: string): void {
        this.out.push(text);
    }

    blank(): void {
        this.out.push("");
    }

    comment(text: string): void {
        this.out.push(text.length === 0 ? kComment : `${kComment} ${text}`);
    }

    block(paragraph: readonly string[]): void {
        for (const line of paragraph) {
            this.comment(line);
        }
    }

    rule(title: string): void {
        const head = `${kComment} --- ${title} `;
        this.out.push(head.padEnd(kWidth, "-"));
    }

    text(): string {
        return `${this.out.join("\n")}\n`;
    }
}

function quote(value: string): string {
    return JSON.stringify(value);
}

function key(name: string): string {
    return kIdentifier.test(name) ? name : quote(name);
}

// A union of string literals, one member per line so that adding one is a
// one-line diff. An empty union is `never`, which is the honest type for a
// descriptor whose table is empty and is what makes every use of it fail to
// compile rather than accept anything.
function union(members: readonly string[]): readonly string[] {
    if (members.length === 0) {
        return [" never;"];
    }
    return members.map((member, index) => {
        const suffix = index === members.length - 1 ? ";" : "";
        return `${kIndent}| ${member}${suffix}`;
    });
}

function emitUnion(lines: Lines, name: string, members: readonly string[]): void {
    const body = union(members.map(quote));
    if (members.length === 0) {
        lines.raw(`export type ${name} =${body[0] ?? " never;"}`);
        return;
    }
    lines.raw(`export type ${name} =`);
    for (const line of body) {
        lines.raw(line);
    }
}

function emitRecord(lines: Lines, name: string, entries: readonly (readonly [string, string])[]): void {
    if (entries.length === 0) {
        lines.raw(`export const ${name} = {} as const;`);
        return;
    }
    lines.raw(`export const ${name} = {`);
    for (const [entryKey, value] of entries) {
        lines.raw(`${kIndent}${key(entryKey)}: ${value},`);
    }
    lines.raw("} as const;");
}

// --- the banner -------------------------------------------------------------

function emitBanner(lines: Lines, descriptor: Descriptor): void {
    lines.block([
        "DO NOT EDIT. Generated by hammer codegen.",
        "",
        "Regenerate with:",
        "",
        `${kIndent}hammer codegen --descriptor <descriptor.json> --out <dir>`,
        "",
        `Descriptor format: ${descriptor.descriptor}`,
        `Tables: sha256 ${descriptor.hash}`,
        "",
        "Nothing above `tables` in the descriptor reaches this file, and the",
        "omission is the reason the hash covers `tables` and nothing else: an",
        "application's version changes on every release and changes no table, so a",
        "module carrying it would diff on every release and be regenerated for",
        "nothing. The hash answers the one question worth failing a build over —",
        "whether this client was generated from these tables.",
        "",
        "The module has no side effects and no default export, and every table is",
        "an individual `const`. A bundler can drop an export nothing references; it",
        "cannot drop one member of an object something imported.",
    ]);
}

function emitProvenance(lines: Lines, descriptor: Descriptor): void {
    lines.rule("the client's provenance");
    lines.blank();
    lines.raw(`export const kDescriptorFormat = ${descriptor.descriptor};`);
    lines.blank();
    lines.block([
        "Compared against the hash the session response carries. A mismatch is a",
        "stale bundle, and it is surfaced rather than acted on: an automatic reload",
        "discards whatever the user had typed, on the deploy most likely to be",
        "happening during working hours (docs/00-architecture.md §7.1).",
    ]);
    lines.raw(`export const kTablesHash = ${quote(descriptor.hash)};`);
}

// --- error codes ------------------------------------------------------------

function emitErrorCodes(lines: Lines, table: ErrorCodeTable): void {
    const failures = table.codes.filter((code) => code.value !== kSuccessValue);
    const success = table.codes.find((code) => code.value === kSuccessValue);

    lines.rule("error codes");
    lines.blank();
    lines.block([
        "The wire names, unchanged. A second spelling would be a translation table",
        "in every bundle that decodes an envelope, a second copy of an enum that is",
        "append-only server-side, and a word on a screen that does not match the",
        "word in the log somebody is reading it against.",
        "",
        "`Unknown` is hammer's, and its shape is deliberate: anvil's enumerators are",
        "SCREAMING_SNAKE, so no code it appends can collide with it. A client that",
        "threw on a code added this morning would turn a deploy into an outage in",
        "every tab that was already open.",
    ]);
    emitUnion(lines, "ErrorCode", [...failures.map((code) => code.name), kUnknownMember]);
    lines.blank();

    if (success !== undefined) {
        lines.block([
            "The member that is not a failure, and therefore not a member of the union",
            "above: an application would otherwise owe a sentence for a case no error",
            "surface can reach.",
        ]);
        lines.raw(`export const kErrorCodeOk = ${quote(success.name)};`);
        lines.blank();
    }

    lines.block([
        "Every member with the value anvil stores, the success member included. The",
        "decode needs the set of names; the numbers are here because anvil keeps them",
        "in audit rows that are read back long after the deploy that wrote them.",
    ]);
    emitRecord(
        lines,
        "kErrorCodeValues",
        table.codes.map((code) => [code.name, String(code.value)] as const),
    );
    lines.blank();
    lines.block(["The bound the decode can state, because the enum is append-only."]);
    lines.raw(`export const kErrorCodeMaxValue = ${table.max};`);
    lines.blank();
    lines.block([
        "Carried for the contract suite rather than for the run time: hammer branches",
        "on the code, and this is what asserts the server still maps it the way it did.",
    ]);
    emitRecord(
        lines,
        "kErrorCodeHttpStatus",
        table.codes.map((code) => [code.name, String(code.http)] as const),
    );
    lines.blank();
    lines.block([
        "The codes the stealth filter rewrites to a 404. A client needs them to know",
        "that a not-found answer on a stealth route is not evidence of anything — and",
        "that rendering a denial there rebuilds the oracle the server removed.",
    ]);
    const hidden = table.codes.filter((code) => code.stealth_hidden).map((code) => quote(code.name));
    if (hidden.length === 0) {
        lines.raw("export const kStealthHiddenErrorCodes = [] as const;");
        return;
    }
    lines.raw("export const kStealthHiddenErrorCodes = [");
    for (const name of hidden) {
        lines.raw(`${kIndent}${name},`);
    }
    lines.raw("] as const;");
}

// --- validation reasons -----------------------------------------------------

function emitValidationReasons(lines: Lines, tables: Tables): void {
    const reasons = tables.validation_reasons;
    const failures = reasons.filter((reason) => reason.value !== kSuccessValue);

    lines.rule("validation reasons");
    lines.blank();
    lines.block([
        "anvil names the reason a field failed and never the sentence. The map from a",
        "reason to words is `Record<ValidationReason, string>` in the application, so a",
        "reason added server-side is a compile error in every client until somebody",
        "writes them — the alternative is a fallback string, and a fallback string is",
        "how a wire enumerator reaches a user in an interface that is otherwise",
        "entirely in Arabic.",
        "",
        "`Unknown` is here for the reason it is in `ErrorCode`, and it is the",
        "member that makes the compile error above safe to rely on. `Reason` is",
        "append-only server-side too, so a deploy can answer an open tab with a",
        "reason its bundle predates. The alternative to a member is dropping the",
        "field from the map, which is a form that refuses to submit with nothing",
        "marked on it — and that is worse than a sentence that is too general.",
    ]);
    emitUnion(lines, "ValidationReason", [
        ...failures.map((reason) => reason.name),
        kUnknownMember,
    ]);
    lines.blank();
    emitRecord(
        lines,
        "kValidationReasonValues",
        reasons.map((reason) => [reason.name, String(reason.value)] as const),
    );
}

// --- permissions ------------------------------------------------------------

function emitPermissions(lines: Lines, permissions: readonly PermissionSpec[]): void {
    lines.rule("permissions");
    lines.blank();
    lines.block([
        "One `const` per bit and no table of names. A membership test is",
        "`permissions.has(kPermContentRead)` — a number against sixteen bytes — and the",
        "union below is erased, so a bundle carries no permission name at all.",
        "",
        "An application that has to SHOW a permission's name writes it as",
        "`Record<Permission, string>`, which is copy, and copy is the application's",
        "(docs/01-seams.md §13). It is then total by type: a permission added",
        "server-side is a compile error until it has a word.",
        "",
        "Bit indices are stored in access tokens and in the user document and are",
        "never renumbered. A renumber is a silent regrant of authority, and it is not",
        "something this side can check.",
    ]);
    emitUnion(lines, "Permission", permissions.map((permission) => permission.name));
    lines.blank();
    for (const permission of permissions) {
        lines.raw(`export const ${permissionIdentifier(permission.name)} = ${permission.bit};`);
    }
    lines.blank();
    lines.block([
        "The one place a table of names is unavoidable, and it is its own `const` for",
        "exactly that reason.",
        "",
        "anvil sends a holder's permissions on the session response as NAMES in bit",
        "order rather than as a mask — `for_each_name` skips the reserved gaps between",
        "an application's blocks, so a bit nothing names never reaches a client as a",
        "control that authorises nothing — and nothing can turn a name back into a bit",
        "without the map. `SessionStore` takes it, and a chunk that decodes no session",
        "references it and therefore ships none of these names.",
        "",
        "That is the whole reason it is not a member of `kApiTables`: putting it there",
        "would put every permission name into any bundle that constructs a client,",
        "including the one an anonymous visitor downloads.",
    ]);
    emitRecord(
        lines,
        "kPermissionBits",
        permissions.map(
            (permission) =>
                [permission.name, permissionIdentifier(permission.name)] as const,
        ),
    );
}

// --- locales ----------------------------------------------------------------

function emitLocales(lines: Lines, locales: readonly LocaleSpec[]): void {
    lines.rule("locales");
    lines.blank();
    lines.block([
        "In the server's order, which is persisted and append-only: anvil stores a",
        "locale as a one-byte index into this table. Never sorted, never renumbered,",
        "and index 0 means nothing in particular.",
        "",
        "`collation` is carried so a client-side sort agrees with the server's. They",
        "are otherwise two different orderings of the same rows, and the disagreement",
        "shows up exactly at a page boundary — where a row appears twice, or never.",
    ]);
    emitUnion(lines, "Locale", locales.map((locale) => locale.tag));
    lines.blank();
    if (locales.length === 0) {
        lines.raw("export const kLocales = [] as const;");
        return;
    }
    lines.raw("export const kLocales = [");
    for (const locale of locales) {
        lines.raw(
            `${kIndent}{ tag: ${quote(locale.tag)}, collation: ${quote(locale.collation)}, rtl: ${String(locale.rtl)} },`,
        );
    }
    lines.raw("] as const;");
}

// --- capability scopes ------------------------------------------------------

function emitCapabilityScopes(lines: Lines, scopes: readonly CapabilityScopeSpec[]): void {
    lines.rule("capability scopes");
    lines.blank();
    lines.block([
        "A route declaring a scope takes a `Capability<Scope>` in its parameters, and",
        "the only producer of one is the call that mints it — so the requirement is",
        "discharged by the compiler rather than by a reviewer.",
        "",
        "No TTL here. A scope declares whether redemption burns the token; how long a",
        "particular token lives is a property of that token, set where it is minted.",
    ]);
    emitUnion(lines, "CapabilityScope", scopes.map((scope) => scope.name));
    lines.blank();
    emitRecord(
        lines,
        "kCapabilityScopeValues",
        scopes.map((scope) => [scope.name, String(scope.value)] as const),
    );
    lines.blank();
    lines.block([
        "Whether redemption burns the token. A call carrying a single-use capability",
        "is never auto-retried: the server consumed it whether or not the response",
        "arrived, so a retry gets CAPABILITY_INVALID and reports a failure for an",
        "operation that succeeded. The recovery is a re-read, not a retry.",
    ]);
    emitRecord(
        lines,
        "kCapabilitySingleUse",
        scopes.map((scope) => [scope.name, String(scope.single_use)] as const),
    );
}

// --- rate limits ------------------------------------------------------------

function emitRateLimits(lines: Lines, buckets: readonly RateLimitSpec[]): void {
    lines.rule("rate limits");
    lines.blank();
    lines.block([
        "Advisory, and not the limit — the server's is. The client's copy exists so an",
        "interface does not spend a user's budget on its own retries and then present",
        "them with a lockout they did not cause.",
        "",
        "A 429 holds back the whole bucket rather than the one call. Retrying the",
        "other nine while one waits is how a rate limit becomes a lockout.",
    ]);
    emitUnion(lines, "RateLimitBucket", buckets.map((bucket) => bucket.bucket));
    lines.blank();
    emitRecord(
        lines,
        "kRateLimits",
        buckets.map(
            (bucket) =>
                [
                    bucket.bucket,
                    `{ windowMs: ${bucket.window_ms}, maxEvents: ${bucket.max_events} }`,
                ] as const,
        ),
    );
}

// --- limits -----------------------------------------------------------------

function emitLimits(lines: Lines, tables: Tables): void {
    lines.rule("limits");
    lines.blank();
    lines.block([
        "Individual constants, and the unit is in the name: a bound whose unit lives",
        "in a comment is a bound that gets read as the other one. A byte cap is bytes",
        "and a text bound is code points, and the two are never conflated.",
        "",
        "The upload cap is checked against `file.size` before the first byte is sent.",
        "It is not the enforcement — anvil enforces during the stream, because a",
        "client-side check is a check an attacker skips.",
    ]);
    lines.raw(`export const kUploadMaxBytes = ${tables.limits.upload_max_bytes};`);
    lines.raw(`export const kBodyMaxBytes = ${tables.limits.body_max_bytes};`);
    lines.raw(`export const kPageLimitMax = ${tables.limits.page_limit_max};`);
}

// --- routes -----------------------------------------------------------------

function paramsType(route: RouteSpec): string {
    const names = pathParametersOf(route.path);
    if (names.length === 0) {
        return "Record<string, never>";
    }
    const fields = names.map((name) => `readonly ${key(name)}: string`).join("; ");
    return `{ ${fields} }`;
}

// --- what a route answers with ----------------------------------------------
//
// The six field types anvil names, as the types a DECODED BODY has. That last
// phrase is the whole design of this map and the reason none of these is a
// branded type: what a call site holds is the output of `JSON.parse`, and a
// brand asserts that work was done which here nobody did. `UuidText` and
// `ServerTimeText` are plain aliases — they carry the provenance in the name and
// in one comment, and they point at the constructor that turns each into the
// value hammer actually wants (`Uuid.parse`, `ServerInstant.fromServerIso`).
//
// Emitting `string` for all three would have been the same bytes and a worse
// type: anvil goes to the trouble of distinguishing them precisely because the
// distinction is the client's to make (`http/response_spec.h`).
const kResponseTypes: Readonly<Record<ResponseFieldType, string>> = {
    string: "string",
    int: "number",
    bool: "boolean",
    uuid: "UuidText",
    time: "ServerTimeText",
    strings: "readonly string[]",
};

// One field per line, so that a field anvil adds is a one-line diff in the
// consuming application's review rather than a re-wrapped paragraph.
function responseLines(response: ResponseSpec, indent: string): readonly string[] {
    const out: string[] = [];
    out.push(response.kind === "array" ? "readonly {" : "{");
    for (const field of response.fields) {
        const type = kResponseTypes[field.type];
        const declared = field.nullable ? `${type} | null` : type;
        out.push(`${indent}${kIndent}readonly ${key(field.name)}: ${declared};`);
    }
    out.push(`${indent}}${response.kind === "array" ? "[]" : ""}`);
    return out;
}

function emitResponses(lines: Lines, routes: readonly RouteSpec[]): void {
    const described = routes.filter((route) => route.response !== null);

    lines.block([
        "What a route answers with, for the routes anvil DECLARED an answer for.",
        "",
        "These are not a guess at the handler. anvil writes a described body through a",
        "response binder, where writing a key that is not next, writing one of the",
        "wrong type or stopping before the last field are compile errors on the SERVER",
        "— so the schema is a description of the bytes rather than a claim about them,",
        "which is the only condition under which generating types from one is better",
        "than generating none. A type nothing enforces is a lie that type-checks.",
        "",
        "A route with no declaration is absent here and resolves to `unknown`, which",
        "forces its call sites to narrow loudly rather than inherit that lie. Declare",
        "one in your application the way this interface was augmented before anvil",
        "emitted any of them:",
        "",
        `${kIndent}declare module "./hammer.generated.js" {`,
        `${kIndent}${kIndent}interface RouteResponses {`,
        `${kIndent}${kIndent}${kIndent}"content.get": { readonly id: string };`,
        `${kIndent}${kIndent}}`,
        `${kIndent}}`,
    ]);
    if (described.length === 0) {
        lines.raw("export interface RouteResponses {}");
        lines.blank();
        return;
    }
    lines.blank();

    lines.block([
        "A JSON string that is a UUID. A plain alias and not a branded type: this is",
        "what `JSON.parse` handed back, and nothing has parsed it yet. `Uuid.parse`",
        "from `hammer` is what turns it into the 16 bytes a comparison should use — a",
        "36-character string compared with `===` is the thing that type is for.",
    ]);
    lines.raw("export type UuidText = string;");
    lines.blank();
    lines.block([
        "A JSON string that is an RFC 3339 instant in UTC. `ServerInstant.fromServerIso`",
        "from `hammer` is what turns it into a value, and the reason to bother is that",
        "a `ServerInstant` cannot be subtracted from `Date.now()`: the device clock is",
        "user-settable and is routinely minutes out, so a countdown built from the",
        "difference of two clocks is wrong by an amount nothing on the device can",
        "measure (`ENGINEERING_RULES.md` §6).",
    ]);
    lines.raw("export type ServerTimeText = string;");
    lines.blank();

    lines.raw("export interface RouteResponses {");
    for (const route of described) {
        if (route.response === null) {
            continue;
        }
        const body = responseLines(route.response, "");
        const head = body[0] ?? "{";
        lines.raw(`${kIndent}${key(route.id)}: ${head}`);
        for (let at = 1; at < body.length; at += 1) {
            const line = body[at];
            if (line === undefined) {
                continue;
            }
            lines.raw(at === body.length - 1 ? `${kIndent}${line};` : `${kIndent}${line}`);
        }
    }
    lines.raw("}");
    lines.blank();
}

function emitRouteConst(lines: Lines, route: RouteSpec): void {
    const holder = route.visibility !== "public";
    const perms = route.perms.map((name) => permissionIdentifier(name)).join(", ");
    const page =
        route.page === null
            ? "null"
            : `{ cursor: ${quote(route.page.cursor)}, limitMax: ${route.page.limit_max} }`;

    lines.raw(`export const ${routeIdentifier(route.id)} = {`);
    lines.raw(`${kIndent}id: ${quote(route.id)},`);
    lines.raw(`${kIndent}access: ${quote(route.access)},`);
    lines.raw(`${kIndent}visibility: ${quote(route.visibility)},`);
    lines.raw(`${kIndent}method: ${holder ? "null" : quote(route.method)},`);
    lines.raw(`${kIndent}path: ${holder ? "null" : quote(route.path)},`);
    lines.raw(`${kIndent}perms: [${perms}],`);
    lines.raw(
        `${kIndent}capability: ${route.capability.length === 0 ? "null" : quote(route.capability)},`,
    );
    lines.raw(
        `${kIndent}rateLimit: ${route.rate_limit.length === 0 ? "null" : quote(route.rate_limit)},`,
    );
    lines.raw(`${kIndent}idempotent: ${String(route.idempotent)},`);
    lines.raw(`${kIndent}page: ${page},`);
    lines.raw("} as const;");
}

function emitRoutes(lines: Lines, routes: readonly RouteSpec[]): void {
    const publicRoutes = routes.filter((route) => route.visibility === "public");
    const holderRoutes = routes.filter((route) => route.visibility !== "public");

    lines.rule("routes");
    lines.blank();
    lines.block([
        "Every route's SHAPE is a type, and types are erased: a shape cannot leak",
        "because at run time it does not exist. What is emitted as a value is decided",
        "by visibility, and only there.",
    ]);
    emitUnion(lines, "RouteId", routes.map((route) => route.id));
    lines.blank();
    lines.block(["Reachable with no credential at all — the paths below are values."]);
    emitUnion(lines, "PublicRouteId", publicRoutes.map((route) => route.id));
    lines.blank();
    lines.block([
        "Everything else. The id is here, the path is not: it arrives with the session,",
        "filtered by the server to what this holder's permissions actually reach, so a",
        "content editor never learns the addresses of the routes above them.",
    ]);
    emitUnion(lines, "HolderRouteId", holderRoutes.map((route) => route.id));
    lines.blank();

    lines.block([
        "The path parameters of each route, whatever its visibility. A holder route's",
        "parameters are as safe to emit as its id: this is a type, and it is gone by",
        "the time a bundle exists.",
    ]);
    lines.raw("export type RouteParams = {");
    for (const route of routes) {
        lines.raw(`${kIndent}readonly ${key(route.id)}: ${paramsType(route)};`);
    }
    lines.raw("};");
    lines.blank();

    lines.block(["The capability scope each route consumes, or null."]);
    lines.raw("export type RouteCapability = {");
    for (const route of routes) {
        const scope = route.capability.length === 0 ? "null" : quote(route.capability);
        lines.raw(`${kIndent}readonly ${key(route.id)}: ${scope};`);
    }
    lines.raw("};");
    lines.blank();

    emitResponses(lines, routes);
    lines.raw(
        "export type ResponseOf<Id extends RouteId> = Id extends keyof RouteResponses",
    );
    lines.raw(`${kIndent}? RouteResponses[Id]`);
    lines.raw(`${kIndent}: unknown;`);
    lines.blank();

    lines.block([
        "One `const` per route. A screen that calls two routes ships two, and a bundle",
        "an anonymous visitor can fetch carries neither the ids nor the facts of the",
        "rest — which is a property of the emission shape, not of the bundler's",
        "configuration.",
        "",
        "`method` and `path` are null for a holder route in every bundle. There is no",
        "table of them anywhere in this file, deliberately: an aggregate would put",
        "every route's facts into any bundle that touched one of them.",
    ]);
    for (let i = 0; i < routes.length; i += 1) {
        const route = routes[i];
        if (route === undefined) {
            continue;
        }
        if (i !== 0) {
            lines.blank();
        }
        emitRouteConst(lines, route);
    }
    lines.blank();

    lines.block([
        "The public tier as one table, for a client that resolves an id at run time.",
        "Only this tier can be a table at all — a holder route has no path to put in",
        "one — and a consumer that wants the whole table pays for the whole table,",
        "which is the choice being offered rather than made for them.",
    ]);
    emitRecord(
        lines,
        "publicRoutes",
        publicRoutes.map((route) => [route.id, routeIdentifier(route.id)] as const),
    );
}

// --- the two names `createClient` takes --------------------------------------
//
// Both are assembled here rather than in the application, and for the same
// reason the tables above are emitted rather than written: what a consumer
// copies out of a guide is what a consumer gets wrong. Every application was
// writing the identical four-member type and passing the identical four tables
// one at a time, which is four opportunities to hand a client a table from a
// descriptor other than this one — and nothing in the type system was looking.

function emitApi(lines: Lines): void {
    lines.rule("the client");
    lines.blank();
    lines.block([
        "What this client knows, as ONE type parameter rather than four. Every call is",
        "then typed by the route `const` it names: a route that does not exist cannot",
        "be spelled, a parameter cannot be omitted, and a route declaring a capability",
        "scope will not compile without a `Capability` for that scope.",
        "",
        "A route with no `RouteResponses` declaration resolves to `unknown` here, which",
        "forces its call sites to narrow loudly rather than inherit a lie.",
        "",
        "`hash` is what PAIRS this type with the tables below it. It is a literal type,",
        "so two applications with two generated modules cannot cross them: the tables",
        "carry one descriptor's hash and the type demands that descriptor's hash, and a",
        "mismatch is a compile error at `createClient` rather than a client decoding one",
        "server's vocabulary against another's. Two modules generated from the SAME",
        "descriptor pair freely, which is correct — they are the same tables.",
        "",
        `${kIndent}import { createClient } from "hammer/wire";`,
        `${kIndent}import { kApiTables, routeAuthRefresh } from "./hammer.generated.js";`,
        `${kIndent}import type { Api } from "./hammer.generated.js";`,
        "",
        `${kIndent}const client = createClient<Api>({`,
        `${kIndent}${kIndent}api: kApiTables,`,
        `${kIndent}${kIndent}refreshRoute: routeAuthRefresh,`,
        `${kIndent}${kIndent}origin: { pageOrigin: location.origin, apiOrigin, site: null },`,
        `${kIndent}${kIndent}fetch: window.fetch.bind(window),`,
        `${kIndent}${kIndent}session: sessions.source,`,
        `${kIndent}});`,
    ]);
    lines.raw("export type Api = {");
    lines.raw(`${kIndent}readonly params: RouteParams;`);
    lines.raw(`${kIndent}readonly responses: { readonly [Id in RouteId]: ResponseOf<Id> };`);
    lines.raw(`${kIndent}readonly code: ErrorCode;`);
    lines.raw(`${kIndent}readonly reason: ValidationReason;`);
    lines.raw(`${kIndent}readonly hash: typeof kTablesHash;`);
    lines.raw("};");
    lines.blank();

    lines.block([
        "One descriptor's tables, as one member. They are generated together and they",
        "are handed over together, so the set a client decodes with is the set whose",
        "hash is checked against the server's.",
        "",
        "An object literal over the constants above — no call and no import — so a",
        "bundle that references none of this still drops all of it, and this module",
        "still depends on nothing.",
        "",
        "`hash` is the sixth member and it is not decoded with: it is the descriptor",
        "this set came from, as a value, so that `Api` above can demand the same one.",
        "Without it the pairing was a CONVENTION — both names come out of one file — and",
        "a convention holds until an application has two generated modules.",
        "",
        "`refreshRoute` is deliberately NOT in here. The descriptor does not say which",
        "route a refresh goes to, so putting one in would be this generator guessing at",
        "an id; the application names it, and naming it is one line.",
    ]);
    lines.raw("export const kApiTables = {");
    lines.raw(`${kIndent}codes: kErrorCodeValues,`);
    lines.raw(`${kIndent}reasons: kValidationReasonValues,`);
    lines.raw(`${kIndent}rateLimits: kRateLimits,`);
    lines.raw(`${kIndent}singleUse: kCapabilitySingleUse,`);
    lines.raw(`${kIndent}bodyMaxBytes: kBodyMaxBytes,`);
    lines.raw(`${kIndent}hash: kTablesHash,`);
    lines.raw("} as const;");
}

// --- the content tables -----------------------------------------------------
//
// The same two properties decide these as decide the tables above, and one more
// that only applies here: a union is emitted from EXACTLY what the table holds
// and never from a vocabulary written down on this side. anvil owns the words —
// the answer kinds, the section field types, the event classes, the media roles
// — and a copy of an enum in a generator is the second copy nobody updates
// (`ENGINEERING_RULES.md` §1). Deriving the union means a member anvil appends appears the
// day the descriptor carries it and never a release later.

// The distinct members of a column, in the table's own order. Never sorted: the
// output is diffed against committed bytes, so an order that depends on
// anything but the descriptor is a diff on every run.
function distinct(values: readonly string[]): readonly string[] {
    const seen = new Set<string>();
    const out: string[] = [];
    for (const value of values) {
        if (seen.has(value)) {
            continue;
        }
        seen.add(value);
        out.push(value);
    }
    return out;
}

function list(values: readonly string[]): string {
    return `[${values.map(quote).join(", ")}]`;
}

// --- field types ------------------------------------------------------------

function emitFieldTypes(lines: Lines, types: readonly FieldTypeSpec[]): void {
    lines.rule("field types");
    lines.blank();
    lines.block([
        "What a form definition's fields may declare. The renderer is hammer's and",
        "the table is the application's: a field type is machinery — a control, a",
        "validation rule, an answer shape — and the particular set an application",
        "declares is not.",
    ]);
    emitUnion(lines, "FieldTypeName", types.map((type) => type.name));
    lines.blank();
    lines.block([
        "The JSON shape an answer arrives in and is read back as. A PII type has",
        "`null` here rather than a kind: its value is never echoed back, so a",
        "renderer told `\"text\"` renders a control for a value that has silently",
        "emptied itself.",
    ]);
    emitUnion(
        lines,
        "AnswerKind",
        distinct(types.map((type) => type.answer ?? "").filter((kind) => kind.length !== 0)),
    );
    lines.blank();
    lines.block([
        "One table rather than one `const` per type, because the lookup is by the",
        "`code` a definition carries at run time: every consumer that renders a form",
        "resolves a type it was handed rather than one it spelled, so there is no",
        "bundle in which a subset of this table is the useful part.",
        "",
        "`defaultCodePoints` is CODE POINTS, as its name says, and it is the number",
        "the server checks against. A client enforcing it in UTF-16 code units",
        "refuses text the server would have accepted, which for any non-Latin script",
        "is most of it.",
        "",
        "The flags are named booleans and not the byte anvil stores, for the reason",
        "everything else here is a name: a client carrying a copy of a bitmask is a",
        "client carrying a copy of an enum.",
    ]);
    lines.raw("export const kFieldTypes = {");
    for (const type of types) {
        lines.raw(`${kIndent}${key(type.name)}: {`);
        lines.raw(`${kIndent.repeat(2)}code: ${type.code},`);
        lines.raw(`${kIndent.repeat(2)}pii: ${String(type.pii)},`);
        lines.raw(
            `${kIndent.repeat(2)}answer: ${type.answer === null ? "null" : quote(type.answer)},`,
        );
        lines.raw(`${kIndent.repeat(2)}defaultCodePoints: ${type.default_code_points},`);
        lines.raw(`${kIndent.repeat(2)}flags: {`);
        lines.raw(`${kIndent.repeat(3)}options: ${String(type.flags.options)},`);
        lines.raw(`${kIndent.repeat(3)}attachment: ${String(type.flags.attachment)},`);
        lines.raw(`${kIndent.repeat(3)}ranged: ${String(type.flags.ranged)},`);
        lines.raw(`${kIndent.repeat(3)}codePointCapped: ${String(type.flags.code_point_capped)},`);
        lines.raw(`${kIndent.repeat(3)}multiLine: ${String(type.flags.multi_line)},`);
        lines.raw(`${kIndent.repeat(3)}multiSelect: ${String(type.flags.multi_select)},`);
        lines.raw(`${kIndent.repeat(2)}},`);
        lines.raw(`${kIndent}},`);
    }
    lines.raw("} as const;");
}

// --- sections ---------------------------------------------------------------

function emitSectionFields(lines: Lines, fields: readonly SectionFieldSpec[]): void {
    if (fields.length === 0) {
        lines.raw(`${kIndent}fields: [],`);
        return;
    }
    lines.raw(`${kIndent}fields: [`);
    for (const field of fields) {
        lines.raw(`${kIndent.repeat(2)}{`);
        lines.raw(`${kIndent.repeat(3)}key: ${quote(field.key)},`);
        lines.raw(`${kIndent.repeat(3)}type: ${quote(field.type)},`);
        lines.raw(`${kIndent.repeat(3)}labels: ${list(field.labels)},`);
        lines.raw(`${kIndent.repeat(3)}maxCodePoints: ${field.max_code_points},`);
        lines.raw(`${kIndent.repeat(3)}localized: ${String(field.localized)},`);
        lines.raw(`${kIndent.repeat(3)}required: ${String(field.required)},`);
        lines.raw(`${kIndent.repeat(3)}choices: ${list(field.choices)},`);
        lines.raw(`${kIndent.repeat(2)}},`);
    }
    lines.raw(`${kIndent}],`);
}

function emitSectionImages(lines: Lines, images: readonly SectionImageSpec[]): void {
    if (images.length === 0) {
        lines.raw(`${kIndent}images: [],`);
        return;
    }
    lines.raw(`${kIndent}images: [`);
    for (const image of images) {
        const aspect =
            image.aspect === null
                ? "null"
                : `{ num: ${image.aspect.num}, den: ${image.aspect.den} }`;
        lines.raw(`${kIndent.repeat(2)}{`);
        lines.raw(`${kIndent.repeat(3)}slot: ${quote(image.slot)},`);
        lines.raw(`${kIndent.repeat(3)}labels: ${list(image.labels)},`);
        lines.raw(`${kIndent.repeat(3)}minWidth: ${image.min_width},`);
        lines.raw(`${kIndent.repeat(3)}minHeight: ${image.min_height},`);
        lines.raw(`${kIndent.repeat(3)}aspect: ${aspect},`);
        lines.raw(`${kIndent.repeat(2)}},`);
    }
    lines.raw(`${kIndent}],`);
}

function emitSections(lines: Lines, sections: readonly SectionSpec[]): void {
    lines.rule("sections");
    lines.blank();
    lines.block([
        "The editable surface: what a section holds, what bounds each field is under,",
        "and which image slots it offers.",
        "",
        "`labels` is the one place the descriptor carries words a person reads, and it",
        "is not an exception to the rule that hammer ships none. They are the",
        "application's own labels for its own editors, declared once in the",
        "application's own table and generated from there — which is the opposite of a",
        "library shipping a string and of an application keeping a second copy of a",
        "table. They are indexed by the LOCALE TABLE's order, which is persisted and",
        "append-only, so index 0 means nothing in particular.",
    ]);
    emitUnion(lines, "SectionKey", sections.map((section) => section.key));
    lines.blank();
    lines.block([
        "The control a field asks for. What it looks like is the application's — a",
        "mapping is not a component.",
    ]);
    emitUnion(
        lines,
        "SectionFieldType",
        distinct(sections.flatMap((section) => section.fields.map((field) => field.type))),
    );
    lines.blank();
    lines.block([
        "One `const` per section, because a screen that edits one section ships one",
        "section — and a section is the only table here whose rows carry words, in",
        "every declared locale, which is the difference between a few hundred bytes",
        "and all of them.",
    ]);
    for (let i = 0; i < sections.length; i += 1) {
        const section = sections[i];
        if (section === undefined) {
            continue;
        }
        if (i !== 0) {
            lines.blank();
        }
        lines.raw(`export const ${sectionIdentifier(section.key)} = {`);
        lines.raw(`${kIndent}key: ${quote(section.key)},`);
        lines.raw(`${kIndent}sitePath: ${quote(section.site_path)},`);
        emitSectionFields(lines, section.fields);
        emitSectionImages(lines, section.images);
        lines.raw("} as const;");
    }
    lines.blank();
    lines.block([
        "The whole registry, for an editor that lists what there is to edit rather",
        "than knowing. A consumer that wants every section pays for every section,",
        "which is the choice being offered rather than made for them.",
    ]);
    emitRecord(
        lines,
        "sections",
        sections.map((section) => [section.key, sectionIdentifier(section.key)] as const),
    );
}

// --- notification topics ----------------------------------------------------

function emitTopics(lines: Lines, topics: readonly TopicSpec[]): void {
    const open = topics.filter((topic) => topic.visibility === "public");
    const holder = topics.filter((topic) => topic.visibility !== "public");

    lines.rule("notification topics");
    lines.blank();
    lines.block([
        "The same split the route table is under, applied to the other table that",
        "has one (`docs/01-seams.md` §4.1). A route's PATH is what calling it needs",
        "and a topic's KEY is what subscribing to it needs, so the key is the half",
        "withheld here — subscription is the disclosure, and a topic gated by a",
        "permission is one whose existence is part of what the permission protects.",
        "",
        "A bundle is a public file and a lazily loaded chunk is a public URL, so the",
        "key of a holder topic is in no bundle, no chunk and no source map. It",
        "arrives from the preferences endpoint, filtered by the server to the topics",
        "this holder may actually subscribe to, and a client re-attaches the entry it",
        "is given to the `const` below by CODE — which is why the code is emitted for",
        "every topic and the key is not.",
        "",
        "None of this is a boundary. anvil refuses the subscription, and for a topic",
        "that declares `stealthOnDenial` it refuses it the way it refuses a stealth",
        "route: as though the topic were not there.",
    ]);
    emitUnion(lines, "TopicKey", topics.map((topic) => topic.key));
    lines.blank();
    lines.block(["Subscribable with no permission at all — the keys below are values."]);
    emitUnion(lines, "PublicTopicKey", open.map((topic) => topic.key));
    lines.blank();
    lines.block([
        "Everything else. The key is a type here and nowhere a value: this union is",
        "erased, and by the time a bundle exists there is nothing of it left.",
    ]);
    emitUnion(lines, "HolderTopicKey", holder.map((topic) => topic.key));
    lines.blank();
    emitUnion(lines, "TopicFanOut", distinct(topics.map((topic) => topic.fanout)));
    lines.blank();
    emitUnion(lines, "TopicScope", distinct(topics.map((topic) => topic.scope)));
    lines.blank();
    lines.block([
        "The transports anvil itself implements. A channel is not a row an",
        "application declares — adding one means shipping a sender — so this union is",
        "what the table holds and never more.",
    ]);
    emitUnion(
        lines,
        "NotificationChannel",
        distinct(topics.flatMap((topic) => [...topic.default_channels])),
    );
    lines.blank();
    lines.block([
        "One `const` per topic. `userOptional` is false where the reader may not turn",
        "it off: a client that offers a switch there offers one the server refuses,",
        "and a security alert an account can silence is not an alert.",
        "",
        "`code` is a BIT POSITION in a stored preference mask and is append-only, the",
        "way a permission bit is. It is also the identity a holder topic is matched",
        "on, since its key is not here.",
    ]);
    for (let i = 0; i < topics.length; i += 1) {
        const topic = topics[i];
        if (topic === undefined) {
            continue;
        }
        if (i !== 0) {
            lines.blank();
        }
        const withheld = topic.visibility !== "public";
        lines.raw(`export const ${topicIdentifier(topic.key)} = {`);
        lines.raw(`${kIndent}key: ${withheld ? "null" : quote(topic.key)},`);
        lines.raw(`${kIndent}code: ${topic.code},`);
        lines.raw(`${kIndent}visibility: ${quote(topic.visibility)},`);
        lines.raw(`${kIndent}fanout: ${quote(topic.fanout)},`);
        lines.raw(`${kIndent}scope: ${quote(topic.scope)},`);
        lines.raw(`${kIndent}defaultChannels: ${list(topic.default_channels)},`);
        lines.raw(`${kIndent}coalesceWindowSeconds: ${topic.coalesce_window_s},`);
        lines.raw(`${kIndent}retentionDays: ${topic.retention_days},`);
        lines.raw(`${kIndent}userOptional: ${String(topic.user_optional)},`);
        lines.raw(`${kIndent}stealthOnDenial: ${String(topic.stealth_on_denial)},`);
        lines.raw(
            `${kIndent}perms: [${topic.perms.map((name) => permissionIdentifier(name)).join(", ")}],`,
        );
        lines.raw("} as const;");
    }
    lines.blank();
    lines.block([
        "The public tier as one table, for a client that resolves a key at run time.",
        "Only this tier can be a table at all — a holder topic has no key to put in",
        "one — which is the same sentence `publicRoutes` is under, for the same",
        "reason.",
    ]);
    emitRecord(
        lines,
        "publicTopics",
        open.map((topic) => [topic.key, topicIdentifier(topic.key)] as const),
    );
}

// --- analytics events -------------------------------------------------------

function emitEvents(lines: Lines, events: readonly EventSpec[]): void {
    lines.rule("analytics events");
    lines.blank();
    lines.block([
        "What may be reported, and under what consent. One `const` per event and no",
        "table of them: a call site names the event it reports, so a screen that",
        "reports one ships one — and an aggregate would put every event's name into",
        "any bundle that touched a single one.",
        "",
        "`requiresConsent` is read at the door. An event that declares it is not",
        "queued pending a decision, because a buffer that flushes when consent",
        "arrives is a buffer of pre-consent data, which is the thing consent was",
        "about.",
    ]);
    emitUnion(lines, "EventName", events.map((event) => event.name));
    lines.blank();
    emitUnion(lines, "EventClass", distinct(events.map((event) => event.class)));
    lines.blank();
    lines.block([
        "A dimension's values are the CLOSED set the server stores an index into, so",
        "the `as const` is what makes a typo a compile error rather than a row the",
        "ingest path drops — and a dropped row is the analytics defect nobody notices",
        "for a quarter.",
    ]);
    for (let i = 0; i < events.length; i += 1) {
        const event = events[i];
        if (event === undefined) {
            continue;
        }
        if (i !== 0) {
            lines.blank();
        }
        lines.raw(`export const ${eventIdentifier(event.name)} = {`);
        lines.raw(`${kIndent}name: ${quote(event.name)},`);
        lines.raw(`${kIndent}code: ${event.code},`);
        lines.raw(`${kIndent}class: ${quote(event.class)},`);
        lines.raw(`${kIndent}requiresConsent: ${String(event.requires_consent)},`);
        if (event.dimensions.length === 0) {
            lines.raw(`${kIndent}dimensions: {},`);
        } else {
            lines.raw(`${kIndent}dimensions: {`);
            for (const dimension of event.dimensions) {
                lines.raw(
                    `${kIndent.repeat(2)}${key(dimension.name)}: ${list(dimension.values)},`,
                );
            }
            lines.raw(`${kIndent}},`);
        }
        lines.raw("} as const;");
    }
}

// --- media ------------------------------------------------------------------

function emitMedia(lines: Lines, media: MediaTable): void {
    lines.rule("media");
    lines.blank();
    lines.block([
        "A role is what an image is FOR, and it is the whole public grammar:",
        "`GET /media/{ns}/{id}/{role}`, built by the route builder and never by",
        "concatenation. The width is published BESIDE the role rather than instead of",
        "it, because a responsive `srcset` cannot exist without width descriptors —",
        "the browser has no basis on which to choose between the sources it is handed.",
        "",
        "Knowing a width was never the hazard. Building a URL out of one was, and",
        "there is no ladder here to build one from: no file extension, no format list,",
        "no rule from a width back to a path. A role→width mapping may change without",
        "a client release, and the addresses a client already holds keep working while",
        "it does.",
        "",
        "Bytes never enter the JS heap: this is an `<img src>` pointed at the media",
        "origin, not a fetch and a blob.",
    ]);
    emitUnion(lines, "MediaNamespace", media.namespaces.map((namespace) => namespace.ns));
    lines.blank();
    emitUnion(
        lines,
        "MediaRole",
        distinct(
            media.namespaces.flatMap((namespace) => namespace.roles.map((role) => role.role)),
        ),
    );
    lines.blank();
    lines.block([
        "What a request with no role segment resolves to, and therefore what a caller",
        "that forgets the segment gets.",
    ]);
    lines.raw(`export const kMediaDefaultRole = ${quote(media.default_role)};`);
    lines.blank();
    lines.block([
        "One table, because a namespace arrives with the object being rendered rather",
        "than being spelled at the call site.",
    ]);
    emitRecord(
        lines,
        "kMediaWidths",
        media.namespaces.map(
            (namespace) =>
                [
                    namespace.ns,
                    `{ ${namespace.roles
                        .map((role) => `${key(role.role)}: ${role.width}`)
                        .join(", ")} }`,
                ] as const,
        ),
    );
    lines.blank();
    lines.block([
        "What each namespace will TAKE, which is the server's list and not a second",
        "one. It reaches two places and they have to agree: the `accept` attribute on",
        "a file input, which decides what the picker offers, and the check that runs",
        "against `File.type` before a byte is sent. An application that wrote this",
        "down itself kept a copy of something the server already enforces, and the",
        "copy is the one that goes stale — the symptom being an upload the picker",
        "offered and the server refused, or a file the picker hid and the server would",
        "have taken.",
        "",
        "Lowercase `type/subtype` with no parameters, which is the essence string a",
        "browser puts in `File.type`, so the comparison is `===` rather than a parse.",
    ]);
    emitRecord(
        lines,
        "kMediaAccepts",
        media.namespaces.map(
            (namespace) =>
                [
                    namespace.ns,
                    `[${namespace.accepts.map(quote).join(", ")}]`,
                ] as const,
        ),
    );
}

// --- the whole module -------------------------------------------------------

export function emitClient(descriptor: Descriptor): string {
    const lines = new Lines();
    const tables = descriptor.tables;

    emitBanner(lines, descriptor);
    lines.blank();
    emitProvenance(lines, descriptor);
    lines.blank();
    emitErrorCodes(lines, tables.error_codes);
    lines.blank();
    emitValidationReasons(lines, tables);
    lines.blank();
    emitPermissions(lines, tables.permissions);
    lines.blank();
    emitLocales(lines, tables.locales);
    lines.blank();
    emitCapabilityScopes(lines, tables.capability_scopes);
    lines.blank();
    emitRateLimits(lines, tables.rate_limits);
    lines.blank();
    emitLimits(lines, tables);
    lines.blank();
    emitRoutes(lines, tables.routes);
    lines.blank();
    emitApi(lines);
    lines.blank();
    emitFieldTypes(lines, tables.field_types);
    lines.blank();
    emitSections(lines, tables.sections);
    lines.blank();
    emitTopics(lines, tables.topics);
    lines.blank();
    emitEvents(lines, tables.events);
    lines.blank();
    emitMedia(lines, tables.media);

    return lines.text();
}

// The file the generator writes. Named so that an application's .gitignore can
// say `*.generated.ts` and mean it.
export const kOutputFileName = "hammer.generated.ts";
