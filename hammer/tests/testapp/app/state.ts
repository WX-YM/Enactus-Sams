// The reference consumer's state layer, and the proof that its seams are
// satisfiable from outside hammer.
//
// Everything hammer needs that it cannot know — where the identity is in a
// session body, which bounded class a route's entries live in, how the server
// spells a page's limit and cursor, where a worker module lives, where an
// analytics batch goes — is supplied here, from the generated module and from
// the deployment (`docs/01-seams.md` §18). If any of it could not be supplied
// from an application, it would fail HERE, at type-check time, which is the only
// place it can fail cheaply.

import type { Uuid } from "hammer";
import type { Client, ExclusiveLocks, FanOut, FetchLike } from "hammer/wire";
import { beaconFrom } from "hammer/wire";
import type {
    CacheConfig,
    Consent,
    CountSink,
    Identity,
    MediaConfig,
    Page,
    WorkerFactory,
} from "hammer/state";
import {
    AnalyticsSink,
    Form,
    Inbox,
    ImagePool,
    Invalidator,
    Pager,
    ResourceStore,
    Sections,
    SessionStore,
    definitionsFrom,
    imageSource,
    imageSources,
    optimistic,
    writeVersioned,
} from "hammer/state";
import type { FieldDefinition, FieldTypeSpec, FormReasons } from "hammer/state";
import type { PrehashBounds } from "hammer/prehash";
import { Argon2Pool } from "hammer/prehash";
import { Accounts, accountCall } from "hammer/accounts";

import type { RouteId, SectionFieldType, ValidationReason } from "../api/hammer.generated.js";
import {
    kFieldTypes,
    kMediaDefaultRole,
    kMediaWidths,
    kPageLimitMax,
    kPermissionBits,
    kAccounts,
    kTablesHash,
    eventProjectViewed,
    eventSignupCompleted,
    routeAuthLogout,
    routeContentGet,
    routeIdentityMe,
    routeMediaList,
    routeMediaObject,
    routeSessionCurrent,
    sectionHomeAbout,
} from "../api/hammer.generated.js";
import { invalidations } from "./invalidate.js";
import type { Api } from "./client.js";
import { client } from "./client.js";

// --- what an application decides about its own caches -----------------------
//
// The names are this application's and so are the numbers. hammer's only
// requirement is that a class exists and that it has a ceiling: a tab stays open
// for days, and a forgotten class gets the default rather than no bound at all.
const kCache: CacheConfig = {
    classes: {
        // One row per screen, so a handful is plenty.
        document: 32,
        // Sections carry words in every declared locale, so they are large and
        // there are few of them.
        section: 8,
        // A paged list evicts fastest and matters least once it is off screen.
        listing: 16,
    },
    defaultMaxEntries: 8,
};

// `MEDIA_ORIGIN`, and the public media grammar as the generated route.
//
// The pattern used to be written out here, because the reference descriptor
// carried no route for it — a path this application kept a second copy of, and a
// row in `docs/15-tasks.md` §Cross-repo rather than something to be comfortable
// about. anvil describes `media.object` now, so the copy is gone: the address
// comes from the same table as every other address, and the route builder still
// does the encoding.
const kMedia: MediaConfig = {
    origin: "https://media.example.com",
    route: routeMediaObject,
    widths: kMediaWidths,
    defaultRole: kMediaDefaultRole,
};

// A section field's `type` is a CONTROL — `Text`, `RichText`, `Choice` — and not
// a member of the field-type table. They are two closed sets in the descriptor
// and the bridge between them is a product decision: which stored type a rich
// text control writes into is an application's answer, not a library's. So the
// map is here, total by type, which makes the run-time refusal in
// `definitionsFrom` unreachable from a typed call site.
const kSectionControls = {
    Text: kFieldTypes.TEXT_SHORT,
    Number: kFieldTypes.NUMBER,
    RichText: kFieldTypes.TEXT_LONG,
    Choice: kFieldTypes.SELECT_SINGLE,
    Url: kFieldTypes.TEXT_SHORT,
    Color: kFieldTypes.TEXT_SHORT,
    Bool: kFieldTypes.SELECT_SINGLE,
} as const satisfies Record<SectionFieldType, FieldTypeSpec>;

// The words hammer refuses to ship. A reason is a member of an append-only enum
// server-side; the sentence for it belongs to whoever knows the audience.
const kFormReasons: FormReasons<ValidationReason> = {
    required: "REQUIRED",
    tooLong: "TOO_LONG",
    badFormat: "BAD_FORMAT",
    notAllowed: "NOT_ALLOWED",
};

// What a salt answer may ask of this application's devices. The reference
// server asks 64 MiB at three passes — plain mode's own cost, so what a guess
// against a leaked database costs is unchanged — and this application accepts
// down to OWASP's floor for Argon2id and up to twice the server's ask. Refusing
// outside it surfaces a misconfigured server rather than hashing whatever it
// named. The numbers are this application's: hammer ships none.
const kPrehashBounds: PrehashBounds = {
    minMemoryKib: 19456,
    maxMemoryKib: 131072,
    minIterations: 2,
    maxIterations: 6,
    maxParallelism: 4,
};

export type Platform = {
    readonly fetch: FetchLike;
    readonly locks: ExclusiveLocks | null;
    readonly fanOut: FanOut;
    readonly pageOrigin: string;
    readonly apiOrigin: string;

    // A worker module this application's bundler produced. hammer cannot name
    // one: `new Worker(new URL(...))` is a bundler contract and this library has
    // no bundler.
    readonly imageWorker: WorkerFactory;

    // The worker that runs Argon2 for a sign-in: this application's bundle of
    // `serveArgon2Pool(self)` (`./prehash_worker.ts`), for the same reason.
    readonly prehashWorker: WorkerFactory;

    // `navigator`, for the one send that survives the page going away.
    readonly beaconTo: { readonly sendBeacon: (url: string, data: BodyInit) => boolean };

    readonly count: CountSink;
};

export type AppState = {
    readonly api: Client<Api>;
    readonly session: SessionStore;
    readonly resources: ResourceStore<Api>;
    readonly invalidator: Invalidator<RouteId>;
    readonly sections: Sections<Api, typeof routeIdentityMe>;
    readonly inbox: Inbox<Api, InboxPayload>;
    readonly images: ImagePool;
    readonly accounts: Accounts;
    readonly analytics: AnalyticsSink;
    readonly close: () => void;
};

export type InboxPayload = { readonly topic: string; readonly subject: string };

export function appState(platform: Platform): AppState {
    // The session store is built first and told how to read a session AFTER the
    // client exists, because the client takes this store as its `SessionSource`
    // and the two would otherwise each need the other first.
    const session = new SessionStore({
        clientHash: kTablesHash,
        // anvil sends a holder's permissions as names in bit order, so the table
        // that turns them back into bits comes from here — it is the only place
        // that has it (`docs/01-seams.md` §3).
        permissionBits: kPermissionBits,
        // Where the identity is in a session body. The shape past the three
        // fields hammer decodes is this application's.
        identityOf: (body) => {
            const who = (body as { readonly user?: { readonly id?: unknown } }).user?.id;
            return typeof who === "string" ? who : null;
        },
        count: platform.count,
    });

    const api = client({
        origin: {
            pageOrigin: platform.pageOrigin,
            apiOrigin: platform.apiOrigin,
            // Null rather than absent: this deployment is same-origin and does
            // not need a registrable domain claim, and saying so is not the same
            // as forgetting to. `createClient` checks the pair and throws on one
            // it cannot drive.
            site: null,
        },
        fetch: platform.fetch,
        session: session.source,
        locks: platform.locks,
        fanOut: platform.fanOut,
        onLogout: () => {
            session.clear();
            resources.adopt(null);
            resources.clear();
            inbox.clear();
            inbox.close();
        },
    });

    // The session route, as the generated route.
    //
    // Its address used to be written out here with a `visibility: "public"`
    // override, because anvil described `session.current` as `authenticated` and
    // the generator therefore withheld its path — so the address of the session
    // arrived WITH the session, which on a cold load does not exist. anvil
    // describes it as public now, for the reason `auth.refresh` already was: a
    // route gated on the credential it exists to establish works only while it
    // is unnecessary.
    session.readsFrom((signal) => api.call(routeSessionCurrent, { signal }));

    const resources = new ResourceStore<Api>({
        client: api,
        cache: kCache,
        count: platform.count,
    });

    const invalidator = new Invalidator<RouteId>({
        table: invalidations,
        resources,
        fanOut: platform.fanOut,
        count: platform.count,
    });

    const sections = new Sections<Api, typeof routeIdentityMe>({
        resources,
        route: routeIdentityMe,
        // The query names are anvil's vocabulary, not hammer's.
        query: ({ key, stage }) => ({ section: key, stage }),
        class: "section",
    });

    const inbox = new Inbox<Api, InboxPayload>({
        client: api,
        route: routeIdentityMe,
        decode: (event) => {
            try {
                const body = JSON.parse(event.data) as Record<string, unknown>;
                const topic = body["topic"];
                const subject = body["subject"];
                if (typeof topic !== "string" || typeof subject !== "string") {
                    return null;
                }
                return { topic, subject };
            } catch {
                return null;
            }
        },
        // Which topics deserve a badge is this application's decision.
        counts: (body) => body.topic !== "session.new_device",
        count: platform.count,
    });

    const images = new ImagePool({ create: platform.imageWorker, count: platform.count });

    // Every account flow, from the table the descriptor published: which
    // routes, which fields, and — because the reference server hashes on the
    // client, anvil's default — that a password never leaves this tab. What
    // this application supplies is the worker and what its devices can afford.
    // The pool spawns its worker on the first sign-in, not here, so a session
    // that never signs in never pays for one.
    if (kAccounts === null) {
        throw new Error("the reference descriptor declares its accounts");
    }
    const argon2 = new Argon2Pool({ create: platform.prehashWorker, count: platform.count });
    const accounts = new Accounts({
        table: kAccounts,
        call: accountCall(api),
        prehash: { pool: argon2, bounds: kPrehashBounds },
    });

    const analytics = new AnalyticsSink({
        deliver: async (batch, signal) => {
            const answered = await api.call(routeIdentityMe, { body: batch, signal });
            return answered.ok;
        },
        beacon: beaconFrom(platform.beaconTo, platform.apiOrigin + "/ingest"),
        flushAt: 20,
        maxBatch: 200,
        count: platform.count,
    });

    return {
        api,
        session,
        resources,
        invalidator,
        sections,
        inbox,
        images,
        accounts,
        analytics,
        close: () => {
            analytics.close();
            images.close();
            // Terminating the worker is what hands back a heap that just ran a
            // 64 MiB Argon2; an idle one keeps it until the collector gets round
            // to it.
            accounts.close();
            argon2.close();
            inbox.close();
            invalidator.close();
            resources.close();
            session.close();
            api.close();
        },
    };
}

// --- the seams, exercised ----------------------------------------------------

// Signing in, and registering. Both are one line: the flows are anvil's, and
// `hammer/accounts` drives them from the published table. What this
// application writes is only what it knows — here, which of its fields holds
// the identifier a person typed.
export type Credentials = {
    readonly email: string;
    readonly password: string;
};

export async function signIn(state: AppState, who: Credentials, signal: AbortSignal) {
    return await state.accounts.signIn({ identifier: who.email, password: who.password }, signal);
}

// anvil answers a new address and a taken one identically, so `ok` here means
// "accepted", never "created" — what the screen says next is this application's.
export type Registration = Credentials & {
    readonly username: string;
    readonly givenName: string;
};

export async function signUp(state: AppState, who: Registration, signal: AbortSignal) {
    return await state.accounts.register(
        {
            email: who.email,
            username: who.username,
            profile: { given_name: who.givenName },
            password: who.password,
        },
        signal,
    );
}

// Signing out, in the order that matters.
//
// `client.logout()` is local: it empties this tab and fans the news out to every
// other tab on the origin. It does NOT call a route, and it must not — the
// address of a sign-out is an application's, and a library that invented one
// would be guessing at the one call whose failure is a session that looks ended
// and is not.
//
// So the route goes first, and the local clear goes second and unconditionally.
// The order is the whole point. Clearing first would leave a revoked-looking tab
// in front of a session anvil still holds; skipping the clear when the call
// fails would leave a signed-in shell in front of a session that is gone, which
// on a shared device is one walk-away from a disclosure.
//
// The route is described as idempotent, and anvil means it: revoking a session
// that is already gone is the ordinary shape of a retried sign-out.
// Opening a session, and the second call that has to come with it.
//
// `session.load` gets the route table and the authority. It does NOT get an
// identity, because anvil's session response carries none — `append_reachable_routes`
// and `append_holder_authority` write a table and a set of names, and neither
// says who this is. `SessionStore`'s `identityOf` seam is for an application
// whose session body does carry one; against the reference application it finds
// nothing, and there is nothing for it to find.
//
// So the identity comes from `identity.me`, which is a described route, and
// `me.id` is typed `UuidText` because the descriptor said so. Then it is ADOPTED
// by the resource store, and that is the part this function exists for: every
// cache is keyed by the identity allowed to read it (`CLAUDE.md` §2.3), and a
// store that was never told who it is holding for is a store that renders one
// user's documents to the next person on a shared device. Adopting re-keys every
// live entry and drops everything cached under the previous identity.
//
// It is a second round trip on the bootstrap path and that is a real cost. What
// closes it is anvil's session response carrying the holder's id, which is a row
// in `docs/15-tasks.md` §Cross-repo — listed rather than worked around here,
// because the workaround is one an application would copy.
export async function openSession(state: AppState, signal: AbortSignal): Promise<Identity> {
    const view = await state.session.load(signal);
    if (view === null) {
        state.resources.adopt(null);
        return null;
    }

    const me = await state.api.call(routeIdentityMe, { signal });
    // A failed read is an ANONYMOUS cache rather than the previous identity's.
    // Keeping the old key because the new one could not be read is the disclosure
    // this whole mechanism exists to prevent, arrived at by being careful.
    const who = me.ok ? me.value.id : null;
    state.resources.adopt(who);
    return who;
}

export async function signOut(state: AppState, signal: AbortSignal) {
    const answered = await state.api.call(routeAuthLogout, { signal });
    state.api.logout();
    return answered;
}

// A read, with its bounded class named. The parameters are the route's own, so a
// typo is a compile error and a missing one is not spellable.
export function openContent(state: AppState, id: string) {
    return state.resources.open(routeContentGet, { params: { id }, class: "document" });
}

// A paged list. The limit is the descriptor's maximum for this route and there
// is no offset to pass — `after` is where the server said the last page ended.
export function mediaPager(state: AppState, where: { readonly ns: string; readonly id: string }) {
    return new Pager<Api, typeof routeMediaList, { readonly id: string }>({
        client: state.api,
        route: routeMediaList,
        params: where,
        limit: kPageLimitMax,
        readPage: (body): Page<{ readonly id: string }> | null => {
            const shape = body as {
                readonly items?: readonly { readonly id: string }[];
                readonly next?: string | null;
            };
            return Array.isArray(shape.items)
                ? { items: shape.items, next: shape.next ?? null }
                : null;
        },
        // The names the server reads the page by are anvil's vocabulary.
        query: ({ limit, after }) => ({ limit, after }),
    });
}

// A form over a section's own field rows, resolved against the generated
// field-type table.
export function aboutForm(): Form<ValidationReason> | null {
    const built = definitionsFrom(sectionHomeAbout.fields, kSectionControls);
    if (!built.ok) {
        return null;
    }
    const fields: readonly FieldDefinition[] = built.value;
    return new Form<ValidationReason>({ fields, reasons: kFormReasons });
}

// A read-modify-write that carries the version it read, and a conflict that is
// reconciled rather than retried.
export async function renameContent(
    state: AppState,
    id: string,
    title: string,
    signal: AbortSignal,
) {
    const resource = openContent(state, id);
    try {
        // The version belongs to the document ON SCREEN, so there has to be one
        // before the write is worth sending. A caller that skipped this would
        // get `no-version` back, which is the refusal doing its job rather than
        // a defect — but it is a refusal a screen cannot act on.
        await resource.ready(signal);
        return await writeVersioned({
            resource,
            versionOf: (document) => document.version,
            send: async (version) =>
                await state.api.call(routeContentGet, {
                    params: { id },
                    body: { title, version },
                    signal,
                }),
            signal,
        });
    } finally {
        resource.release();
    }
}

// An optimistic update, confirmed against the document the server returns.
export async function starContent(state: AppState, id: string, signal: AbortSignal) {
    const resource = openContent(state, id);
    try {
        await resource.ready(signal);
        return await optimistic({
            resource,
            apply: (current) => ({ ...current, starred: true }),
            commit: async (inner) => await state.api.call(routeContentGet, { params: { id }, signal: inner }),
            signal,
        });
    } finally {
        resource.release();
    }
}

// The address an image editor previews: the `hero` role of the source, wide
// enough for an editing surface and never the master.
export function editPreview(subject: { readonly ns: string; readonly id: string }) {
    return imageSource(kMedia, subject, "hero");
}

// Every source for one stored image, at every width the namespace serves.
export function contentImage(id: string) {
    return imageSources(kMedia, { ns: "content", id });
}

// An event whose dimensions are a closed set, so a typo is a compile error
// rather than a row the ingest path drops.
export function reportSignup(state: AppState, surface: "web" | "ios" | "android") {
    state.analytics.report(eventSignupCompleted, { surface });
}

// An event whose dimension is a foreign id rather than a member of a closed
// set. `project` is OPTIONAL — the screen may render before the id is known —
// and omitting it is how that is spelled: the sink never sends an empty
// string or a null for a column the row does not have yet.
export function reportProjectViewed(state: AppState, project?: Uuid): void {
    state.analytics.report(eventProjectViewed, project === undefined ? {} : { project });
}

export function answerConsent(state: AppState, answer: Consent): void {
    state.analytics.setConsent(answer);
}
