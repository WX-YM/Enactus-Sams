// Asserted against the BUILT output, not against the source.
//
// The source assertion is the one a bundler configuration silently invalidates,
// and the property being claimed is about bytes an anonymous visitor can fetch.
// `/assets/staff-a3f2.js` is fetched with `curl`; if the whole staff route table
// is inside it, splitting changed which bundle downloads the map in a browser and
// changed nothing about who can read it (docs/01-seams.md §4.1).

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { build } from "esbuild";
import { describe, expect, it } from "vitest";

import { readDescriptorJson } from "../../src/codegen/descriptor.js";
import { kOutputFileName } from "../../src/codegen/emit.js";

const kRoot = fileURLToPath(new URL("../../", import.meta.url));
const kCoreEntry = fileURLToPath(new URL("../../src/core/index.ts", import.meta.url));
const kWireEntry = fileURLToPath(new URL("../../src/wire/index.ts", import.meta.url));

type Built = {
    readonly code: string;
    readonly map: string;
    readonly outputs: readonly string[];
};

async function bundle(entry: string): Promise<Built> {
    const result = await build({
        entryPoints: [fileURLToPath(new URL(`../testapp/app/${entry}`, import.meta.url))],
        bundle: true,
        minify: true,
        format: "esm",
        target: "es2022",
        platform: "browser",
        sourcemap: "external",
        metafile: true,
        write: false,
        outdir: `${kRoot}dist/never-written`,
        alias: { hammer: kCoreEntry, "hammer/wire": kWireEntry },
        logLevel: "silent",
    });

    let code = "";
    let map = "";
    for (const file of result.outputFiles) {
        if (file.path.endsWith(".map")) {
            map = file.text;
        } else {
            code = file.text;
        }
    }
    return { code, map, outputs: Object.keys(result.metafile.outputs) };
}

// The generated module's own source, as the map carries it.
//
// Scoped to that one file rather than to the whole map, because the claim is
// about what the GENERATOR wrote. hammer's own prose explains route patterns and
// says `/content/{id}` while doing it, and an assertion that fires on the
// sentence explaining a rule is one somebody turns off — which is the lesson
// check-layering.sh and check-wire-discipline.sh each learned once already.
function generatedSourceIn(map: string): string {
    const parsed: unknown = JSON.parse(map);
    const asRecord = parsed as {
        readonly sources?: readonly string[];
        readonly sourcesContent?: readonly string[];
    };
    const sources = asRecord.sources ?? [];
    const contents = asRecord.sourcesContent ?? [];
    const at = sources.findIndex((name) => name.endsWith(kOutputFileName));
    const found = at < 0 ? undefined : contents[at];
    if (found === undefined) {
        throw new Error(`the source map carries no ${kOutputFileName}`);
    }
    return found;
}

const kPublic = await bundle("public_screen.ts");
const kStaff = await bundle("staff_screen.ts");

// Read from the descriptor rather than written down here. A route added
// server-side has to be covered by these assertions the moment it exists, and a
// hand-kept list is a list that goes stale in the direction of passing.
const kReading = readDescriptorJson(
    readFileSync(fileURLToPath(new URL("../testapp/hammer.descriptor.json", import.meta.url)), "utf8"),
);
if (!kReading.ok) {
    throw new Error("the reference descriptor does not read");
}
const kTables = kReading.value.descriptor.tables;

const kHolderRoutes = kTables.routes.filter((route) => route.visibility !== "public");
const kHolderPaths = kHolderRoutes.map((route) => route.path);
const kPublicPaths = kTables.routes
    .filter((route) => route.visibility === "public")
    .map((route) => route.path);

// A holder path whose bytes a PUBLIC path already puts in the bundle.
//
// `/media/{ns}/{id}/{role}` is public and contains `/me`, which is
// `identity.me`'s path — so a substring search over the bundle reports the
// public path as a leak of the holder one. It is the same collision the source
// map assertion at the foot of this file already had to name, and it arrived in
// the bundle the day anvil described the media grammar as a route.
//
// The narrowing is deliberately minimal. Every other holder path keeps the
// unquoted substring check, which is the one that would catch a path ASSEMBLED
// rather than emitted; only a path a public one already spells is checked as a
// quoted literal, which is the form the generator would have written it in.
function shadowedByAPublicPath(path: string): boolean {
    return kPublicPaths.some((candidate) => candidate !== path && candidate.includes(path));
}

function expectNoHolderPath(code: string): void {
    expect(kHolderPaths.length).toBeGreaterThan(0);
    for (const path of kHolderPaths) {
        if (shadowedByAPublicPath(path)) {
            for (const quote of ['"', "'", "`"]) {
                expect(code).not.toContain(`${quote}${path}${quote}`);
            }
            continue;
        }
        expect(code).not.toContain(path);
    }
}
const kHolderIds = kHolderRoutes.map((route) => route.id);
const kPermissionNames = kTables.permissions.map((permission) => permission.name);
const kHolderTopicKeys = kTables.topics
    .filter((topic) => topic.visibility !== "public")
    .map((topic) => topic.key);

describe("the bundle an anonymous visitor downloads", () => {
    it("contains the public paths it calls", () => {
        expect(kPublic.code).toContain("/login");
    });

    it("contains no holder route's path", () => {
        expectNoHolderPath(kPublic.code);
    });

    it("contains no permission name", () => {
        // The bits are numbers and the union is erased, so this holds for a
        // permission the screen DOES check as well as for one it does not.
        expect(kPermissionNames.length).toBeGreaterThan(0);
        for (const name of kPermissionNames) {
            expect(kPublic.code).not.toContain(name);
        }
    });

    it("does not name a route only a holder reaches", () => {
        // Not a control — the ids of the routes a screen calls are in whichever
        // chunk calls them, and anvil's stealth 404 is what actually withholds
        // an administrative surface. It is the honest scope of what the emission
        // shape buys: an anonymous bundle learns nothing about the staff
        // surface, because nothing in it referenced one.
        expect(kHolderIds.length).toBeGreaterThan(0);
        for (const id of kHolderIds) {
            expect(kPublic.code).not.toContain(id);
        }
    });

    it("contains the key of a topic nothing gates", () => {
        expect(kPublic.code).toContain("content.published");
    });

    it("contains no holder topic's key", () => {
        expect(kHolderTopicKeys.length).toBeGreaterThan(0);
        for (const key of kHolderTopicKeys) {
            expect(kPublic.code).not.toContain(key);
        }
    });

    it("contains no descriptor", () => {
        // A JSON file in an output directory looks exactly like an asset, which
        // is why this is easy to get wrong: the descriptor carries every route's
        // path, visibility included.
        expect(kPublic.code).not.toContain("emitted_by");
        expect(kPublic.code).not.toContain("stealth_hidden");
        expect(kPublic.outputs.some((name) => name.endsWith(".json"))).toBe(false);
        expect(kPublic.outputs.some((name) => name.includes(kOutputFileName))).toBe(false);
    });
});

describe("the bundle only a holder reaches", () => {
    it("carries the route id, because the call site is in it", () => {
        // What remains exposed after §4.1 is the id and the call site. Where the
        // existence of a feature is itself sensitive, the answer is deployment —
        // serve the entry point from a protected location — and not codegen.
        expect(kStaff.code).toContain("audit.list");
    });

    it("carries no path for it", () => {
        expectNoHolderPath(kStaff.code);
    });

    it("carries no key for the topic it does gate on", () => {
        // The stronger half of the topic split, and the difference from a route:
        // a route id is in the bundle that calls it, but a holder topic's key is
        // in NO bundle — not even the one that holds the const. The screen gates
        // on the permission bits and matches a preferences entry by code; the key
        // arrives from the server, filtered to this holder.
        // Matched on a property name, which minification keeps, rather than on
        // the export name, which it does not.
        expect(kStaff.code).toContain("stealthOnDenial");
        expect(kStaff.code).toContain("key:null");
        for (const key of kHolderTopicKeys) {
            expect(kStaff.code).not.toContain(key);
        }
    });

    it("carries nothing of the routes it does not call", () => {
        expect(kStaff.code).not.toContain("content.delete");
        expect(kStaff.code).not.toContain("media.list");
    });
});

describe("a source map", () => {
    it("re-leaks everything tree-shaking removed", () => {
        // Asserted rather than assumed, because it is the reason for the policy:
        // a privileged bundle publishes no public source map. It uploads one to
        // the error reporter privately, or it ships none.
        const generated = generatedSourceIn(kStaff.map);
        expect(kStaff.code).not.toContain("kPermSystemAnnounce");
        expect(generated).toContain("kPermSystemAnnounce");

        // And it still holds no holder path, because the emission never wrote
        // one: the map carries the generated module in full, and there is
        // nothing in it to re-leak. That is the difference between a fact and a
        // build-time optimisation.
        //
        // Quoted, because that is how a path would be emitted. The generated
        // module carries prose as well as values — the media grammar explains
        // `/media/{ns}/{id}/{role}`, and `/me` is a holder path and a substring
        // of it — and an assertion that fires on the sentence explaining a rule
        // is one somebody turns off. The bundle assertions above stay unquoted:
        // minification leaves no prose for them to fire on.
        for (const path of kHolderPaths) {
            expect(generated).not.toContain(`"${path}"`);
        }
    });
});
