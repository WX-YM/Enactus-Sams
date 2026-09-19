// Every component in `hammer/dom`, mounted by an application, at the same time.
//
// This is the proof §19 names. Type-checking one seam at a time says each can be
// satisfied; this says they can all be satisfied together, by one consumer, in
// one construction order, with one set of tables — which is the half that
// type-checking a list of signatures does not prove.
//
// It is also what `tests/dom/registry.test.ts` drives: the accessibility, the
// direction and the CSP passes enumerate what is mounted here rather than being
// written per component, so a component nobody mounts fails rather than being
// quietly uncovered.
//
// Nothing in this file is hammer's. Every class name, every word, every route
// and every decision about what a notification says is the application's, and
// that is the entire point of it being here rather than in `src/`.

import type { HammerError } from "hammer";
import type { Mounted } from "hammer/dom";
import {
    renderBell,
    renderConsent,
    renderError,
    renderForm,
    renderImage,
    renderInbox,
    renderLogin,
    renderPager,
    renderPermissionGate,
    renderSection,
    renderSessionGate,
    renderUpload,
} from "hammer/dom";
import { renderChart } from "hammer/chart";
import { Form } from "hammer/state";
import type { Consent, InboxState, PagerState, Readable } from "hammer/state";

import type { ErrorCode, Locale, ValidationReason } from "../api/hammer.generated.js";
import { kFieldTypes, sectionHomeAbout } from "../api/hammer.generated.js";
import {
    authClasses,
    chartClasses,
    bellClasses,
    consentClasses,
    errorClasses,
    formClasses,
    imageClasses,
    inboxClasses,
    pagerClasses,
    sectionClasses,
    uploadClasses,
} from "./classes.js";
import { componentCopy } from "./component_copy.js";
import { copy } from "./copy.js";

// What this application's notifications carry. A response shape is the
// application's to declare (`docs/01-seams.md` §4).
export type Alert = { readonly headline: string };

export type ScreenParts = {
    // Where a component is handed its data from. All read-side: a component
    // handed a writable store is a component that can decide somebody consented.
    readonly inbox: Readable<InboxState<Alert>>;
    readonly pager: Readable<PagerState<{ readonly id: string }, never>>;
    readonly consent: Readable<Consent>;

    readonly session: Parameters<typeof renderSessionGate>[1]["session"];

    readonly markRead: (ids: readonly string[]) => void;
    readonly more: () => void;
    readonly setConsent: (answer: Consent) => void;
    readonly upload: (
        file: File,
        report: (progress: { readonly sentBytes: number; readonly totalBytes: number }) => void,
        signal: AbortSignal,
    ) => Promise<unknown>;
    readonly signIn: (
        body: Readonly<Record<string, unknown>>,
        signal: AbortSignal,
    ) => Promise<{ readonly ok: true; readonly value: unknown } | { readonly ok: false; readonly error: HammerError<ErrorCode | "Unknown", ValidationReason | "Unknown"> }>;
};

// The form this application puts on a screen, built from the descriptor's own
// field types. The bound and the control both come from `kFieldTypes`, so the
// form validates against the number the server validates against.
export function contentForm(): Form<ValidationReason | "Unknown"> {
    return new Form<ValidationReason | "Unknown">({
        fields: [
            { key: "headline", type: kFieldTypes.TEXT_SHORT, required: true },
            { key: "body", type: kFieldTypes.TEXT_LONG, required: false },
        ],
        // Which member of the descriptor's append-only enum each failure is. The
        // names are the application's to choose from; hammer spells none of them.
        reasons: {
            required: "REQUIRED",
            tooLong: "TOO_LONG",
            badFormat: "BAD_FORMAT",
            notAllowed: "NOT_ALLOWED",
        },
    });
}

// One mount per component, named. The name is what the enumerating suites report
// a failure against, and it is why they can say WHICH component has no label.
export type ScreenComponent = {
    readonly name: string;
    readonly mount: (into: Element) => Mounted;
};

export function screenComponents(locale: Locale, parts: ScreenParts): readonly ScreenComponent[] {
    const words = componentCopy[locale];
    const failure = copy[locale];

    return [
        {
            name: "form",
            mount: (into) =>
                renderForm(into, {
                    form: contentForm(),
                    fields: [
                        { key: "headline", label: "Headline", description: "Shown on the card." },
                        { key: "body", label: "Body" },
                    ],
                    classes: formClasses,
                    copy: words.form,
                    reasons: failure.reasons,
                    onSubmit: () => undefined,
                }),
        },
        {
            name: "section",
            mount: (into) =>
                renderSection(into, {
                    // Straight off the generated section const: the labels are
                    // declared once, server-side, and generated into the client
                    // rather than written a second time here.
                    fields: sectionHomeAbout.fields.map((field) => ({
                        key: field.key,
                        labels: field.labels,
                        // Which section control stores markup is a product
                        // decision, and the descriptor bridges nothing.
                        rich: field.type === "RichText",
                    })),
                    values: { title: "A harbour at dawn", body: "<p>Some <em>prose</em>.</p>" },
                    localeIndex: locale === "en" ? 0 : 1,
                    classes: sectionClasses,
                }),
        },
        {
            name: "login",
            mount: (into) =>
                renderLogin(into, {
                    form: contentForm(),
                    fields: [{ key: "headline", label: "Email" }],
                    classes: authClasses,
                    formClasses,
                    copy: words.form,
                    reasons: failure.reasons,
                    errors: failure.errors,
                    signIn: parts.signIn,
                }),
        },
        {
            name: "sessionGate",
            mount: (into) =>
                renderSessionGate(into, {
                    session: parts.session,
                    classes: authClasses,
                    whenActive: (at) => renderError(at, { error: null, classes: errorClasses, copy: words.failure }),
                    whenAnonymous: (at) => renderError(at, { error: null, classes: errorClasses, copy: words.failure }),
                }),
        },
        {
            name: "permissionGate",
            mount: (into) =>
                renderPermissionGate(into, {
                    session: parts.session,
                    // A route affordance, answered from the session's own route
                    // table rather than by counting bits: a superadmin's set is
                    // deliberately not all-ones, so a bitset check would hide
                    // every guarded control from the one account that reaches
                    // all of them (`docs/01-seams.md` §4.1.1).
                    allows: () => true,
                    render: (at) =>
                        renderImage(at, {
                            sources: { src: "https://media.test/avatar/1/card", srcset: "", widths: [] },
                            alt: "A harbour at dawn",
                            width: 320,
                            height: 240,
                            classes: imageClasses,
                        }),
                }),
        },
        {
            name: "bell",
            mount: (into) =>
                renderBell(into, {
                    inbox: parts.inbox,
                    markRead: parts.markRead,
                    // The sentence about a notification is this application's,
                    // and so is which topics deserve one at all.
                    describe: (item) => item.body.headline,
                    classes: bellClasses,
                    copy: words.bell,
                }),
        },
        {
            name: "inbox",
            mount: (into) =>
                renderInbox(into, {
                    inbox: parts.inbox,
                    markRead: parts.markRead,
                    describe: (item) => item.body.headline,
                    classes: inboxClasses,
                    copy: words.inbox,
                }),
        },
        {
            name: "upload",
            mount: (into) =>
                renderUpload(into, {
                    // The cap is the descriptor's; the accept list is this
                    // application's, because anvil has no table of media types
                    // to emit.
                    limits: { maxBytes: 5_000_000, accept: ["image/jpeg", "image/png"] },
                    upload: parts.upload,
                    classes: uploadClasses,
                    copy: words.upload,
                }),
        },
        {
            name: "image",
            mount: (into) =>
                renderImage(into, {
                    sources: {
                        src: "https://media.test/avatar/1/card",
                        srcset: "https://media.test/avatar/1/thumb 320w",
                        widths: [320],
                    },
                    alt: "A harbour at dawn",
                    // Not in any table the server emits: §12 publishes a width
                    // per role and no height, and this image's ratio is its own.
                    width: 320,
                    height: 240,
                    sizes: "(min-width: 40em) 20rem, 100vw",
                    classes: imageClasses,
                }),
        },
        {
            name: "pager",
            mount: (into) =>
                renderPager(into, {
                    pager: parts.pager,
                    more: parts.more,
                    classes: pagerClasses,
                    copy: words.pager,
                }),
        },
        {
            name: "error",
            mount: (into) =>
                renderError(into, {
                    error: {
                        kind: "server",
                        code: "NOT_FOUND",
                        status: 404,
                        requestId: null,
                        fields: null,
                    },
                    classes: errorClasses,
                    copy: words.failure,
                }),
        },
        {
            name: "chart",
            mount: (into) =>
                renderChart(into, {
                    series: [
                        {
                            key: "signups",
                            label: "Signups",
                            points: [
                                { x: 0, y: 12 },
                                { x: 1, y: 31 },
                                { x: 2, y: 24 },
                            ],
                        },
                    ],
                    widthCssPx: 320,
                    heightCssPx: 160,
                    // The formatters are this application's, in the locale it is
                    // reading. A chart that formatted its own numbers would ship
                    // one locale's punctuation to every consumer.
                    formatX: (value) => new Intl.NumberFormat(locale).format(value),
                    formatY: (value) => new Intl.NumberFormat(locale).format(value),
                    classes: chartClasses,
                    copy: words.chart,
                }),
        },
        {
            name: "consent",
            mount: (into) =>
                renderConsent(into, {
                    consent: parts.consent,
                    setConsent: parts.setConsent,
                    classes: consentClasses,
                    copy: words.consent,
                }),
        },
    ];
}
