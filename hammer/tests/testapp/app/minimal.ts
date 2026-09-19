// The application `docs/02-getting-started.md` walks through, and the reason
// that document is no longer prose.
//
// Every TypeScript block in the guide is a REGION of this file or of one beside
// it, transcluded by `tools/check-docs.sh` and diffed on every `npm run lint`.
// Before that existed, the guide's examples were paragraphs written next to a
// compiled testapp and believed to be compiled BY it: four of them described an
// API this library has never had, and the first application to consume hammer
// found them by trying to follow along.
//
// So the shape of this file is decided by the document rather than by the
// suite. `client.ts`, `state.ts` and `screen.ts` are the exhaustive proof that
// every seam is satisfiable; this is the narrow path through them that a person
// reads first, written so that each region stands on its own when it is lifted
// out of its surroundings.

import type { Copy } from "hammer";
import type { Capability, Client, FetchLike } from "hammer/wire";
import { createClient } from "hammer/wire";
import type { Inbox, FormReasons, MediaConfig, SessionStore } from "hammer/state";
import { Form, definitionsFrom, imageSources } from "hammer/state";
import type { BellCopy, FormCopy, Mounted } from "hammer/dom";
import { renderBell, renderForm, renderImage } from "hammer/dom";

import type { Api } from "../api/hammer.generated.js";
import {
    kApiTables,
    kFieldTypes,
    kPageLimitMax,
    kUploadMaxBytes,
    routeAuthRefresh,
    routeContentDelete,
    routeContentGet,
    routeContentPreview,
    routeMediaList,
    sectionHomeAbout,
} from "../api/hammer.generated.js";
import { bellClasses, formClasses, imageClasses } from "./classes.js";
import { copy } from "./copy.js";
import type { ErrorCode, Locale, ValidationReason } from "../api/hammer.generated.js";
import type { InboxPayload } from "./state.js";

// #region the-copy-table
// src/app/copy.ts
//
// `Record`, never `Partial<Record>`, and that is the whole of its value: an
// error code or a validation reason added server-side is a compile error here
// until somebody writes the words for it. The alternative is a fallback string,
// and a fallback string is how `BAD_FORMAT` reaches a reader in an interface
// that is otherwise entirely in Arabic.
export type AppCopy = Copy<Locale, ErrorCode, ValidationReason>;

// One half of it, in one locale, so that the shape is visible. The members are
// anvil's own names, untranslated on the way in, so the word on the screen is
// the word in the log somebody is reading it against — and `Unknown` is the one
// hammer adds, for a code the server appended after this bundle was built.
export const reasonWords: Record<ValidationReason, string> = {
    REQUIRED: "This is required.",
    TOO_SHORT: "That is too short.",
    TOO_LONG: "That is too long.",
    BAD_FORMAT: "Check the format.",
    BAD_CHARSET: "Some of those characters are not allowed.",
    OUT_OF_RANGE: "That is outside the allowed range.",
    NOT_ALLOWED: "That value is not allowed.",
    BAD_CHECKSUM: "Check the digits.",
    WEAK: "Choose something harder to guess.",
    BREACHED: "That password has appeared in a breach.",
    Unknown: "That value was not accepted.",
};
// #endregion

// #region build-the-client
// src/app/client.ts
export function buildClient(platform: {
    readonly apiOrigin: string;
    readonly fetch: FetchLike;
    readonly sessions: SessionStore;
}): Client<Api> {
    return createClient<Api>({
        // Every table the decode needs, as one value from the generated module.
        // They describe one descriptor and they travel as one thing, so a client
        // cannot decode with a rate-limit table from one regeneration and routes
        // from another.
        api: kApiTables,

        // The descriptor carries this route and nothing that marks it as the
        // refresh, so the application names it. One line, and a line whose
        // absence is a compile error rather than a 401.
        refreshRoute: routeAuthRefresh,

        // Unvalidated, and checked here rather than by you: a cross-site API is
        // not a degraded client but a session that cannot exist, because
        // `SameSite=Lax` cookies are not sent on a cross-site subresource
        // request at all. `null` rather than absent — a same-origin deployment
        // saying so is not the same as one that forgot to.
        origin: {
            pageOrigin: window.location.origin,
            apiOrigin: platform.apiOrigin,
            site: null,
        },

        fetch: platform.fetch,
        session: platform.sessions.source,
    });
}
// #endregion

// No token is passed, stored or read anywhere above. anvil's credentials are
// `__Host-` cookies with `HttpOnly` and hammer cannot see them, which is the
// property being bought rather than a limitation being worked around.

// #region call-something
// A page of a list. `after` is where the server said the last page ended, and
// there is no offset to pass — hammer has no way to express one.
export async function listMedia(
    api: Client<Api>,
    where: { readonly ns: string; readonly id: string },
    after: string | null,
    locale: Locale,
    signal: AbortSignal,
): Promise<readonly { readonly id: string }[] | null> {
    // The ROUTE VALUE, not its id. A string would be a name the compiler cannot
    // check and a module the bundler cannot shake; the value carries the method,
    // the bucket and the idempotence with it.
    const page = await api.call(routeMediaList, {
        params: where,
        query: { limit: kPageLimitMax, after },
        signal,
    });

    if (!page.ok) {
        // One decode site, one union — and `code` is only on the failures that
        // came from the server. A transport failure and a locally shed request
        // are the other two shapes, and they have no code because no server
        // answered.
        show(
            page.error.kind === "server"
                ? copy[locale].errors[page.error.code]
                : copy[locale].errors.Unknown,
        );
        return null;
    }

    return page.value.items;
}
// #endregion

// #region a-destructive-route
// A capability is minted by a call and by nothing else, so "the person
// confirmed this" is discharged by the compiler rather than by a reviewer.
export async function deleteContent(
    api: Client<Api>,
    id: string,
    signal: AbortSignal,
): Promise<boolean> {
    // The token is somewhere in a response whose shape is this application's, so
    // this application says where. hammer never mints implicitly in response to
    // `CAPABILITY_REQUIRED`: the scope exists so that a destructive action is a
    // second, deliberate act.
    const granted = await api.mint<typeof routeContentPreview, "ContentDelete">(
        routeContentPreview,
        { params: { id }, capability: capabilityFromConfirmation(), signal },
        (body) => (body as { readonly token?: unknown }).token,
    );
    if (!granted.ok) {
        return false;
    }

    // `content.delete` will not compile without this value. Its parameter type
    // requires a `Capability<"ContentDelete">` and the call above is the only
    // producer of one.
    const gone = await api.call(routeContentDelete, {
        params: { id },
        capability: granted.value,
        signal,
    });
    return gone.ok;
}
// #endregion

// #region a-form
// The field rows are the descriptor's and the bounds are the ones the server
// validates against, in CODE POINTS, so the two cannot disagree at the boundary.
export function aboutForm(): Form<ValidationReason> | null {
    // Which stored type a section control writes into is a product decision, so
    // the bridge between the two closed sets is the application's and is total
    // by type.
    const built = definitionsFrom(sectionHomeAbout.fields, {
        Text: kFieldTypes.TEXT_SHORT,
        RichText: kFieldTypes.TEXT_LONG,
        Choice: kFieldTypes.SELECT_SINGLE,
    });
    if (!built.ok) {
        return null;
    }

    // hammer refuses to ship the words. A reason is a member of an append-only
    // enum server-side; the sentence for it belongs to whoever knows the
    // audience and the locale.
    const reasons: FormReasons<ValidationReason> = {
        required: "REQUIRED",
        tooLong: "TOO_LONG",
        badFormat: "BAD_FORMAT",
        notAllowed: "NOT_ALLOWED",
    };
    return new Form<ValidationReason>({ fields: built.value, reasons });
}

export function mountForm(
    into: Element,
    form: NonNullable<ReturnType<typeof aboutForm>>,
    locale: Locale,
    words: FormCopy,
): Mounted {
    return renderForm(into, {
        form,
        // A label per field, and it cannot be omitted: an unlabelled control is
        // a defect, and one shipped from a library is shipped to every consumer
        // at once.
        fields: [{ key: "title", label: "Headline" }],
        classes: formClasses,
        copy: words,
        reasons: copy[locale].reasons,
        onSubmit: () => undefined,
    });
}
// #endregion

// #region an-image
// Bytes never enter the JS heap: an image is an `<img>` pointed at the media
// origin, the widths come from the namespace table, and format negotiation is
// `Vary: Accept` — the browser's job, not a branch in the client.
export function mountImage(into: Element, media: MediaConfig, id: string): Mounted | null {
    const sources = imageSources(media, { ns: "content", id });
    if (!sources.ok) {
        return null;
    }
    return renderImage(into, {
        sources: sources.value,
        alt: "A harbour at dawn",
        // Both, always: without them every image on the page is a layout shift.
        width: 320,
        height: 240,
        classes: imageClasses,
    });
}
// #endregion

// #region an-upload
// The size and the type are refused before the first byte, which saves an
// upload that was going to be refused after the last one. It is not the
// enforcement — anvil enforces during the read, because a client-side check is
// one an attacker skips.
export async function uploadTo(
    api: Client<Api>,
    where: { readonly ns: string; readonly id: string },
    file: File,
    signal: AbortSignal,
): Promise<boolean> {
    const sent = await api.upload(
        // Whichever route your descriptor declares for it. The reference
        // descriptor declares none — anvil's example application has no upload
        // route — so this names the media route it does have, which is enough to
        // show the shape and is a row in `docs/15-tasks.md` §Cross-repo rather
        // than something to be comfortable about.
        routeMediaList,

        // The cap is the descriptor's. The accept list is this application's:
        // anvil has no table of the media types a namespace takes to emit, so
        // this is a second copy of something the server already enforces.
        { maxBytes: kUploadMaxBytes, accept: ["image/jpeg", "image/png"] },
        {
            // A handle, not bytes. The body streams from the `File` itself, and
            // the size check happens against `file.size` before anything is
            // read — `readAsArrayBuffer` on a 40 MB video is 40 MB of tab memory
            // and a frozen main thread.
            file,
            params: where,
            onProgress: ({ sentBytes, totalBytes }) => report(sentBytes / totalBytes),
            signal,
        },
    );
    return sent.ok;
}
// #endregion

// #region the-bell
export function mountBell(
    into: Element,
    inbox: Inbox<Api, InboxPayload>,
    markRead: (ids: readonly string[]) => void,
    words: BellCopy,
): () => void {
    const bell = renderBell<InboxPayload>(into, {
        // The READ side. The bell cannot write the store, and the stream it
        // reads is neither opened nor closed here: it is leader-owned across
        // every tab on one session, and a bell unmounting would otherwise take
        // the connection out from under an inbox screen.
        inbox: inbox.store,

        markRead,

        // The sentence about a notification is this application's, and so is
        // which topics deserve one at all.
        describe: (item) => item.body.subject,

        classes: bellClasses,
        copy: words,
    });

    // Closing is not optional. It unsubscribes, aborts and releases everything
    // the component opened, and a tab stays open for days.
    return bell.close;
}
// #endregion

// --- what the regions above lean on, and the guide does not show -------------
//
// Each of these stands in for something an application has and hammer does not
// ship: a surface that renders a sentence, a progress indicator, a confirmation
// flow that produced a preview capability. They are here so that every region
// compiles as written rather than as an excerpt with an ellipsis in it.

declare function show(sentence: string): void;
declare function report(fraction: number): void;
declare function capabilityFromConfirmation(): Capability<"DraftPreview">;

// The one route the guide reads a document from, so that the response shape it
// narrows is declared somewhere the compiler can see it.
export function readContent(api: Client<Api>, id: string, signal: AbortSignal) {
    return api.call(routeContentGet, { params: { id }, signal });
}
