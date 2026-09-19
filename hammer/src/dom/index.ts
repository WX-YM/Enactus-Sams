// The `hammer/dom` entry point: the one insertion site, and the components that
// hold mechanism without holding a name, a word or a look.
//
// Exports are named and explicit, for the reason the other entry points give: a
// star re-export puts every module in every bundle that touches any of them,
// because a bundler cannot drop what it was never told is side-effect free at
// the granularity it needs. An application with no upload control ships no
// upload control.
//
// Every component in here follows one shape — `renderX(mount, options)` handing
// back a `Mounted` whose `close` is not optional to keep — and takes its
// document from the element it was asked to mount in rather than from a global,
// which `tools/check-layering.sh` enforces.
//
// What is deliberately NOT exported: `brand`, and therefore any way to make a
// `SanitizedHtml` other than `sanitize`. A consumer who could forge one could
// forge the guarantee the insertion site rests on (`core/brand.ts`).

export type { Mounted } from "./mount.js";
export { Closers, bind, detach, documentOf, elementIn, isolatedAttribute, setUserText, uniqueId } from "./mount.js";

export type { SanitizedHtml, TrustedTypesError, TrustedTypesScope } from "./sanitized.js";
export { installTrustedTypes, sanitize, setSanitized } from "./sanitized.js";

export type { FormCopy, FormField, FormOptions, FormPart } from "./form.js";
export { renderForm } from "./form.js";

export type { SectionFieldView, SectionOptions, SectionPart } from "./section.js";
export { renderSection } from "./section.js";

export type {
    AuthPart,
    Branch,
    LoginOptions,
    PermissionGateOptions,
    SessionGateOptions,
} from "./auth.js";
export { renderLogin, renderPermissionGate, renderSessionGate } from "./auth.js";

export type { BellCopy, BellOptions, BellPart } from "./bell.js";
export { renderBell } from "./bell.js";

export type { InboxCopy, InboxOptions, InboxPart } from "./inbox.js";
export { renderInbox } from "./inbox.js";

export type { UploadCopy, UploadOptions, UploadPart, UploadProgressReport } from "./upload.js";
export { renderUpload } from "./upload.js";

export type { ImageOptions, ImagePart } from "./image.js";
export { renderImage } from "./image.js";

export type { PagerCopy, PagerOptions, PagerPart } from "./pager.js";
export { renderPager } from "./pager.js";

export type { ErrorCopy, ErrorOptions, ErrorPart } from "./error.js";
export { renderError } from "./error.js";

export type { ConsentCopy, ConsentOptions, ConsentPart } from "./consent.js";
export { renderConsent } from "./consent.js";
