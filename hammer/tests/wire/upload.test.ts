// The upload, and the one property that is not negotiable: the bytes never
// enter the JS heap.
//
// `tools/check-source-bans.sh` is what proves the absence of
// `readAsArrayBuffer`, `readAsDataURL` and `createObjectURL` across the whole
// library — a test can only assert what this module does, and the ban is what
// asserts what no module does. What is here is what this layer decides: what the
// body ends up being, and what the progress stream counts. What an upload is
// REFUSED for moved to `tests/core/upload_bounds.test.ts` along with the bounds
// themselves, which the layer that draws the control also has to reach.

import { describe, expect, it } from "../support/test.js";

import {
    checkUpload,
    supportsRequestStreams,
    uploadBody,
    uploadContentType,
} from "../../src/wire/upload.js";

describe("the bounds this layer still spells", () => {
    // Not a duplicate of the core suite: what is asserted here is that
    // `hammer/wire` still offers the name, because a consumer that imported it
    // from this entry point is not supposed to learn that it moved.
    it("re-exports the one implementation", () => {
        expect(checkUpload({ size: 0, type: "image/jpeg" }, { maxBytes: 1, accept: [] })).toMatchObject({
            ok: false,
            error: { cause: "bad-parameter" },
        });
    });
});

describe("the body", () => {
    // A File is a handle, not bytes, until something reads it. Handing it to
    // fetch is what keeps a 40 MB video out of a tab whose whole budget is
    // frequently 350 MB.
    it("is the file itself when nothing asked for progress", () => {
        const file = new File(["hello"], "a.txt", { type: "text/plain" });
        expect(uploadBody(file, undefined)).toBe(file);
    });

    it("counts the bytes that pass through it when progress was asked for", async () => {
        const file = new File(["hello world"], "a.txt", { type: "text/plain" });
        const seen: number[] = [];
        const body = uploadBody(file, (progress) => seen.push(progress.sentBytes));

        if (body === file) {
            // A platform that will not send a stream body. The fallback is the
            // file itself, which is the same memory property and no progress —
            // never a read of the file to produce a number for a screen.
            expect(seen).toEqual([]);
            return;
        }

        const reader = (body as ReadableStream<Uint8Array>).getReader();
        let total = 0;
        for (;;) {
            const chunk = await reader.read();
            if (chunk.done) {
                break;
            }
            total += chunk.value.byteLength;
        }

        expect(total).toBe(file.size);
        expect(seen[seen.length - 1]).toBe(file.size);
    });

    it("stops reading the file when the request is cancelled", async () => {
        const file = new File(["hello world"], "a.txt", { type: "text/plain" });
        const body = uploadBody(file, () => {});
        if (body === file) {
            return;
        }
        await expect(
            (body as ReadableStream<Uint8Array>).cancel("the screen went away"),
        ).resolves.toBeUndefined();
    });
});

describe("the type it is sent as", () => {
    it("is the file's own", () => {
        expect(uploadContentType({ size: 1, type: "image/png" })).toBe("image/png");
    });

    // A client that guessed would be telling the server something the person
    // never said, and anvil decides what it stores from the bytes.
    it("is absent where the platform gave none", () => {
        expect(uploadContentType({ size: 1, type: "" })).toBe(null);
    });
});

// The probe the two body paths branch on.
//
// Published, because the difference is visible to a caller rather than hidden: a
// platform that will not send a streamed body gets no progress events, and a
// control that draws a bar has to be able to ask BEFORE it draws one rather than
// discover it by never being called.
describe("whether this platform will send a streamed body", () => {
    it("answers without throwing, on whatever engine is running this", () => {
        expect(typeof supportsRequestStreams()).toBe("boolean");
    });

    it("gives the same answer every time, because the answer cannot change", () => {
        // Cached, and the cache is the point: the probe constructs a `Request`
        // to find out, and doing that once per upload is an allocation per file
        // for a fact that was settled when the page loaded.
        expect(supportsRequestStreams()).toBe(supportsRequestStreams());
    });

    it("is what decides which body an upload is sent as", () => {
        // The tie. A probe that disagreed with the branch it exists for would
        // let a control promise progress that never arrives.
        const file = new File(["hello"], "a.txt", { type: "text/plain" });
        const body = uploadBody(file, () => {});
        expect(body instanceof ReadableStream).toBe(supportsRequestStreams());
        expect(body === file).toBe(!supportsRequestStreams());
    });
});
