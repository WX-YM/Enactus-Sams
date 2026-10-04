// Asserted against the BUILT output, not against the source.
//
// The source is already checked: `tools/check-source-bans.sh` refuses every
// markup sink in `src/`, and `tools/check-vocabulary.sh` refuses a sentence in
// `src/dom` and `src/chart`. Both are the checks a refactor routes around — an
// exemption comment, a helper in a file nobody scans, a bundler configuration
// that pulls in something else. What follows is the claim in the form a consumer
// can verify: the bytes an application actually ships.
//
// It is also the assertion that made `dom/sanitized.ts` parse rather than assign.
// A `// ban-exempt:` line would have satisfied the source check and left this one
// failing, which is the right way round: the exemption mechanism cannot buy its
// way past the property that matters.

import { fileURLToPath } from "node:url";

import { build } from "esbuild";
import { describe, expect, it } from "../support/test.js";

import { componentCopy } from "../testapp/app/component_copy.js";

const kRoot = fileURLToPath(new URL("../../", import.meta.url));

const kAliases = {
    hammer: fileURLToPath(new URL("../../src/core/index.ts", import.meta.url)),
    "hammer/wire": fileURLToPath(new URL("../../src/wire/index.ts", import.meta.url)),
    "hammer/state": fileURLToPath(new URL("../../src/state/index.ts", import.meta.url)),
    "hammer/dom": fileURLToPath(new URL("../../src/dom/index.ts", import.meta.url)),
    "hammer/chart": fileURLToPath(new URL("../../src/chart/index.ts", import.meta.url)),
};

async function bundle(entry: string): Promise<string> {
    const result = await build({
        entryPoints: [fileURLToPath(new URL(entry, import.meta.url))],
        bundle: true,
        minify: true,
        format: "esm",
        target: "es2022",
        platform: "browser",
        write: false,
        outdir: `${kRoot}dist/never-written`,
        alias: kAliases,
        // Left as UTF-8 rather than escaped to `\uXXXX`. The default would turn
        // every Arabic string in the output into an ASCII escape, and then an
        // assertion that the application's own words are present would pass or
        // fail on the bundler's escaping rather than on the words.
        charset: "utf8",
        // A preserved licence banner is prose, and prose is what the last two
        // cases below are looking for.
        legalComments: "none",
        logLevel: "silent",
    });
    return result.outputFiles.map((file) => file.text).join("");
}

// The reference consumer's screen, which mounts every component this library
// ships. What is in this bundle is what an application that used all of them
// would send to a device.
const kScreen = await bundle("../testapp/app/screen.ts");

describe("the bundle an application ships", () => {
    // The row `docs/16-test-plan.md` asks for, and the reason the insertion site
    // parses into an inert document instead of assigning.
    it("contains no markup sink at all", () => {
        for (const sink of [
            "innerHTML",
            "outerHTML",
            "insertAdjacentHTML",
            "document.write",
            "createContextualFragment",
        ]) {
            expect(kScreen).not.toContain(sink);
        }
    });

    it("contains no run-time code generation", () => {
        expect(kScreen).not.toContain("new Function");
        expect(kScreen).not.toMatch(/\beval\(/);
    });

    // A file read into the heap is the one memory property the upload and image
    // paths exist to keep.
    it("contains no route that reads a file into the heap", () => {
        for (const reader of ["readAsArrayBuffer", "readAsDataURL", "createObjectURL"]) {
            expect(kScreen).not.toContain(reader);
        }
    });

    it("holds no credential anywhere it could be read back", () => {
        for (const store of ["localStorage", "sessionStorage", "indexedDB", "document.cookie"]) {
            expect(kScreen).not.toContain(store);
        }
    });

    it("contains no predictable randomness", () => {
        expect(kScreen).not.toContain("Math.random");
    });
});

// --- what is NOT asserted here, and why ------------------------------------
//
// "No component contains a user-visible string" is checked over the SOURCE, by
// `tools/check-vocabulary.sh`, which scans `src/dom` and `src/chart` for three
// lowercase words inside a literal and exempts a `throw new Error(...)` on the
// same line — because `throw` is reserved for programmer error here, so the
// audience for one of those strings is whoever is holding a stack trace.
//
// Doing the same over the BUILT output was attempted and abandoned, and the
// reason is worth keeping. Finding string literals in minified JavaScript needs
// a real parse: a regex literal can contain a quote — `escapeAttribute` in
// `dom/sanitized.ts` contains exactly one — and a scanner that pairs quotes by
// hand mistakes the code between two strings for a string. It reported the
// library's own module boundaries as copy. A dependency-free repository does not
// get to have a JavaScript parser in its test suite for one assertion, and a
// scanner that is wrong in the direction of passing is worse than no scanner.
//
// What survives is the half that needs no parse: a sink is a fixed identifier
// and either appears in the bytes or does not.

describe("the words in an application's bundle", () => {
    // Present because the application put them there, in both declared locales.
    // An English default would ship to the Arabic one from the same bytes.
    it("are the application's own, in every locale it declares", () => {
        for (const locale of ["en", "ar"] as const) {
            const words = componentCopy[locale];
            for (const sentence of [
                words.form.summary,
                words.bell.empty,
                words.upload.label,
                words.pager.more,
                words.consent.question,
            ]) {
                expect(kScreen).toContain(sentence);
            }
        }
    });
});
