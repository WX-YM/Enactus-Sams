# 02 — Getting started

A minimal application, end to end.

**Every TypeScript block below is lifted out of compiled source**, from
[`tests/testapp/app/minimal.ts`](../tests/testapp/app/minimal.ts) and the files beside it, by
[`tools/check-docs.sh`](../tools/check-docs.sh) — which diffs them on every `npm run lint` and
rewrites them with `--write`. A block that drifts from the code it names fails the build.

That mechanism exists because of what happened without it. This document used to open by saying
its examples "cannot rot" because they are compiled as `tests/testapp/`. They were not: the
testapp was compiled and these were paragraphs beside it, and four of them described an API this
library has never had — a `createClient` taking a route table and three origins, a `call` taking
a route id, a `useResource` taking a query, a `useSession` taking nothing. The first application
to consume hammer found them by trying to follow along. The sentence asserting that drift could
not happen is what stopped anybody checking, so it is now true rather than gone.

## 1. What you need

| | Minimum | Notes |
|---|---|---|
| An anvil application | current | Its tables are the source of every seam ([`01-seams.md`](01-seams.md)) |
| Node | 20 | Build time only. hammer ships nothing that needs it at run time |
| A bundler | any | It must honour `"sideEffects": false` and `exports`. Nothing else is required of it |

The application and the API must be served from **the same origin**
([`00-architecture.md`](00-architecture.md) §8.4). A cross-origin API costs a preflight on
every mutating request; a cross-site one does not work at all, and `createClient` throws rather
than be built against one.

## 2. Generate the client

```sh
# In your anvil application's build: emit the descriptor from your own tables.
./build/tools/emit-descriptor > web/hammer.descriptor.json

# In your web application: generate the typed client from it.
npx hammer codegen --descriptor hammer.descriptor.json --out src/api
```

That writes one file, `src/api/hammer.generated.ts`, and it is **committed**. It carries the
descriptor hash, and `tools/check-descriptor.sh` in your own repository regenerates and diffs
it so a client built from last month's server cannot ship.

One file, not one per audience. A public route's method and path are values; a privileged
route's are not emitted at all, because a lazily loaded chunk is a public URL and splitting
the module would have changed which bundle downloads the map rather than who can read it
([`01-seams.md`](01-seams.md) §4.1). Everything else about a route — its id, its parameters,
the capability it consumes — is a `type`, which costs nothing, and every table is an individual
`const`, so your bundle carries what it spells and no more.

## 3. The three tables hammer cannot generate

The descriptor carries what the server knows. Words, class names and screen knowledge are
yours ([`01-seams.md`](01-seams.md) §13). Each is checked against a generated type, so adding
a locale, an error code or a validation reason server-side is a **compile error here** until
it has an answer.

```ts tests/testapp/app/minimal.ts#the-copy-table
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
```

The full table, in every locale the descriptor declares, is
[`tests/testapp/app/copy.ts`](../tests/testapp/app/copy.ts). `NOT_FOUND` says the thing was not
found and nothing else: anvil answers a denied request on a stealth route with a byte-identical
404 so that a probe cannot distinguish a missing object from a forbidden one, and a client that
renders "you do not have permission" there hands back the oracle the server spent a whole design
removing.

```ts tests/testapp/app/invalidate.ts#invalidations
// src/app/invalidate.ts
export const invalidations = {
    "content.delete": ["content.get"],
    "media.delete": ["media.list"],
} as const satisfies Invalidations<RouteId>;
```

This one is **partial** where the other two are total, and deliberately: a read invalidates
nothing, so an entry per route would be a table of empty arrays whose only effect is to hide the
routes that matter. Both sides are still real route ids, so a mutation naming a resource the
server retired stops compiling rather than quietly ceasing to invalidate anything.

## 4. Build the client

`ClientConfig` has five required members. Four of them are things only your deployment knows;
the fifth is the generated module handing over every table the decode needs, as one value.

```ts tests/testapp/app/minimal.ts#build-the-client
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
```

No token is passed, stored or read. anvil's credentials are `__Host-` cookies with
`HttpOnly`; every request carries them because it is same-origin, and hammer cannot see them,
which is the property being bought ([`CLAUDE.md`](../CLAUDE.md) §5).

Everything past those five has a default — `queue`, `retry`, `breaker`, `locks`, `fanOut`,
`now`, `sleep`, `telemetry`, `onLogout`, `leaderWaitMs`. A single-tab application needs none of
them; `locks` and `fanOut` are what make the refresh single-flight across every tab on one
session (§5), and [`tests/testapp/app/client.ts`](../tests/testapp/app/client.ts) passes all
three.

**The client is not given a route table.** Routes are values you import and pass to `call`
(§5), which is what keeps the rest of the table out of your bundle: there is no aggregate of
the privileged tier to import by accident, and tree-shaking removes what you never name.

A privileged route's `const` carries its id, its permissions, its bucket and its idempotence,
and a **null** method and path. The path arrives with the session, scoped by the server to what
your permissions reach, and is compiled into no bundle and no source map
([`01-seams.md`](01-seams.md) §4.1). You call every route the same way; the resolution is the
client's business.

## 5. Call something

```ts tests/testapp/app/minimal.ts#call-something
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
```

A `403` is handled for you: the session is refetched, because the local permission copy is
stale by design ([`00-architecture.md`](00-architecture.md) §4.1). A `401` is handled for you
too — one refresh, in one tab, with the in-flight requests replayed after it.

## 6. A destructive route

```ts tests/testapp/app/minimal.ts#a-destructive-route
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
```

`content.delete` **will not compile** without the capability, because its parameter type
requires a `Capability<"ContentDelete">` and the only producer of one is `mint`. hammer never
mints implicitly in response to `CAPABILITY_REQUIRED`: the scope exists so that a destructive
action is a second, deliberate act, usually one the person confirmed.

## 7. A form

```ts tests/testapp/app/minimal.ts#a-form
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
```

Field types come from your table; validation bounds come from the same descriptor the server
validates against, in **code points**, so the two agree. The renderer ships labels, error
placement and ARIA wiring — and no styling, no strings and no field types of its own
([`CLAUDE.md`](../CLAUDE.md) §1, §9).

## 8. Images and uploads

```ts tests/testapp/app/minimal.ts#an-image
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
```

Bytes never enter the JS heap. An image is an `<img>` pointed at the media origin, never a
`fetch(...).blob()` and never a base64 `data:` URI: anvil serves those bytes with `sendfile()`
precisely so they never enter *its* heap, and decoding them into a JS `Blob` to display them
would put the whole file in the one heap with the least room, on the device least able to spare
it, and lose the CDN cache besides. The widths come from your namespace table and format
negotiation is `Vary: Accept` — the browser's job, not a branch in the client.

```ts tests/testapp/app/minimal.ts#an-upload
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
```

The size and the type are refused before the first byte, which saves an upload that was going
to be refused after the last one — on a connection where forty megabytes is minutes. It is not
the enforcement: anvil enforces during the read, because a client-side check is one an attacker
skips.

## 9. React

```tsx tests/testapp/app/minimal_react.tsx#react-a-read
function Content({ state, id }: { readonly state: AppState; readonly id: string }): ReactElement {
    // `useSession` takes the STORE. The hook subscribes to it; it does not find
    // one for you, because a library that reached for an ambient singleton would
    // be deciding your application's composition.
    const session = useSession(state.session);

    // `useResource` takes a function that OPENS a resource, not a route and a
    // query. Opening is yours — it is where the cache key, the invalidation set
    // and the decode live — and the hook supplies only the part React can: when
    // the subscription starts and when it stops.
    //
    // The factory is the identity that decides the lifetime, exactly as
    // `useSyncExternalStore`'s own `subscribe` argument is, and it carries the
    // same obligation: memoise it over the address. An inline arrow releases and
    // re-opens on every render, which is an aborted request per keystroke of
    // whatever else is on the screen.
    const open = useCallback(() => openContent(state, id), [state, id]);
    const view = useResource<Document, ResourceFailure<Api>>(open);

    // An affordance, never a control. `affordsRoute` reads the session's own
    // route table — the server built it with the same `satisfies()` its request
    // filter calls — so it cannot disagree with what the server will do.
    if (!affordsRoute(session.view, routeContentGet)) {
        return <></>;
    }

    // Where the affordance is NOT a route — a column of personal data, a bulk
    // action, a tab — the bits answer instead. `holdsAll` short-circuits on
    // superadmin, because anvil's held set is deliberately not all-ones and a
    // client that only counted bits would hide the whole application from the
    // one account that reaches all of it.
    const showPii = holdsAll(session.view, [kPermFormPii]);

    // `status` is "loading" | "ready" | "failed", and `value` is null until it
    // is ready.
    if (view.state.status !== "ready" || view.state.value === null) {
        return <p aria-busy={view.state.status === "loading"} />;
    }

    return (
        <p dir="auto" data-pii={showPii}>
            {view.state.value.title}
        </p>
    );
}
```

**Use `affordsRoute` where the affordance is a route, and the bits only where it is not.** The
session's route table is the server's own answer — anvil built it with the same `satisfies()`
its request filter calls — so the table cannot disagree with what the server will do, and
counting bits can: a superadmin's permission set is deliberately not all-ones, so a bitset
check would hide every guarded control from the one account that reaches all of them
([`01-seams.md`](01-seams.md) §4.1.1).

Neither can refuse a call, and not by convention: there is no error in the library meaning "a
permission denied this locally". Hide it, and if it is invoked anyway, send it and let the
server decide — a client that refuses locally turns a stale permission copy into a permanent
denial no server-side change can clear ([`00-architecture.md`](00-architecture.md) §4.1).

The adapter binds stores to a lifecycle and contains no logic of its own — which is why the
same behaviour is testable with no framework at all.

## 10. Components that hold their own state

```ts tests/testapp/app/minimal.ts#the-bell
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
```

The bell owns the hard parts: an unread count reconciled against an at-least-once stream and
against the server's own count, dedupe by event id, popover focus and focus return, and a live
region that announces an arrival without stealing focus from whatever is being typed. It owns
none of the words, none of the classes and none of your topic names — the split is
[`CLAUDE.md`](../CLAUDE.md) §9.

Every component in `hammer/dom` hands back the same handle:

```ts sketch: the shape every renderer returns, not a call site
renderX(mount: Element, options: XOptions): Mounted      // { element, close }
```

Closing is not optional. It unsubscribes, aborts and releases every resource the component
opened, and a component that is dropped without it is a leak in a tab that stays open for days.

## 11. What to read next

- [`00-architecture.md`](00-architecture.md) — the layers, the three origins, the request
  lifecycle, and the error table that says what the client does with every code.
- [`01-seams.md`](01-seams.md) — the descriptor, and every table it carries.
- [`03-deployment.md`](03-deployment.md) — the four properties that are true of how the assets
  are served rather than of the code, and pass every test here while being wrong in
  production.
- [`../CLAUDE.md`](../CLAUDE.md) — the rules the library holds itself to, and why each one
  exists.
