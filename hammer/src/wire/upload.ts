// An upload that never holds the file.
//
// A `File` is a HANDLE, not bytes, until something reads it. That is the whole
// of this module's design: `readAsArrayBuffer` on a 40 MB video is 40 MB of tab
// memory and a frozen main thread, against a budget that is frequently around
// 350 MB on a mid-range phone — and the failure mode is not slowness, it is the
// tab being killed with whatever was typed in it (`CLAUDE.md` §2.2). So the body
// handed to `fetch` is the `File` itself, or a stream over it, and
// `tools/check-source-bans.sh` fails the build on every API that would read one
// into the heap.
//
// --- the checks happen before the first byte, and not here ------------------
//
// `file.size` is a fact this client already has, and the cap is in the
// descriptor. The bounds themselves live in `core/upload_bounds.ts` and are
// re-exported below, because the layer that draws the upload control needs the
// identical answer and may not import this one — and two copies of a bound is
// how a component comes to accept what the transport then refuses.
//
// --- progress, and the platform that may not offer it -----------------------
//
// Counting bytes as they go needs a STREAMED request body, which the Fetch
// standard supports and roughly one engine implements, over HTTP/2 only. So
// there are two paths and the difference is visible to the caller rather than
// hidden: with a progress handler and a platform that streams, the body is a
// stream that counts; otherwise the body is the `File` and the browser does the
// streaming, which is the same memory property and no progress events.
//
// Never a fallback that reads the file to produce progress. That would trade the
// one property this module exists for — the bytes stay out of the heap — for a
// number on a screen.
//
// --- a stream is spent by the attempt that sends it -------------------------
//
// So `uploadBody` is called once per ATTEMPT and never once per call, and the
// client is what does that (`wire/client.ts`, `RequestBody`). Building it once
// was the defect the first end-to-end run of `client.upload` found: every retry
// after the first was handed the empty remains of the first, `fetch` rejects on
// that, and the rejection is indistinguishable from a dropped connection — so
// the call spent its whole attempt budget re-sending nothing and then reported a
// network failure for a network that was working.
//
// The counter starts at zero with each new stream, and that is the honest
// number rather than a rough edge: the bytes really are being sent a second
// time. A counter carried across attempts would report an upload as half done
// while the server had received none of it.
//
// --- where an upload goes ----------------------------------------------------
//
// To the API origin, with the session's cookies. NEVER to `MEDIA_ORIGIN`: anvil
// keeps that origin out of its credential allow-list on purpose, and a client
// that sent a credentialed request there would be undoing a server-side control
// from the outside (`docs/00-architecture.md` §8.4).

import type { UploadFile } from "../core/upload_bounds.js";

import type { ApiTypes, CallableRoute, CapabilityOption, ParamsOption } from "./client.js";

// Re-exported rather than redeclared. A caller of this layer still spells
// `checkUpload` from `hammer/wire`; there is one implementation of it.
export type { UploadFile, UploadLimits, UploadRefusal, UploadRefused } from "../core/upload_bounds.js";
export { checkUpload } from "../core/upload_bounds.js";

export type UploadProgress = {
    readonly sentBytes: number;
    readonly totalBytes: number;
};

export type UploadOptions<A extends ApiTypes, R extends CallableRoute> = {
    readonly file: File;

    // Called as bytes leave the device, where the platform can say. Once per
    // chunk at most, and never after the request has settled — the platform
    // cancels a body stream when its request is aborted or fails.
    //
    // `sentBytes` GOES BACKWARDS when an attempt is retried, because a retry
    // sends the file again from the start. A caller rendering a bar draws what
    // it is given; a caller that latches the maximum is drawing a claim about
    // the server that is not true.
    readonly onProgress?: (progress: UploadProgress) => void;

    readonly signal: AbortSignal;
} & ParamsOption<A, R> &
    CapabilityOption<R>;

// Whether this platform will send a `ReadableStream` as a request body.
//
// The detection is the standard one and it is a detection rather than a version
// check: constructing a `Request` with a stream body throws where the platform
// does not support it. Cached, because the answer cannot change and the probe
// allocates.
let streamsRequests: boolean | null = null;

export function supportsRequestStreams(): boolean {
    if (streamsRequests !== null) {
        return streamsRequests;
    }
    try {
        const probe = new Request("https://hammer.invalid/", {
            method: "POST",
            body: new ReadableStream(),
            // `duplex` is required by the standard for a streamed body and is
            // absent from the platform's TypeScript types. Attached rather than
            // spelled in the literal, which is the honest form of "the platform
            // knows this key and the types do not".
            ...({ duplex: "half" } as Record<string, unknown>),
        });
        streamsRequests = !probe.headers.has("Content-Type");
    } catch {
        streamsRequests = false;
    }
    return streamsRequests;
}

// A stream over the file that counts what passes through it.
//
// It holds one chunk at a time — whatever the platform hands it — and nothing
// else. The counter is the only state, which is what separates this from the
// implementation everybody writes first.
function counting(file: File, onProgress: (progress: UploadProgress) => void): ReadableStream {
    const source = file.stream();
    const reader = source.getReader();
    const totalBytes = file.size;
    let sentBytes = 0;

    return new ReadableStream({
        pull: async (controller) => {
            const chunk = await reader.read();
            if (chunk.done) {
                controller.close();
                return;
            }
            sentBytes += chunk.value.byteLength;
            controller.enqueue(chunk.value);
            onProgress({ sentBytes, totalBytes });
        },
        cancel: async (reason) => {
            // The screen went away mid-upload. Cancelling the source is what
            // stops the platform reading the rest of the file off disk.
            await reader.cancel(reason);
        },
    });
}

// The body an upload is sent as: a counting stream where the platform offers
// one and a progress handler asked for it, and the `File` itself otherwise.
//
// Either way the bytes stay out of the JS heap, which is the property that is
// not negotiable. The progress events are the part that degrades.
//
// Called once per attempt. The `File` arm hands back the same handle every time,
// because a `File` is re-readable; the stream arm builds a new one, because the
// last one is gone.
export function uploadBody(
    file: File,
    onProgress: ((progress: UploadProgress) => void) | undefined,
): BodyInit {
    if (onProgress === undefined || !supportsRequestStreams()) {
        return file;
    }
    return counting(file, onProgress);
}

// The type to send it as: the file's own, or nothing. A client that guessed
// would be telling the server something the person never said, and anvil
// decides what it stores from the bytes rather than from this.
export function uploadContentType(file: UploadFile): string | null {
    return file.type.length > 0 ? file.type : null;
}
