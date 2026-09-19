// What an upload is refused for, before a byte of it is read.
//
// This is arithmetic over two numbers and a list, and it lives in the layer that
// imports nothing because THREE layers need the same answer. `hammer/wire` needs
// it to refuse a request it is about to make; `hammer/dom` needs it to refuse a
// file the moment it is dropped, and may not import `hammer/wire`
// (`tools/check-layering.sh`). The alternative was a second copy of the bounds in
// the layer that draws the control — which is the shape of defect where a
// component accepts what the transport then refuses, and the person sees a file
// vanish with no reason attached to it.
//
// --- it is a round-trip saver and never a control ---------------------------
//
// anvil enforces the cap during the read, because a client-side check is a check
// an attacker skips (`docs/01-seams.md` §12). What refusing here buys is the
// upload that was going to be refused after the last byte — on a connection
// where forty megabytes is minutes rather than seconds.

import type { ClientError } from "./errors.js";
import type { Result } from "./result.js";
import { fail, ok } from "./result.js";

// The facts an upload is checked against, and nothing else: a test needs no
// `File` to drive the decision, and the decision needs no bytes.
export type UploadFile = {
    readonly size: number;
    readonly type: string;
};

export type UploadLimits = {
    // `kUploadMaxBytes` from the descriptor. Bytes, and the name says so: a byte
    // cap and a text bound are never conflated (`docs/01-seams.md` §12).
    readonly maxBytes: number;

    // The media types this route accepts. The descriptor carries none — anvil
    // has no table of them to emit — so it is the application's, and an empty
    // list means "the server decides", which is where the decision lives anyway.
    readonly accept: readonly string[];
};

// Exactly the causes this check can produce, and no others.
//
// Narrower than `ClientError["cause"]` on purpose: a surface that has to say
// WHY a file was refused needs a total table of words for it (`docs/01-seams.md`
// §13), and being total over the whole client-error union would mean writing a
// sentence about a full request queue for a control that cannot produce one.
export type UploadRefusal = "bad-parameter" | "too-large" | "unsupported-media";

export type UploadRefused = ClientError & { readonly cause: UploadRefusal };

// What a request will not be made for.
export function checkUpload(file: UploadFile, limits: UploadLimits): Result<void, UploadRefused> {
    if (file.size <= 0) {
        // A zero-byte file is a picker that returned a directory, a file that
        // was moved between the choice and the read, or a stream that ended
        // before it started. None of them is an upload.
        return fail({ kind: "client", cause: "bad-parameter", retryAfterMs: null });
    }
    if (file.size > limits.maxBytes) {
        return fail({ kind: "client", cause: "too-large", retryAfterMs: null });
    }
    if (limits.accept.length > 0 && !limits.accept.includes(file.type)) {
        return fail({ kind: "client", cause: "unsupported-media", retryAfterMs: null });
    }
    return ok();
}
