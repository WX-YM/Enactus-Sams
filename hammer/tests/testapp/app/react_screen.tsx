// Every hook in `hammer/react`, used by an application, at the same time.
//
// `tests/testapp/app/screen.ts` is this for the components; this is it for the
// adapter, and it exists for the same reason: type-checking a signature proves
// the seam can be satisfied one at a time, and a screen proves the seams can be
// satisfied together, by one consumer, in one construction order, with one set
// of tables.
//
// Nothing in this file is hammer's. Every word is `copy.ts`'s, every class name
// is `classes.ts`'s, every route is the generated module's, and every decision
// about what a person is allowed to do is the session's. What comes from the
// library is the lifetime: when the read opens, when it closes, when the write
// is aborted.

import type { ReactElement } from "react";
import { useCallback } from "react";

import { ok } from "hammer";
import type { Consent, ResourceFailure, VersionedOutcome } from "hammer/state";
import { optimistic, writeVersioned } from "hammer/state";
import { useForm, useInbox, useMutation, useResource, useSession, useStore } from "hammer/react";

import type { Locale, ValidationReason } from "../api/hammer.generated.js";
import { routeContentGet } from "../api/hammer.generated.js";
import { formClasses, sectionClasses } from "./classes.js";
import type { Api } from "./client.js";
import { componentCopy } from "./component_copy.js";
import { copy } from "./copy.js";
import type { AppState } from "./state.js";
import { aboutForm, openContent } from "./state.js";

// What this application's content route answers with. Declared once here rather
// than repeated at each component, and it is the augmented response type rather
// than a second description of it.
type Document = { readonly title: string; readonly starred: boolean; readonly version: number };
type Failure = ResourceFailure<Api>;

// This screen's own words, total over the locales the descriptor declares.
//
// `copy.ts` is the FAILURE vocabulary and `component_copy.ts` is the words
// hammer's own components put on a screen; neither has a word for a button this
// application invented, and neither should. A screen of an application's own is
// a third table for the same reason the first two are separate: a slot added
// here must not look like a slot added to either of those.
const kWords: Record<
    Locale,
    { readonly loading: string; readonly rename: string; readonly star: string }
> = {
    en: { loading: "Loading…", rename: "Rename", star: "Star" },
    ar: { loading: "جارٍ التحميل…", rename: "إعادة التسمية", star: "تمييز" },
};

export type ScreenProps = {
    readonly state: AppState;
    readonly locale: Locale;
};

// --- a read, and the two writes that go over it ------------------------------

export function ContentScreen(props: ScreenProps & { readonly id: string }): ReactElement {
    const { state, locale, id } = props;

    // The factory is stable over the address and nothing else. An inline arrow
    // here would release and re-open this entry on every render of the screen,
    // which is an aborted request per keystroke somewhere else on it
    // (`docs/01-seams.md` §20).
    const open = useCallback(() => openContent(state, id), [state, id]);
    const view = useResource<Document, Failure>(open);

    // The outcome is the value, not an error: a conflict is a thing that
    // happened and has words of its own, and flattening it into a failure would
    // lose the re-read document the reconciliation just fetched.
    const rename = useMutation<VersionedOutcome<Document, Failure>, Failure>();
    const star = useMutation<Document | null, Failure>();

    const words = copy[locale];
    const held = view.resource;

    return (
        <section className={sectionClasses.root} dir="auto">
            {view.state.status === "loading" ? (
                <p aria-busy="true">{kWords[locale].loading}</p>
            ) : null}

            {view.state.status === "failed" ? (
                // The code and never a message: anvil's error responses carry no
                // sentence, and the words for one are this table's.
                <p className={sectionClasses.value}>{words.errors[view.state.error.kind === "server" ? view.state.error.code : "Unknown"]}</p>
            ) : null}

            {view.state.value !== null ? (
                <p className={sectionClasses.value} dir="auto">
                    {view.state.value.title}
                </p>
            ) : null}

            {held !== null && view.state.status === "ready" ? (
                <p>
                    <button
                        type="button"
                        className={formClasses.submit}
                        disabled={rename.state.status === "running"}
                        onClick={() => {
                            void rename.run(async (signal) =>
                                ok(
                                    await writeVersioned<Api, Document, number>({
                                        resource: held,
                                        versionOf: (document) => document.version,
                                        send: async (version, inner) =>
                                            await state.api.call(routeContentGet, {
                                                params: { id },
                                                body: { title: "…", version },
                                                signal: inner,
                                            }),
                                        signal,
                                    }),
                                ),
                            );
                        }}
                    >
                        {kWords[locale].rename}
                    </button>

                    <button
                        type="button"
                        className={formClasses.submit}
                        disabled={star.state.status === "running"}
                        onClick={() => {
                            void star.run(async (signal) => {
                                const outcome = await optimistic<Api, Document>({
                                    resource: held,
                                    apply: (current) => ({ ...current, starred: true }),
                                    commit: async (inner) =>
                                        await state.api.call(routeContentGet, {
                                            params: { id },
                                            signal: inner,
                                        }),
                                    signal,
                                });
                                return outcome.kind === "rolled-back"
                                    ? { ok: false, error: outcome.error }
                                    : ok(outcome.document);
                            });
                        }}
                    >
                        {kWords[locale].star}
                    </button>
                </p>
            ) : null}

            {rename.state.status === "done" && rename.state.value.kind === "conflict" ? (
                <p role="status">{words.errors.VERSION_MISMATCH}</p>
            ) : null}
        </section>
    );
}

// --- who is signed in, and what that affords --------------------------------

export function SessionBadge(props: ScreenProps): ReactElement {
    const session = useSession(props.state.session);
    const words = copy[props.locale];

    // "Not answered yet" is not "signed out". Rendering the signed-out surface
    // before the first read has answered is a login form that flashes at
    // everybody who is already signed in (`docs/01-seams.md` §19).
    if (session.status === "unknown") {
        return <p aria-busy="true" />;
    }

    if (session.status === "anonymous") {
        return <p>{words.errors.UNAUTHENTICATED}</p>;
    }

    // The affordance is asked of the session's own table rather than counted out
    // of the bits: anvil's `satisfies()` short-circuits on user type, and a
    // superadmin's permission set is deliberately not all-ones.
    return (
        <p>
            {props.state.session.affords(routeContentGet) ? (
                <button type="button" className={formClasses.submit}>
                    {kWords[props.locale].rename}
                </button>
            ) : null}
        </p>
    );
}

// --- an at-least-once stream, counted ---------------------------------------

export function BellBadge(props: ScreenProps): ReactElement {
    const inbox = useInbox(props.state.inbox);
    // The count's WORDS are this table's, because a plural rule belongs to a
    // locale and not to a library.
    return <p aria-live="polite">{componentCopy[props.locale].bell.unread(inbox.unread)}</p>;
}

// --- a form the descriptor's own field rows built ---------------------------

export function AboutForm(props: {
    readonly form: NonNullable<ReturnType<typeof aboutForm>>;
    readonly locale: Locale;
}): ReactElement {
    const state = useForm<ValidationReason>(props.form);
    const words = copy[props.locale];

    return (
        <form className={formClasses.root}>
            {[...state.fields].map(([key, field]) => (
                <p key={key} className={formClasses.field}>
                    <label className={formClasses.label} htmlFor={`about-${key}`}>
                        {key}
                    </label>
                    <input
                        id={`about-${key}`}
                        className={formClasses.control}
                        value={typeof field.value === "string" ? field.value : ""}
                        onChange={(event) => props.form.set(key, event.target.value)}
                        onBlur={() => props.form.touch(key)}
                    />
                    {field.reason === null ? null : (
                        <span className={formClasses.error}>{words.reasons[field.reason]}</span>
                    )}
                </p>
            ))}
        </form>
    );
}

// --- consent, which is a store like any other -------------------------------

export function ConsentBadge(props: ScreenProps): ReactElement {
    // `useStore` rather than a hook of its own: the sink publishes a
    // `Readable<Consent>` and every other store an application holds is reached
    // the same way.
    const answer: Consent = useStore(props.state.analytics.consent);
    const words = componentCopy[props.locale].consent;
    return <p>{answer === "granted" ? words.granted : answer === "denied" ? words.denied : words.question}</p>;
}

// The whole screen, which is what a test mounts: every hook the adapter ships,
// in one tree, over one application state.
export function ReactScreen(props: ScreenProps & { readonly id: string }): ReactElement {
    const form = aboutForm();
    return (
        <main>
            <SessionBadge state={props.state} locale={props.locale} />
            <BellBadge state={props.state} locale={props.locale} />
            <ContentScreen state={props.state} locale={props.locale} id={props.id} />
            <ConsentBadge state={props.state} locale={props.locale} />
            {form === null ? null : <AboutForm form={form} locale={props.locale} />}
        </main>
    );
}
