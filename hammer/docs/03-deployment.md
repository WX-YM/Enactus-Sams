# 03 — Deployment

Four properties in this library are not properties of the code at all. They are properties of
how the built assets are served, and each of them passes every test in this repository while
being wrong in production:

| | What it is | What it costs to get wrong |
|---|---|---|
| §1 | An entry point whose **existence** is sensitive, served from a public location | One `curl` and an anonymous visitor has the surface |
| §2 | A published source map | Everything tree-shaking removed, handed back |
| §3 | A CSP with `unsafe-inline` | The one policy the DOM suite is written against, absent |
| §4 | Cache headers on the bundle and on the session | A tab running last week's client, or a private response in a shared cache |

[`00-architecture.md`](00-architecture.md) §8 lists the invariants the *code* is allowed to
rely on. This document is the other half: what the person with the Nginx configuration has to
do so that those invariants are true.

Everything below assumes anvil behind Nginx, which is the deployment anvil documents
([anvil `docs/19`](../../anvil/docs/19-server-side-rendering.md)). Nothing here is specific to
Nginx beyond the syntax.

---

## 1. An asset whose existence is sensitive is a protected resource

This is the part of [`01-seams.md`](01-seams.md) §4 that codegen cannot finish.

A **holder** route's method and path are never emitted into a bundle: they arrive with the
session, scoped by the server to the permissions the holder actually has. So a chunk fetched
by an anonymous visitor contains no privileged address. What it *does* contain — if that
visitor can fetch it — is the route **id** and the **call site**, because those live in
whichever chunk calls them.

For almost every surface that is nothing: `content.publish` names a capability the server
enforces, and knowing the name buys an attacker a 404. But where the *existence* of a feature
is itself the sensitive fact — a moderation tool, an impersonation route, a surface that has
not been announced — the id and the call site are the disclosure, and no amount of codegen
removes them, because the code has to be somewhere.

> **A lazily-loaded chunk is a public URL.** Code-splitting decides which bundle a *browser*
> downloads. It decides nothing about who can read it.

The answer is deployment. Build that surface as its own entry point, put it in a directory the
public root does not cover, and serve it through `auth_request`:

```nginx
# Everything an anonymous visitor may have. Content-hashed, so it is immutable
# and may be cached by anything that can reach it.
location /assets/ {
    root      /srv/app/public;
    add_header Cache-Control "public, max-age=31536000, immutable" always;
}

# The one entry point whose EXISTENCE is the sensitive fact. Not in
# /srv/app/public at all — an asset that is only "not linked to" is an asset
# served to whoever guesses the name, and the names are content hashes in a
# manifest the application fetched.
location /assets/private/ {
    auth_request /internal/authorize;
    root      /srv/app/private;

    # `private`, and never `immutable`. A shared proxy that cached this would
    # serve it to the next request that arrived without a session, which is the
    # control being bought, undone by a header.
    add_header Cache-Control "private, no-store" always;
}

# The subrequest. anvil answers it from the session cookie and nothing else:
# 200 to proceed, 401 or 403 to refuse. No body goes up and no body comes back.
location = /internal/authorize {
    internal;
    proxy_pass              http://anvil;
    proxy_pass_request_body off;
    proxy_set_header        Content-Length "";
    proxy_set_header        X-Original-URI $request_uri;
}
```

Three notes, each of which has been got wrong somewhere:

- **The refusal must be the refusal anvil would have given.** If the asset's existence is
  sensitive, `401` on a path that exists and `404` on a path that does not are two
  distinguishable answers, and the difference is the oracle anvil's stealth 404 spends a whole
  design removing (`00-architecture.md` §6). Map the subrequest's `401`/`403` to the same
  `404` body the rest of the origin serves:

  ```nginx
  error_page 401 403 = @notfound;
  ```

- **It costs one subrequest per asset request, and that is the right price.** The asset is
  fetched once per deploy per browser; the subrequest is a cookie check anvil already performs
  on every API call.

- **It is authorization on the asset, not on the feature.** The server's own checks on every
  route that surface calls are unchanged and still the control. This location block stops an
  anonymous `curl`. It stops nothing else, and it is not a reason to relax anything on the
  other side.

### What this does not buy

**Path secrecy is defence in depth and never a boundary.** A staff user's browser holds that
user's paths, and so does anything running inside it. Everything above narrows who learns that
a surface exists; anvil's checks are what decide whether calling it does anything.

---

## 2. A privileged bundle publishes no public source map

A source map carries each module's **full contents** — including every branch the minifier
removed and every export tree-shaking dropped. A build that carefully emits no holder route
into the bundle and then publishes the map beside it has published the generated module in
full. `tests/codegen/built_output.test.ts` asserts exactly this, on the reference
application's own staff bundle: a constant that is absent from the code is present in the map.

So, for the application:

- **Do not serve `.map` alongside a privileged bundle.** Either build without one, or upload it
  to the error reporter out of band and serve it from nowhere.
- If maps are served for the public bundle, serve them from the same protected location as
  §1 for anything that is not public, or not at all.
- `//# sourceMappingURL=` is a *comment*, and removing the file without removing the comment
  points a browser's devtools at whatever answers that path.

And for this library: **hammer's own package publishes no source map**, enforced by
`tools/check-build-output.sh`, which builds and then reads `dist/`. The reason is one step
removed and worth stating, because "it is only the library's source" is the argument that
would otherwise win: a bundler that finds hammer's map folds it into the *application's* map,
and the application's map is the one this section is about.

---

## 3. The CSP the DOM suite is written against

hammer sets no inline style and no inline script (`CLAUDE.md` §5), and its DOM suite runs under
tripwires on every route to one. That work buys nothing unless the policy is actually served:

```
Content-Security-Policy:
    default-src 'self';
    script-src 'self';
    style-src 'self';
    img-src 'self' https://media.example;
    connect-src 'self';
    frame-src https://content.example;
    object-src 'none';
    base-uri 'none';
    form-action 'self';
    frame-ancestors 'none';
    require-trusted-types-for 'script';
    trusted-types hammer default;
```

- **No `unsafe-inline` and no `unsafe-eval`.** A component that sets `style.cssText` from data
  passes every suite here and is silently inert in production; the policy is what makes that a
  build failure rather than a support ticket.
- **`trusted-types hammer default`, and both names are load-bearing.**

  `hammer` is the policy the sanitiser creates for the one place this library turns a string
  into markup. `DOMParser.parseFromString(..., "text/html")` is a Trusted Types sink, which
  this library's own source denied until a browser was pointed at it — so a page that enforces
  `require-trusted-types-for 'script'` without naming this policy throws on **every rich-text
  render**, and it throws only on the deployment that was careful enough to enforce the policy
  at all. It is the sharpest argument in this document for the browser run existing: every
  unit test in the repository passed while it was broken, because no fake document enforces a
  policy.

  `default` is the refusing policy `installTrustedTypes()` installs. It is an explicit call in
  the application, never a side effect of an import: a library that installed a throwing policy
  on load would break the application's own markup from a decision it never made. What it buys
  is that every OTHER sink on the page — an accidental one, a dependency's, a pasted snippet's
  — fails loudly rather than working quietly until the day a CSP is enforced. Where the browser
  has no Trusted Types the call reports `unavailable`, which is not an error: that browser is
  not being served the enforcing half either.
- **`img-src` names the media origin** and `frame-src` names the content origin, because that
  is the only shape in which hammer touches either (§5).
- **`base-uri 'none'`.** An injected `<base>` re-points every relative URL on the page,
  including the ones the generated route builder produced.

---

## 4. Cache headers, and the tab that is older than the server

### The bundle

Content-hashed assets are `immutable` and cached forever; the HTML that names them is not
cached at all. The failure mode of getting this backwards is a browser holding an `index.html`
that points at a chunk the deploy deleted, which is a white screen nobody can reproduce.

```nginx
location = /index.html {
    add_header Cache-Control "no-cache" always;   # revalidate, do not guess
}
```

`no-cache` and not `no-store`: the browser keeps the copy and revalidates it, so an unchanged
deploy costs a `304` rather than the whole document.

### The session response

**Never `public`.** It carries the permission set and the holder-scoped route table, and a
shared cache that held it would serve one person's map to the next.

Its `ETag` is keyed to anvil's `perm_epoch`, so a grant reaches the table at the next
revalidation rather than at the next login — which only works if the revalidation is allowed
to happen:

```nginx
location = /api/session {
    proxy_pass http://anvil;
    add_header Cache-Control "private, no-cache" always;
}
```

### The deploy that lands while a tab is open

A tab open across a deploy is running last week's client against this week's server. The
session response carries the descriptor hash the server was built from, and a mismatch sets
`staleClient` on the session store (`00-architecture.md` §7.1).

**hammer never reloads the page by itself**, so nothing here is automatic. The application
decides what to do with the flag. What deployment owes it is that the previous build's assets
stay reachable long enough for the open tab to finish what it is doing — a deploy that deletes
the old chunks breaks every tab that has not navigated yet, and the ones it breaks are the ones
belonging to people in the middle of typing something.

---

## 5. The three origins

`00-architecture.md` §1 has the table and the reasoning. What deployment owes each of them:

| Origin | Serves | Must never |
|---|---|---|
| `SITE_ORIGIN` | the application and the API, **same origin, not merely same site** | be a different origin from the API. hammer refuses to be constructed against a cross-site one, because `SameSite=Lax` means the session is already gone |
| `CONTENT_ORIGIN` | HTML assembled from partially-trusted content | share a registrable domain with the site origin, or be `fetch`ed by the application. It is navigated to or framed |
| `MEDIA_ORIGIN` | image and file bytes | receive a credential, be in anvil's CORS allow-list, or issue a state change |

The one an otherwise-correct deployment gets wrong: **`CONTENT_ORIGIN` must be a different
registrable domain, not a subdomain.** A subdomain shares the site's cookie jar for anything
scoped to the parent domain, and the split exists so that an XSS in authored content executes
somewhere holding no session.

---

## 6. The checklist

- [ ] The API and the application are the **same origin**.
- [ ] `CONTENT_ORIGIN` is a different registrable domain; `MEDIA_ORIGIN` receives no credential.
- [ ] Any entry point whose existence is sensitive is behind `auth_request`, refusing with the
      same `404` the rest of the origin serves (§1).
- [ ] No `.map` is reachable for any bundle that is not public (§2).
- [ ] The CSP carries no `unsafe-inline` and no `unsafe-eval`, names the media and content
      origins, and permits **both** Trusted Types policies — `trusted-types hammer default`
      (§3). Missing `hammer` breaks every rich-text render, and only under enforcement.
- [ ] Hashed assets are `immutable`; `index.html` is `no-cache`; the session response is
      `private` (§4).
- [ ] The previous build's assets survive the deploy long enough for an open tab (§4).
- [ ] The hammer being deployed passed `npm run check:production` on a clean `npm ci`: no
      runtime dependency, no tolerated dev package, no open advisory (`CLAUDE.md` §12). A
      warning from `npm run check` is fine for development and is a refusal here. The same gate
      runs in the SDK's release, alongside anvil's.
- [ ] `Retry-After` is served on `429` and `503` — anvil does this in one place, and
      `wire/retry.ts` honours it exactly and invents nothing.
