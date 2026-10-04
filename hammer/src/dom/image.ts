// The image element, and the two attributes that are not optional.
//
// --- bytes never enter the JS heap ------------------------------------------
//
// An image is an `<img>` pointed at the media origin. There is no `fetch`, no
// `blob()`, no `createObjectURL` and no `data:` URI anywhere on this path, and
// `tools/check-source-bans.sh` refuses every one of them across the library.
// anvil serves image bytes with `sendfile()` precisely so they never enter ITS
// heap; decoding them into a JS `Blob` to display them would put the whole file
// in the one heap with the least room, on the device least able to spare it, and
// lose the CDN cache besides (`CLAUDE.md` §2.2).
//
// So this module sets attributes. That is the entire implementation, and the
// fact that it is short is the design rather than an absence of one.
//
// --- the intrinsic box is required ------------------------------------------
//
// `width` and `height` are required parameters, not optional ones. Without both,
// the browser has no aspect ratio until the bytes arrive and every image on the
// page is a layout shift — which is the one performance defect that is also an
// accessibility defect, because the thing somebody was about to tap moves out
// from under their finger.
//
// They are not in the media table and cannot be: §12 publishes a width per role
// and no height, and the aspect ratio of a particular image is not in any table
// the server emits. So the caller supplies both (`docs/01-seams.md` §19).
//
// --- format negotiation is the browser's ------------------------------------
//
// No branch here picks WebP over JPEG. anvil answers with `Vary: Accept` and the
// browser sends what it can decode; a client that chose would be a second
// opinion with worse information and a cache key nobody shares.

import type { ClassNames } from "../core/tables.js";

import type { ImageSources } from "../state/media.js";

import { Closers, detach, documentOf, elementIn, isolatedAttribute } from "./mount.js";
import type { Mounted } from "./mount.js";

export type ImagePart = "root";

export type ImageOptions = {
    // Built by `state/media.ts` from the namespace's roles and widths. The
    // addresses in it came from the route builder, which is what keeps a path
    // from being assembled out of a width (`docs/01-seams.md` §12).
    readonly sources: ImageSources;

    // The words. Required, and an empty string is a real answer: it marks an
    // image that carries no information a reader would otherwise miss. What is
    // not available is leaving it out, because an unlabelled image is a defect
    // and one shipped from here ships to every consumer at once (`CLAUDE.md` §9).
    readonly alt: string;

    // CSS pixels, both of them. See above.
    readonly width: number;
    readonly height: number;

    // How wide the image will be laid out, in the application's own terms. It is
    // a fact about a layout and the layout is the application's, so there is no
    // default worth inventing: without it the browser assumes the full viewport
    // and picks the largest source every time.
    readonly sizes?: string;

    // Off-screen images are worth deferring and the one at the top of the page
    // never is. Which this is depends on where the application puts it.
    readonly loading?: "lazy" | "eager";
    readonly decoding?: "async" | "sync" | "auto";

    readonly classes: ClassNames<ImagePart>;
};

export function renderImage(mount: Element, options: ImageOptions): Mounted {
    const doc = documentOf(mount);
    const closers = new Closers();

    const image = elementIn(doc, "img", options.classes.root);

    image.src = options.sources.src;
    if (options.sources.srcset.length > 0) {
        image.srcset = options.sources.srcset;
    }
    if (options.sizes !== undefined) {
        image.sizes = options.sizes;
    }

    // Both, always. One of them alone gives the browser no ratio to reserve.
    image.width = options.width;
    image.height = options.height;

    // Isolated, and the reason is the case people forget: when the image does
    // not load, the browser renders this text INLINE in the page flow. That is
    // user-authored text interpolated into a sentence, which is precisely what
    // `CLAUDE.md` §8 says must carry its own direction — an RTL caption without
    // it reorders the Latin words around it, changing which words the sentence
    // appears to contain. An attribute cannot hold `dir`, so the isolate travels
    // in the value.
    image.alt = isolatedAttribute(options.alt);

    image.loading = options.loading ?? "lazy";
    image.decoding = options.decoding ?? "async";

    mount.append(image);
    closers.add(() => detach(image));

    return { element: image, close: () => closers.run() };
}
