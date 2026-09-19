// What each component's parts are called, in this application's design system.
//
// The part union belongs to whichever component declares it; hammer ships the
// structure, the behaviour, the ARIA and the direction, and not one class name.
// Every union below is IMPORTED from `hammer/dom` rather than written here,
// which is what makes each table total by type: a part a component adds is a
// compile error in this file until somebody says what it is called, and a part
// it removes is one too.
//
// That is the same bargain anvil's `static_assert`s make, in the only currency
// TypeScript has — and the reason these are `ClassNames<Part>` rather than a
// loose record is that `Partial` would turn every one of those compile errors
// into an unstyled element nobody notices until it is on a screen.

import type { ClassNames } from "hammer";
import type { ChartPart } from "hammer/chart";
import type {
    AuthPart,
    BellPart,
    ConsentPart,
    ErrorPart,
    FormPart,
    ImagePart,
    InboxPart,
    PagerPart,
    SectionPart,
    UploadPart,
} from "hammer/dom";

export const formClasses: ClassNames<FormPart> = {
    root: "tf",
    field: "tf-field",
    label: "tf-label",
    required: "tf-required",
    control: "tf-control",
    description: "tf-description",
    counter: "tf-counter",
    error: "tf-error",
    summary: "tf-summary",
    choices: "tf-choices",
    choice: "tf-choice",
    submit: "tf-submit",
};

export const sectionClasses: ClassNames<SectionPart> = {
    root: "ts",
    field: "ts-field",
    label: "ts-label",
    value: "ts-value",
};

export const authClasses: ClassNames<AuthPart> = {
    root: "ta",
    pending: "ta-pending",
    error: "ta-error",
};

export const bellClasses: ClassNames<BellPart> = {
    root: "tb",
    trigger: "tb-trigger",
    badge: "tb-badge",
    popover: "tb-popover",
    list: "tb-list",
    item: "tb-item",
    unreadItem: "tb-item-unread",
    empty: "tb-empty",
    markRead: "tb-mark",
    live: "tb-live",
};

export const inboxClasses: ClassNames<InboxPart> = {
    root: "ti",
    list: "ti-list",
    item: "ti-item",
    unreadItem: "ti-item-unread",
    empty: "ti-empty",
    live: "ti-live",
};

export const uploadClasses: ClassNames<UploadPart> = {
    root: "tu",
    label: "tu-label",
    input: "tu-input",
    dropZone: "tu-zone",
    dragging: "tu-dragging",
    progress: "tu-progress",
    cancel: "tu-cancel",
    error: "tu-error",
};

export const imageClasses: ClassNames<ImagePart> = {
    root: "tm",
};

export const pagerClasses: ClassNames<PagerPart> = {
    root: "tp",
    more: "tp-more",
    busy: "tp-busy",
    error: "tp-error",
};

export const errorClasses: ClassNames<ErrorPart> = {
    root: "te",
    message: "te-message",
    requestId: "te-id",
};

export const consentClasses: ClassNames<ConsentPart> = {
    root: "tc",
    question: "tc-question",
    grant: "tc-grant",
    deny: "tc-deny",
    granted: "tc-settled",
    revoke: "tc-revoke",
};

export const chartClasses: ClassNames<ChartPart> = {
    root: "tk",
    figure: "tk-figure",
    series: "tk-series",
    point: "tk-point",
    hit: "tk-hit",
    tick: "tk-tick",
    table: "tk-table",
    row: "tk-row",
    current: "tk-current",
};
