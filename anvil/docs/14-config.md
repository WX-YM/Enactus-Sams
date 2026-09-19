# 14 — Configuration

anvil ships **readers**, not a `Config` struct.

The system anvil was extracted from had a fixed fifty-member struct behind a
process singleton, and roughly a third of those members meant nothing outside that
product. What was worth keeping was never the struct — it was the discipline
around it.

## 1. Two rules the readers enforce

**A configuration error is a boot failure, never a degraded mode.** Every reader
throws `ConfigError` naming the variable and the rule it broke. A server that
starts with no signing key, or with its storage root inside the web root, is worse
than a server that does not start: the first is a silent compromise and the second
is a pager.

**An empty variable is an unset variable.** A deployment that exports
`SESSION_PEPPER=""` has made a mistake. Treating it as present produces a much
later and much stranger failure than treating it as missing.

## 2. Reading the environment

```cpp
struct Config final {
    std::string mongodb_uri;
    std::string site_origin;
    std::string content_origin;
    crypto::SecretBuffer<32> session_pepper;
    std::size_t db_pool_threads;
    // ...
};

Config load(const EnvLookup& env) {
    Config config{};
    config.mongodb_uri    = required_string(env, "MONGODB_URI");
    config.site_origin    = required_string(env, "SITE_ORIGIN");
    config.content_origin = required_string(env, "CONTENT_ORIGIN");
    require_origin_split("SITE_ORIGIN", config.site_origin,
                         "CONTENT_ORIGIN", config.content_origin);
    load_key(env, "SESSION_PEPPER", config.session_pepper, /*required=*/true);
    config.db_pool_threads = bounded_number(env, "DB_POOL_THREADS", 16, 1, 256);
    return config;
}
```

`EnvLookup` is injected rather than calling `getenv` directly, so a test supplies a
fixed map. **Calling `setenv` from a test is a data race against every other test
in the binary**, and the resulting failure is intermittent and gets blamed on
something else.

Numbers go through `from_chars`, never `stoull`: `stoull` accepts leading
whitespace and a sign, and throws a type that says nothing about which variable was
wrong.

Booleans accept `1/0`, `true/false`, `yes/no`, `on/off`, case-insensitively.
**Anything else is an error, not a false.** A typo in a variable that disables a
security control must not read as "disabled".

## 3. Secrets

`load_key<N>` decodes exactly N bytes of unpadded base64url straight into a
`SecretBuffer<N>`. The decoded material **never passes through a `std::string`**,
which would leave an un-zeroable copy on the heap for the allocator to hand to
whoever asks next.

`SecretBuffer` is move-only, zeroes itself on destruction with `OPENSSL_cleanse`,
and has no `operator<<`. It cannot be streamed, compared or formatted by accident.

The `required` flag carries a real distinction:

- **Required.** A deployment that booted without it would seal under a zero key and
  nothing downstream would report it. Boot fails.
- **Optional.** A deployment without it simply lacks that transport. An absent push
  key means no push, which is a reduced feature set rather than a silent
  compromise.

Getting this backwards in either direction is a bug. An optional signing key is a
zero-key deployment; a required push key is an outage over a feature nobody asked
for.

**`ANALYTICS_VISITOR_PEPPER` is required**, and the reason is the one §3 exists to
make: a visitor id is an HMAC over a packed client address, and an IPv4 address is
a 32-bit input space. Without a key the derivation is enumerable by anybody holding
a dump, so a zero-key deployment is one that stores addresses in a form it believes
to be anonymous. Boot fails rather than degrade.

## 4. Shape checks

**`require_origin_shape`** — an origin is compared byte-for-byte against the
`Origin` header, which browsers send as scheme `://` host [`:` port] and nothing
else. Accepting a trailing slash or a path produces a comparison that can never
match, and the symptom is every mutating request failing its CSRF check *in
production only*, because a developer's origin usually has neither. `http` is
permitted for `localhost` and `127.0.0.1` so a developer is not forced to terminate
TLS locally; anything else must be `https`, since `__Host-` cookies and SameSite
guarantees are meaningless over cleartext.

**`require_distinct`** — two settings that must differ, with both named in the
message. Used for any two keys where deriving one from the other would make a
compromise of either a compromise of both.

**`require_database_name`** — the characters the server forbids in a namespace,
refused at boot with the variable named rather than as an unexplained driver error
hours later. None of this is request data; a namespace is never derived from a
request.

**`require_directory`** — absolute, exists, is a directory, and is **not a
symlink**. The symlink check happens *before* canonicalisation, which would follow
the link and report the target's nature rather than the link's. A symlinked storage
root is a root whose destination can be changed by anyone who can write the link.

It returns the **canonical** path. Resolving once at boot and storing the result is
what lets every later open be relative to a descriptor instead of resolved by name
again, which is the whole TOCTOU defence in `fs/paths.h`.

**`require_origin_split`** — the shape of `SITE_ORIGIN`, the shape of
`CONTENT_ORIGIN`, and the separation between them, in **one call**. A deployment
that sets them to the same host still works, still passes every other test, and
has silently lost the isolation that contains an XSS in assembled HTML, since
`__Host-` cookies are host-only only with respect to a *different* host. Both
failure directions are quiet, so it is a boot failure rather than a warning — and
it is one function rather than three calls at the boot site because a boot site
that made the shape checks and forgot the separation looks correct in review.

The comparison is on the **host**, with the scheme and any port removed. Cookies
are not port-scoped: `https://x.test` and `https://x.test:8443` are two
byte-distinct origins and one host to every browser holding a cookie for it, so a
plain `require_distinct` would accept a deployment whose split exists in the
configuration and not in the browser. What it does *not* check is that the two are
different **registrable** domains, which needs a public-suffix list this library
does not ship; a sibling subdomain already satisfies the property at issue, since
a host-only cookie is never sent to it. See
[`19-server-side-rendering.md`](19-server-side-rendering.md) §6.

**`path_is_within`** — containment by path **component**, never by string prefix.
`/srv/storage-evil` has `/srv/storage` as a string prefix and is a completely
different directory.

**`METRICS_SCRAPE_CIDRS`** parses with the same CIDR matcher as `TRUSTED_PROXIES`, and an
empty value **allows nobody**. That is the deny-by-default direction and it is deliberate: a
deployment that has not set it has not decided who may scrape, and queue depths, cache hit
rates, denial counts and transaction aborts are a live map of where the system is weak and
when it is weakest. An empty list makes the scrape route answer nothing; a permissive default
would serve that map to whoever asked. The permission bit is the second, independent control,
because a bit leaks with a token and a network range does not
([`17-analytics.md`](17-analytics.md) §6).

**`ANALYTICS_SAMPLE_DENOMINATOR`** defaults to **1**, which keeps everything. Sampling is a
pressure valve rather than a policy, and it engages only above the sink's high-water mark. A
denominator above 1 thins **per session** rather than per event, so a funnel stays whole or
absent; the sampled-out count is itself a counter, so the true rate stays recoverable by
multiplication ([`17-analytics.md`](17-analytics.md) §13).

**`is_web_served_root`** — a heuristic, and honest about being one. A storage root
under `/var/www` or `/srv/http` is reachable without passing through the
application at all: the `internal;` marker that makes protected media private
applies to one location block, and a second block serving the same bytes as static
files undoes it in silence. Nothing errors; the files are simply public. An
application should refuse to boot on a true result, and still check its own root if
it serves from an unconventional place.

## 5. The boot summary

```cpp
LOG_INFO << RedactedSummary{}
                .line("MONGO_DB", config.mongo_database)
                .line("DB_POOL_THREADS", config.db_pool_threads)
                .secret("SESSION_PEPPER", true)
                .str();
```

It renders `set` or `UNSET` for a secret — never the value, and **never its
length**, because a length is a meaningful hint about a short secret.

Its real purpose is that a count is often the only thing distinguishing a correct
deployment from a silently broken one. A trusted-proxy list that is empty and one
that is over-wide both behave plausibly and fail in opposite directions; the
number in the boot summary is what tells them apart.

`RedactedSummary` has no access to key material. An application that can only
render its configuration through this cannot leak a secret by logging.

## 6. What an application still owns

- Its `Config` struct and the `load()` that fills it.
- Cross-field invariants: that two databases differ, that a pool is large enough
  for the threads that draw from it, that a derived default did not collide with
  an explicit one.
- Whether to hold it in a singleton. anvil does not, and the one place in the
  original library that read a config singleton was the one place worth removing.
