// The preview is the render: hammer's editor, screenshotted at each output
// size, against the pictures anvil rendered from the same recipes.
//
// --- why this exists -----------------------------------------------------------
//
// `recipe.test.ts` proves the two codecs agree on bytes and on the planned size.
// Neither is the claim a person relies on, which is that what they saw in the
// editor is what the server kept. The editor paints an SVG the browser
// rasterises; anvil orients, crops and resizes with libvips and draws with its
// own capsule rasteriser (anvil `docs/21-image-edits.md` §4.2). Two painters
// agree on a picture only if somebody compares the pictures, so this does.
//
// --- where the other half comes from --------------------------------------------
//
// `tests/edit/parity/` is written by anvil's `testapp_emit_edit_renders`, the
// same arrangement as `recipe_vectors.json`: a fixture the server side
// produces and this side commits, so the run needs a browser and not a
// sibling checkout (`CLAUDE.md` §1). Every render is the derived object's
// MASTER, a PNG, so no codec stands between the two painters.
//
// --- what "agree" means -----------------------------------------------------------
//
// Not byte equality. Resampling (libvips' kernel against the browser's) and
// anti-aliasing along a stroke's edge differ by design; what must not differ
// is where anything is, which way the picture faces, and what colour a stroke
// composites to. So the measure is two numbers — the mean difference over
// every channel of every pixel, and the share of pixels any channel of which is
// off by more than kOutlierDelta — and a control the same size as a real case
// with its flip left out, which must fail both by a wide margin. A threshold
// the control also passes would be measuring nothing.

import { readFileSync } from "node:fs";
import { createServer } from "node:http";
import type { AddressInfo } from "node:net";
import { fileURLToPath } from "node:url";

import { build } from "esbuild";

import { afterAll, beforeAll, describe, expect, it } from "../support/test.js";

import type { Browser, CDPSession, Page } from "../browser/cdp.js";
import { entryAliases, launch } from "../browser/harness.js";
import type { Pixels } from "../support/png.js";
import { decodePng } from "../support/png.js";
import type { Mounted } from "./parity_page.js";

import { decodeRecipe, planEdit } from "../../src/edit/recipe.js";
import type { EditLimits, SourceSize } from "../../src/edit/recipe.js";

const kDir = fileURLToPath(new URL("./parity/", import.meta.url));

type Case = {
    readonly name: string;
    readonly file: string;
    readonly recipe: string;
    readonly out_width: number;
    readonly out_height: number;
};

type Manifest = {
    readonly source: { readonly file: string; readonly width: number; readonly height: number };
    readonly limits: { readonly max_strokes: number; readonly max_points: number; readonly max_edge_px: number; readonly min_edge_px: number };
    readonly cases: readonly Case[];
};

const manifest = JSON.parse(readFileSync(`${kDir}manifest.json`, "utf8")) as Manifest;
const source: SourceSize = { widthPx: manifest.source.width, heightPx: manifest.source.height };
const limits: EditLimits = {
    maxStrokes: manifest.limits.max_strokes,
    maxPoints: manifest.limits.max_points,
    maxEdgePx: manifest.limits.max_edge_px,
    minEdgePx: manifest.limits.min_edge_px,
};

type Difference = {
    readonly meanDelta: number;
    readonly outlierShare: number;
};

// Measured with the fixture as committed: turn, flip and crop identical to the
// byte; strokes 0.02 mean and no outlier; the resize 0.19 and no outlier; every
// operation at once 0.28, its 0.19% of outliers all along a stroke's edge. The
// control scored 42 and 81%. The ceilings leave room for another Chromium's
// anti-aliasing and none for a picture that faces the wrong way.
const kOutlierDelta = 48;
const kMeanDeltaCeiling = 1.5;
const kOutlierShareCeiling = 0.005;

// The outermost pixel of each edge is left out. Scaling an image, the browser
// filters its edge against transparency — the page shows through — where
// libvips extends the edge pixels outward; that one-pixel frame is the only
// place the two conventions differ, and it measured 50 to 70 levels out
// along the whole of one edge. Everything a person looks at is inside it.
const kInsetPx = 1;

function compare(a: Pixels, b: Pixels): Difference {
    if (a.widthPx !== b.widthPx || a.heightPx !== b.heightPx) {
        throw new Error(`sizes differ: ${a.widthPx}x${a.heightPx} against ${b.widthPx}x${b.heightPx}`);
    }
    let total = 0;
    let outliers = 0;
    let pixels = 0;
    for (let y = kInsetPx; y < a.heightPx - kInsetPx; y += 1) {
        for (let x = kInsetPx; x < a.widthPx - kInsetPx; x += 1) {
            const i = (y * a.widthPx + x) * 3;
            let worst = 0;
            for (let c = 0; c < 3; c += 1) {
                const delta = Math.abs((a.rgb[i + c] as number) - (b.rgb[i + c] as number));
                total += delta;
                worst = Math.max(worst, delta);
            }
            if (worst > kOutlierDelta) {
                outliers += 1;
            }
            pixels += 1;
        }
    }
    return { meanDelta: total / (pixels * 3), outlierShare: outliers / pixels };
}

let browser: Browser;
let close: () => Promise<void>;
let origin = "";
let page: Page;
let cdp: CDPSession;

beforeAll(async () => {
    const built = await build({
        entryPoints: [fileURLToPath(new URL("./parity_page.ts", import.meta.url))],
        bundle: true,
        format: "esm",
        target: "es2022",
        platform: "browser",
        write: false,
        outdir: fileURLToPath(new URL("../../dist/never-written", import.meta.url)),
        alias: entryAliases(),
        logLevel: "silent",
    });
    const script = built.outputFiles.map((file) => file.text).join("");
    const picture = readFileSync(`${kDir}${manifest.source.file}`);
    // The application's policy, narrowed to one page: no inline anything, so
    // the editor under test is the editor a CSP-enforcing deployment runs.
    const policy = "default-src 'none'; script-src 'self'; img-src 'self'; style-src 'none'";
    const html =
        '<!doctype html><html lang="en"><head><meta charset="utf-8"><title>parity</title></head>' +
        '<body><div id="root"></div><script type="module" src="/app.js"></script></body></html>';

    const server = createServer((request, response) => {
        const headers = { "Content-Security-Policy": policy, "Cache-Control": "no-store" };
        if (request.url === "/app.js") {
            response.writeHead(200, { ...headers, "Content-Type": "text/javascript; charset=utf-8" });
            response.end(script);
        } else if (request.url === "/source.png") {
            response.writeHead(200, { ...headers, "Content-Type": "image/png" });
            response.end(picture);
        } else {
            response.writeHead(200, { ...headers, "Content-Type": "text/html; charset=utf-8" });
            response.end(html);
        }
    });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    origin = `http://127.0.0.1:${(server.address() as AddressInfo).port}/`;
    close = () => new Promise<void>((resolve) => server.close(() => resolve()));

    browser = await launch();
    const context = await browser.newContext();
    page = await context.newPage();
    cdp = await context.newCDPSession(page);
    // One device pixel per CSS pixel, and room for the tallest oriented frame
    // below the editor's controls.
    await cdp.send("Emulation.setDeviceMetricsOverride", { width: 1200, height: 1600, deviceScaleFactor: 1, mobile: false });
}, { timeout: 60_000 });

afterAll(async () => {
    await browser?.close();
    await close?.();
});

// The preview of `recipe`, cropped to its crop box and painted at the size the
// server renders it.
async function preview(recipe: string): Promise<Pixels> {
    const decoded = decodeRecipe(recipe, limits);
    if (!decoded.ok) {
        throw new Error(`fixture recipe refused: ${decoded.error.fault}`);
    }
    const plan = planEdit(decoded.value, source, limits);
    if (!plan.ok) {
        throw new Error(`fixture recipe does not plan: ${plan.error.fault}`);
    }
    await page.goto(origin, { waitUntil: "load" });
    const at = await page.evaluate(
        (args: readonly [string, SourceSize, EditLimits]) => window.hammerParity.mount(args[0], args[1], args[2]),
        [recipe, source, limits] as const,
    ) as Mounted;
    const { crop, outWidthPx } = plan.value;
    const shot = (await cdp.send("Page.captureScreenshot", {
        format: "png",
        captureBeyondViewport: false,
        clip: {
            x: at.leftPx + crop.left,
            y: at.topPx + crop.top,
            width: crop.width,
            height: crop.height,
            // The browser repaints the vector at this scale rather than
            // resampling a screenshot, so strokes are drawn at output
            // resolution — as anvil draws them.
            scale: outWidthPx / crop.width,
        },
    })) as { readonly data: string };
    return decodePng(Buffer.from(shot.data, "base64"));
}

function rendered(file: string): Pixels {
    return decodePng(readFileSync(`${kDir}${file}`));
}

describe("the editor's preview against anvil's render", () => {
    const real = manifest.cases.filter((entry) => !entry.name.startsWith("control:"));

    it("has a render for every case the fixture names", () => {
        expect(real.length).toBeGreaterThan(0);
        for (const entry of real) {
            const pixels = rendered(entry.file);
            expect([pixels.widthPx, pixels.heightPx]).toEqual([entry.out_width, entry.out_height]);
        }
    });

    for (const entry of real) {
        it(`agrees on ${entry.name}`, async () => {
            const difference = compare(await preview(entry.recipe), rendered(entry.file));
            expect(difference.meanDelta).toBeLessThanOrEqual(kMeanDeltaCeiling);
            expect(difference.outlierShare).toBeLessThanOrEqual(kOutlierShareCeiling);
        });
    }

    it("tells a flipped picture from an unflipped one", async () => {
        const flipped = manifest.cases.find((entry) => entry.file === "turn-flip.png");
        const control = manifest.cases.find((entry) => entry.file === "control-turn.png");
        if (flipped === undefined || control === undefined) {
            throw new Error("the fixture has no control");
        }
        const difference = compare(await preview(flipped.recipe), rendered(control.file));
        // Ten times the ceiling on both measures, so the ceilings sit nowhere
        // near what a wrong picture scores.
        expect(difference.meanDelta).toBeGreaterThan(kMeanDeltaCeiling * 10);
        expect(difference.outlierShare).toBeGreaterThan(kOutlierShareCeiling * 10);
    });
});
