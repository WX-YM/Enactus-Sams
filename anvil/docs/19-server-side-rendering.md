# 19 — Server-side HTML

## 1. What this is, and the engine it is not

anvil emits HTML in one shape: **a function appends into one reserved `std::string`, escaping
every interpolated value at the point it is written.** That is the whole mechanism. There is no
template file, no grammar, no parser, no view catalogue, no page cache and no render program.

This document was first written the other way round — a boot-loaded template engine with six
constructs, three escaping contexts, a layout mechanism and a lint tool to check the markup.
Three arguments retired it, and none of them is about what any particular application happens
to need today.

**A template engine is a policy, and anvil ships mechanism.** A grammar commits every consumer
to one notation, one set of constructs and one evaluation order, forever — and it is the kind
of thing an application can build on top in an afternoon if its own markup wants it. The
escaping rules underneath cannot be built in an afternoon and cannot be got wrong twice, which
is the correct division: this library holds the part where a mistake is a vulnerability, not
the part where a mistake is a preference.

**Every construct in a grammar is a security surface that has to be re-argued.** An
interpolation needs a context; a context needs an escaper; a loop and a conditional need a
value model; an include needs a depth cap; a layout needs a slot resolution order. Each is a
place to be wrong, and the whole apparatus exists to compose strings — which C++ already does,
with the type system available to enforce the one rule that matters (§3).

**And a mechanism with no state has no failure modes to document.** No files on disk, no boot
parse, no cached program, no invalidation. That is also what dissolves the tension the earlier
draft spent a section arguing: ENGINEERING_RULES.md §1 wants what an application supplies to fail at
configure or compile time, and with nothing supplied at run time there is nothing to relax.

Corroboration rather than justification: an application built on this library server-renders
one page, with about seventy lines appending escaped fields into a reserved buffer and one
place where markup is inserted without escaping. That is a data point about how much of this
is usually needed, not the reason the decision went the way it did.

The engine is deferred rather than deleted (§8). What is here instead is the part that was
load-bearing all along and that the engine's design had missed: the escaping discipline, and
the envelope that contains what happens when it fails.

**anvil registers no routes here**, as everywhere. It ships the writers, the rule, and the header
set; a handler composes a page and returns it.

## 2. The escapers, and the contexts that do not exist

`http/html_writer.h`, beside `json_writer.h` and `csv_writer.h`, with the same shape all three
share: append into one caller-owned string after a single `reserve()`.

| | Context | Behaviour |
|---|---|---|
| `append_html_text` | element text | escapes `&`, `<` and `>`. `&` is handled first by construction — one byte at a time — so no double-escaping window exists |
| `append_html_attr` | a whole attribute | emits ` name="value"`: the space, the name, the `=` and both quotes. The escape set is the text set **plus both quote characters** |
| `append_url_attr` | a URL-valued attribute | the same, and it delegates the accept/reject decision to `input::is_safe_link_target`. Emits **nothing at all** when that says no, and returns `false` so the caller can render the text without the link |

The escape sets differ on purpose. In element text a quote is an ordinary character, and the
only reason to escape one would be a call shape that lands text inside an attribute — which the
attribute writer's own quoting removes.

`append_html_escaped` came here from `json_writer.h`, where it had sat since phase 1 under a
comment apologising for being there, and is not public any more. It escaped both quote
characters **without emitting them**, which is exactly the unquoted-attribute call shape this
section removes; it is now the internal attribute escaper. A consumer that called it moves to
`append_html_text` or `append_html_attr` depending on which context it was writing into.

And the two contexts that do not exist:

> A correct JavaScript escaper has to know whether it is inside a string literal, a template
> literal, a regex or a comment. anvil ships **no JS context and no CSS context** — not a
> partial one, not a best-effort one. Data reaches client JavaScript through a
> `<script type="application/json">` block written by `json_writer.h`, which is one escaper this
> library already has, already tests, and already keeps byte-predictable.

A page assembling partially-trusted content should serve a CSP with **no `script-src` at all**
(§7), and then the question of escaping into a script does not arise. That is the right order:
remove the context, then you do not need the escaper.

**`append_html_attr` is meaningless without quoting**, and nothing in a C++ append chain can
check that the surrounding bytes were a quoted attribute — a template loader could have, and
that was a genuine advantage of the engine. What replaces it is narrower and holds: the writer
emits the whole attribute, name and `=` and both quotes, so there is no call shape that produces
an unquoted one and none that forgets the `=`. It takes the name for a second reason too, which
only appeared once `append_url_attr` was written: a writer that can REFUSE has to be able to
leave out the attribute entirely, and a value-only writer that emitted nothing would leave a
dangling `href=` behind it.

The name is not escaped, because escaping would not help — `onload` is a perfectly well-formed
attribute name. A name outside `[A-Za-z0-9-]` emits nothing, which fails closed against the one
shape that would otherwise be an injection: an attribute name built from request data.

**`append_url_attr` does not implement a URL rule.** `input::is_safe_link_target` already exists,
is already exercised by the sanitiser, and already rejects the case that is easy to forget — a
protocol-relative `//evil.test/x`, which inherits the page's scheme and is an absolute off-site
link wearing a relative-looking prefix. A second implementation of "is this URL safe" is a second
one to keep in agreement, and the one that drifts is the one with no attacker reading it. The
writer calls it and emits nothing when it says no.

**Delegating the decision is not delegating the escape.** `is_safe_link_target` answers a
question about schemes and origins; `/x" onmouseover=alert(1)` is a site-relative path it
correctly accepts, and emitting that raw would close the attribute and open an event handler. An
accepted URL still goes through the attribute escape set, and a query string's `&` becomes
`&amp;` on the way out. That has its own case in the suite, because the mistake it prevents is
the plausible reading of the paragraph above it.

**The allow-list is site-relative, `https:` and `mailto:`, and `tel:` is not on it.** That is
defensible — every scheme on a list is one somebody has thought about — but its effect on a
consumer was easy to miss, because `append_url_attr` fails SILENTLY BY DESIGN: it emits nothing
and returns `false`. A handler that passed a `tel:` URL and ignored the return value rendered an
anchor with no `href`, which is not a link, is not focusable and is not announced as one. On a
contact page the symptom was that a phone number stopped being tappable on the device every
visitor is holding, and nothing anywhere said why.

The fix is NOT a scheme added to the list, and NOT a second URL rule written by an application
— the argument above applies unchanged, and the second implementation is the one that drifts.
It is §3's shape applied to a scheme instead of to markup: make the dangerous call impossible
to reach with the wrong thing.

```cpp
// anvil/locale_egy/phone_html.h — takes a number, never a string.
[[nodiscard]] bool append_tel_attr(std::string& out, const input::PhoneEgy& phone);
```

`input::PhoneEgy::e164` is thirteen bytes a scanner wrote, so the function cannot be handed a
request byte by a refactor that was not thinking about it. It builds the `tel:` URI itself in
automatic storage and emits through `append_html_attr`, which still owns the quoting and the
escape set — the escaping stays even though no byte that survives the check is in it, because
a writer that skips it on the grounds that "these bytes are safe" is one refactor from not
being.

Two things about it that are not the obvious reading:

- **It re-checks the value at the point of emission**, exactly as `append_sanitized` re-runs
  the sanitiser's verdict rather than arguing from the write path. The type narrows what can
  be passed; it cannot prove the value was filled. `PhoneEgy` is an aggregate behind an
  out-parameter API, so `PhoneEgy phone{};` compiles and has to — which means a caller who
  ignored the `Reason` is holding thirteen NUL bytes of a perfectly well-typed phone number,
  and `tel:` followed by thirteen NULs is an `href` worse than no `href` at all.
- **It takes no attribute name.** A `tel:` URI is an `href` and is nothing else, so there is no
  name to get wrong and no second failure mode where a bad name emits nothing while the call
  still reports success.

It lives in the locale module rather than beside `append_url_attr` because `PhoneEgy` is one
country's rules and `anvil::http` is not. A general `append_tel_attr(std::string_view)` would
be the URL rule again one layer down; the general form waits for a second validator to
generalise **from** rather than being guessed at from one.

## 3. Raw insertion is a type, not a convention

The single dangerous operation is appending markup that has not been escaped. It is spelled so
that it cannot be reached with a `string_view`:

```cpp
// The ONLY overload that appends markup verbatim. It does not take a string.
void append_sanitized(std::string& out, const input::SanitizedHtml& safe);
```

`input::SanitizedHtml` is produced by `input::sanitize_rich_text` and by nothing else
([`06-input-validation.md`](06-input-validation.md)). To insert raw markup you must hold a value
the sanitiser produced, so "a raw site" stops being a discipline anybody has to remember or a
comment anybody has to notice in review. It is the same trick the metrics API uses in
[`17-analytics.md`](17-analytics.md) §6: a function that cannot be handed a request byte cannot
be made to accept one by a refactor that was not thinking about it.

**That requires one change to `SanitizedHtml`, and the rule is worth nothing without it.** It is
an aggregate with public members, so `SanitizedHtml{attacker_controlled, Ok}` compiles and the
guarantee is decoration. The constructor is now private with the sanitiser as its only producer —
the shape `CapabilityScope` already uses, for the same reason: a value that asserts something
about its contents must not be constructible by anyone who has not done the work. The bytes come
out through `html()` for a reader and `into_html()` for a caller about to store them, and the
latter is rvalue-qualified, so a value that has been emptied cannot then be emitted.

The rule is tested by proving the type system enforces it: two `static_assert`s, one that
`append_sanitized` is not invocable with a string and one that a `SanitizedHtml` is not
constructible from arbitrary bytes. The second is the one that matters, because without it the
first is a speed bump.

This is also why there is no `AllowsRaw` flag, no raw-site table and no boot-time count of raw
sites. All three were machinery for auditing a rule by hand; with the constructor closed, the
rule is checked at every call site by the compiler, and machinery for auditing it is machinery
for auditing something that cannot happen.

## 4. Re-sanitise at render

`append_sanitized` takes the verdict as well as the bytes, and **a value whose verdict is not
`Ok` is not emitted**. The caller passes a freshly sanitised value, not a stored one:

```cpp
const input::SanitizedHtml safe = input::sanitize_rich_text(stored, policy);
append_sanitized(out, safe);   // emits a comment, not the markup, when safe.verdict() != Ok
```

Rich text is already sanitised **on write** ([`12-sections-cms.md`](12-sections-cms.md)), and an
earlier draft of this document argued from that: sanitise where the value arrives, trust where it
is emitted. That is wrong here, and one sentence says why — a stored value that fails
re-sanitisation means **something bypassed the write path**, and the write path is exactly what
an attacker who has reached the database no longer has to go through.

The cost is one allow-list pass over a bounded field. The threat it answers is stored XSS in
content authored by one privileged user and rendered to another on an authenticated session —
the shape where an account holding only the right to *write* content escalates to whatever the
account *reading* it can do. Against that, a second pass on a page that is not on a hot path is
not a close call.

It is a close call on a page that **is** hot, and the decision there is different: cache the
sanitised form rather than skip the check. What must not happen is trusting the stored bytes
because the second pass looked expensive.

A failed re-sanitisation emits `<!-- content withheld: failed re-sanitisation -->`. Visible, not
silent: a blank region with no explanation is indistinguishable from a content bug, and gets
"fixed" by whoever finds it next.

## 5. Composition: one reserved string

A page is one allocation. The handler reserves once, appends through the writers, and returns.
No fragments, no per-field `std::string`, no `operator+`.

There is no chunked sink and no streaming render here. The writers append; **what bounds a page
is the handler's own limits on what it puts in one**, and every field these writers are designed
around already carries a code-point cap from `input/`. A page that can exceed the 256 KB line
ENGINEERING_RULES.md §2.4 draws is streamed or rejected like any other payload that size — that is §2.4's
rule, not an exception to it, and it is a change to a handler rather than to this mechanism.
Shipping a chunked renderer before a page needs one would be the same mistake §1 describes.

Values are borrowed. **A `string_view` into `req->body()` is valid only while the
`HttpRequestPtr` is alive** — ENGINEERING_RULES.md §2.2 names this as the single most likely crash in code
built on this library, and a render that borrows from a request and a document at the same time
is a place it is easy to arrange. Compose on the thread that holds the values; a composition is
pure CPU and bounded, so there is rarely a reason not to.

## 6. The content origin, and the credential that reaches it

This is the part the engine design omitted entirely, and it is the part that has actually failed
in production.

HTML assembled from partially-trusted content is served from `CONTENT_ORIGIN`, **a different
registrable host from `SITE_ORIGIN`** — not a path on the same host. Session cookies use the
`__Host-` prefix, so they are host-only and are never sent to the content origin: an XSS that
lands there cannot read them or ride the session. One configuration decision contains the entire
blast radius. `config::require_origin_split` checks the shape of both and refuses a deployment
where the two share a host ([`14-config.md`](14-config.md) §4) — it is one call rather than two,
because a boot site that made the shape check and forgot the separation looks correct in review
and is the whole defect.

**The comparison is on the host, with the port removed, and that is not a convenience.** Cookies
are not port-scoped: `https://x.test` and `https://x.test:8443` are two byte-distinct origins
that a browser treats as one host for every cookie it holds. A plain inequality would accept a
deployment whose split exists in the configuration and not in the browser.

**And that split is exactly what breaks the obvious implementation.** If the render path
requires a `UserContext`, and `UserContext` comes from `__Host-at`, and `__Host-at` is host-only,
then the context is null on every render and every request answers the stealth 404 — a page that
has never worked, in a deployment where nothing fails and no test sees it, because a test that
drives the repository or the service exercises neither cookie. This is not hypothetical; it has
shipped that way.

The separate origin is not the defect and must not be the fix. What was missing is a credential
that can reach that origin, and anvil already ships its shape in
[`accesscontrol/cookies.h`](../include/anvil/accesscontrol/cookies.h):

| | |
|---|---|
| **What it is** | A capability token bound to `{user, scope, subject}` like every other, the subject being the one document being previewed |
| **How it arrives** | The creating response hands out `CONTENT_ORIGIN/...?k=<token>`; following it sets the cookie and `303`s to the clean URL, so the token leaves the address bar on first load |
| **Cookie** | `kPreviewCookieName`, `Secure`, `HttpOnly`, `SameSite=Lax`, path-scoped to the one subject |
| **Not `__Host-`** | That prefix requires `Path=/`, and the path scoping is worth more: the browser will not attach it to a request for any *other* subject |
| **Not a session** | It carries **no permission bits**, so it cannot reach a write. Putting a full staff credential on the origin whose purpose is to be un-escalatable *from* hands back everything the split took away |
| **Lifetime** | The subject's remaining TTL and never longer, verified on every render with an **explicit expiry predicate** — the TTL monitor lags by up to a minute ([`09-mongodb.md`](09-mongodb.md) §6) |

The route is then `Public` in the table and enforced entirely by its handler — the one route
allowed to be, because the credential it takes is not the one the filter knows how to read. A
request carrying no credential is rejected on a length compare before it touches the database, so
a leaked id still gets the byte-identical stealth 404.

**The lesson generalises past this feature and is the reason §6 exists at all:** a page whose
security rests on an origin split has an authentication path that no unit test of the renderer,
the repository or the service will ever execute. It is end-to-end or it is unverified.

## 7. Headers

Every response carrying assembled HTML:

```
Content-Security-Policy: default-src 'none'; img-src <CONTENT_ORIGIN>; style-src 'self';
                         font-src 'self'; frame-ancestors 'none'; base-uri 'none';
                         form-action 'none'
Cache-Control: private, no-store
X-Robots-Tag: noindex, nofollow
Referrer-Policy: no-referrer
X-Content-Type-Options: nosniff
```

**No `script-src`, because there is no script.** A page that renders content for review needs no
JavaScript, so the strongest possible policy is also the correct one — and a policy with no
script source needs no nonce, which is why nothing here has one. A nonce is a mechanism for
pages that run scripts, and it forbids caching besides.

The directive is **absent** rather than spelled `script-src 'none'`, and the two are not the same
promise. With no `script-src`, `default-src 'none'` keeps governing script through every later
edit of the policy; with an explicit one, it stops the moment that directive gains a source.
Both block script today and they differ in what the next change does, which is why the test
asserts the absence rather than matching on the rest.

`no-store` is not a caching decision to revisit later. The content is unpublished and the URL is
a capability; a shared cache holding either is the leak the whole envelope exists to prevent.
`no-referrer` keeps the capability out of a third party's logs.

`http/content_headers.h` ships both halves: `content_security_policy(content_origin)`, built
**once at boot** because the only variable in it is deployment configuration, and
`apply_content_headers`, which puts the policy and the other four headers on a response.

`CONTENT_ORIGIN` is boot configuration and never request data, and it is still checked before it
lands inside a directive. A value carrying a `;` or a quote is not a broken origin — it is an
appended directive, and a policy an operator typo can rewrite is not a policy. One that fails
emits `img-src 'none'`: a review page with no pictures, rather than a review page with no
policy.

## 8. What is deferred, deliberately

**The template engine.** A grammar, a parser, a boot loader, a view catalogue and a page cache —
designed in full, then set aside for the three reasons in §1. The design is not lost; it is in
the history of this file. What would bring it back is not a consumer asking, but the mechanism
half proving insufficient: markup that composition in C++ genuinely cannot express readably, or
a second escaping context that has to exist. At that point the argument has to be re-made rather
than assumed, including whether markup living in files and parsed at boot is worth relaxing
ENGINEERING_RULES.md §1's "fails at configure or compile time" for. It is not relaxed today, and that is
the quiet benefit of the smaller mechanism.

**A page cache.** The three-tier shape `sections/service.h` uses is proven and reusable, and
it applies to a page that may be cached. The one that exists may not.

> One has since been built by the first application on this library, and two things it needed
> are worth recording, because the next consumer will need them too.
>
> **`SectionService::peek()` is not usable as an SSR read.** It returns a
> `shared_ptr<const SerializedSection>`, and `SerializedSection` holds pre-serialised API
> JSON — bytes shaped for the editor endpoint, not typed values shaped for a renderer
> (`sections/payload.h`). An SSR path built on it re-parses JSON per render; the alternative,
> `read_document()`, is blocking and belongs on `db_pool`. The consumer's answer was to hold
> its own snapshot of `SectionContent` ABOVE this library's cache rather than instead of it:
> one read per section per invalidation, typed values in memory, and anvil's cache left
> serving the JSON endpoint it is shaped for. That worked, and needed no change here.
>
> **There is no application invalidation callback, and a cache above this one wants exactly
> that.** `invalidate_local` and the Redis subscriber are both internal to `SectionService`,
> so a consumer cannot be told "this key changed, drop what you derived from it". Both
> workarounds are worse than a hook: drop-and-refill from the application's own write handler
> misses writes from every other instance, and subscribing to the same Redis channel
> separately is a second listener thread doing what one already does. A
> `std::function<void(std::string_view key)>` on `SectionServiceConfig`, invoked wherever
> `invalidate_local` already is, would close it — and would make "the three-tier shape is
> reusable" true for a derived cache as well as for this one.

**Layouts, partials and slots.** Composition in C++ is function calls. Slots become a language
the moment there are two of them.

**Streaming and chunked output.** See §5.

**A JavaScript escaping context.** See §2. The honest answer is that it cannot be written
correctly, so the context is removed instead.
