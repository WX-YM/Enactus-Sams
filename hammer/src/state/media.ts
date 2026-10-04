// Where an image comes from, and how one gets sent.
//
// --- the read side: bytes never enter the JS heap ---------------------------
//
// This module produces a `src` and a `srcset` — strings for an `<img>` to
// resolve — and nothing else. There is no `fetch`, no `blob()`, no
// `createObjectURL` and no `data:` URI anywhere on this path, because decoding a
// photograph into a JS `Blob` to display it puts the whole file in the one heap
// with the least room, on the device least able to spare it, and loses the CDN
// cache besides (`CLAUDE.md` §2.2). anvil serves image bytes with `sendfile()`
// so they never enter ITS heap; undoing that on the client is the same defect
// with worse consequences.
//
// The media origin never receives credentials and never issues a state change
// (`docs/00-architecture.md` §8.7). Nothing here can send one: an `<img src>` is
// not a `fetch`, and there is no code path from this module to the request
// pipeline.
//
// --- why the width is published and the path still is not --------------------
//
// A responsive `srcset` cannot exist without width descriptors — without them
// the browser has no basis on which to choose between the sources it is handed.
// So the descriptor publishes the width BESIDE the role rather than instead of
// it (`docs/01-seams.md` §12): the request is still by role, the address still
// comes from the route builder, and there is no ladder here to derive a path
// from. Knowing a width was never the hazard; building a URL out of one was.
//
// --- the write side: through `imagePool`, one bitmap at a time ---------------
//
// A photograph off a phone is eight to twelve megapixels and several megabytes.
// Sending it as it is costs the person their connection; re-encoding it on the
// main thread costs them the frame. So it goes through the image pool, which is
// two workers because two decoded bitmaps is 96 MB and three is a killed tab
// (`state/workers/image.ts`), and the `File` reaches that pool as a HANDLE — it
// is never read here.

import type { ClientError } from "../core/errors.js";
import type { Result } from "../core/result.js";
import { fail, ok } from "../core/result.js";
import type { ApiTypes, CallResult, CallableRoute, Client } from "../wire/client.js";
import type { BuiltRoute, PathParams } from "../wire/route.js";
import { buildRoute } from "../wire/route.js";
import type { HttpMethod } from "../wire/session_view.js";
import type { UploadLimits } from "../wire/upload.js";
import { checkUpload } from "../wire/upload.js";

import type { DownscaleRequest, DownscaleResult } from "./workers/image.js";
import { checkDownscale } from "./workers/image.js";
import type { ImagePool } from "./workers/image.js";
import type { PoolError } from "./workers/pool.js";

// The role→width table the descriptor emits, as a shape rather than a table
// (`CLAUDE.md` §1). hammer knows there are namespaces and that each serves roles
// at widths; which ones an application declared is not its business.
export type MediaWidths = Readonly<Record<string, Readonly<Record<string, number>>>>;

export type MediaConfig = {
    // `MEDIA_ORIGIN`. A deployment fact, so it is handed over rather than
    // derived (`docs/01-seams.md` §16). It is a different registrable host from
    // the site's on purpose, and nothing in this library ever sends it a
    // credential.
    readonly origin: string;

    // The public media route. Its path is the grammar `/media/{ns}/{id}/{role}`
    // and the substitution is the route builder's, so a namespace or an id that
    // held a dot segment or half a surrogate pair is refused rather than
    // concatenated (`wire/route.ts`).
    readonly route: { readonly method: HttpMethod; readonly path: string };

    readonly widths: MediaWidths;

    // What a request with no role segment resolves to, so a caller that forgets
    // one does not get a 404 found in production (`docs/01-seams.md` §12).
    readonly defaultRole: string;
};

export type ImageSources = {
    // The default role's address, for the `src` attribute and for a browser that
    // does not pick from a `srcset`.
    readonly src: string;

    // `"<url> 320w, <url> 1024w"`, in ascending width. Ascending because it is
    // what a person reading the attribute expects, and because a list that
    // wandered would read as though the widths meant something else.
    readonly srcset: string;

    readonly widths: readonly number[];
};

export type MediaError =
    // The descriptor has no such namespace, or it serves no such role. A
    // generation-time union makes the first unspellable in a typed application;
    // this is what answers for a value that arrived at run time with an object.
    | "unknown-namespace"
    | "unknown-role"
    // The namespace does not serve the default role, so a request without one
    // would resolve to nothing.
    | "no-default-role"
    | "bad-subject";

// Every source for one stored image, at every width its namespace serves.
export function imageSources(
    config: MediaConfig,
    subject: { readonly ns: string; readonly id: string },
): Result<ImageSources, MediaError> {
    const roles = readTable(config.widths, subject.ns);
    if (roles === null) {
        return fail("unknown-namespace");
    }

    const entries: { readonly role: string; readonly width: number }[] = [];
    for (const [role, width] of Object.entries(roles)) {
        entries.push({ role, width });
    }
    if (entries.length === 0) {
        return fail("unknown-role");
    }
    entries.sort((left, right) => left.width - right.width);

    const defaultWidth = readWidth(roles, config.defaultRole);
    if (defaultWidth === null) {
        return fail("no-default-role");
    }

    const src = address(config, subject, config.defaultRole);
    if (src === null) {
        return fail("bad-subject");
    }

    // One pass building one string, rather than a `map` and a `join` over a
    // throwaway array of formatted pieces. A gallery renders this per thumbnail.
    let srcset = "";
    const widths: number[] = [];
    for (const entry of entries) {
        const url = address(config, subject, entry.role);
        if (url === null) {
            return fail("bad-subject");
        }
        if (srcset.length > 0) {
            srcset += ", ";
        }
        srcset += url + " " + String(entry.width) + "w";
        widths.push(entry.width);
    }

    return ok({ src, srcset, widths });
}

// One address, for a caller that wants a single role rather than a ladder.
export function imageSource(
    config: MediaConfig,
    subject: { readonly ns: string; readonly id: string },
    role: string,
): Result<string, MediaError> {
    const roles = readTable(config.widths, subject.ns);
    if (roles === null) {
        return fail("unknown-namespace");
    }
    if (readWidth(roles, role) === null) {
        return fail("unknown-role");
    }
    const url = address(config, subject, role);
    return url === null ? fail("bad-subject") : ok(url);
}

// --- the write side ---------------------------------------------------------

export type UploadImageOptions<R extends CallableRoute> = {
    readonly route: R;
    readonly params?: PathParams;
    readonly file: File;
    readonly limits: UploadLimits;

    // What to re-encode to before sending. Absent for a file that should go as
    // it is — a vector, an animation, or an image already small enough that a
    // re-encode would only lose detail.
    readonly downscale?: Omit<DownscaleRequest, "file">;

    readonly signal: AbortSignal;
};

export type UploadImageError = ClientError | PoolError;

// Downscale, then upload, with the bytes never entering this thread's heap.
//
// The order matters and it is not the obvious one: a refusal is worked out
// BEFORE a worker is spawned. A 40 MB file the server will refuse should cost
// nothing at all, and a client that downscaled first would spend the decode to
// find that out — on the device that can least afford it.
//
// What the preflight checks is the original's SIZE and the type the upload will
// actually CARRY, which is not the original's type: a photograph re-encoded to
// WebP is a WebP by the time the server sees it, so checking the JPEG it started
// as would refuse exactly the conversion the downscale exists to perform. It is
// a round-trip saver either way — the real check is `client.upload`'s, against
// the body that is really sent, and anvil's is the one that counts.
export async function uploadImage<A extends ApiTypes, R extends CallableRoute>(
    client: Client<A>,
    pool: ImagePool,
    options: UploadImageOptions<R>,
): Promise<Result<CallResult<A, R>, UploadImageError>> {
    const allowed = checkUpload(
        { size: options.file.size, type: options.downscale?.type ?? options.file.type },
        options.limits,
    );
    if (!allowed.ok) {
        return fail(allowed.error);
    }

    let body: File = options.file;
    if (options.downscale !== undefined) {
        const request: DownscaleRequest = { ...options.downscale, file: options.file };
        const checked = checkDownscale(request);
        if (!checked.ok) {
            return fail(checked.error);
        }

        const scaled = await pool.downscale(request, options.signal);
        if (!scaled.ok) {
            // A full pool is a refusal the CALLER has to answer, because the
            // answer is one only a caller has: send the original, ask again
            // later, or say so on screen. Retrying here would be this module
            // deciding to hold a second photograph in memory.
            return fail(scaled.error);
        }
        body = asFile(scaled.value, options.file.name);
    }

    // Through the same pipeline as every other request: queued, bounded,
    // retried by policy and observable. The body is a handle either way.
    const answered = await client.upload(options.route, options.limits, {
        file: body,
        signal: options.signal,
        ...(options.params === undefined ? {} : { params: options.params }),
    } as never);

    return ok(answered);
}

// --- the pieces -------------------------------------------------------------

// `hasOwnProperty` rather than a bare read, for the reason `state/cache.ts`
// gives: the namespace arrives with an object rather than being spelled, so a
// value of `constructor` would otherwise resolve to a function.
function readTable(widths: MediaWidths, ns: string): Readonly<Record<string, number>> | null {
    if (!Object.prototype.hasOwnProperty.call(widths, ns)) {
        return null;
    }
    return widths[ns] ?? null;
}

function readWidth(roles: Readonly<Record<string, number>>, role: string): number | null {
    if (!Object.prototype.hasOwnProperty.call(roles, role)) {
        return null;
    }
    const width = roles[role];
    return typeof width === "number" ? width : null;
}

function address(
    config: MediaConfig,
    subject: { readonly ns: string; readonly id: string },
    role: string,
): string | null {
    const built = buildRoute(config.route, {
        params: { ns: subject.ns, id: subject.id, role },
        query: {},
    });
    if (!built.ok) {
        return null;
    }
    return mediaUrl(config.origin, built.value);
}

// The media origin plus a built path. It is not `requestUrl`, deliberately:
// that one takes an `ApiOrigin`, which is the origin the session's cookies go
// to. Two functions rather than one parameter, so there is no call site where
// the media origin and the API origin are one substitution apart.
function mediaUrl(origin: string, built: BuiltRoute): string {
    return origin + built.path + (built.query.length === 0 ? "" : "?" + built.query);
}

// A `File` rather than a `Blob`, because `client.upload` reads a name and a type
// off it and a multipart body would otherwise carry neither. The bytes are not
// copied: a `File` built over a `Blob` shares its storage.
function asFile(scaled: DownscaleResult, name: string): File {
    return new File([scaled.blob], name, { type: scaled.blob.type });
}
