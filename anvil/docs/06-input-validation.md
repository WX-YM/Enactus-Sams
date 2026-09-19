# 06 — Input validation

Every byte a client sends passes through this layer before it reaches a service,
a filter, or a database. Three properties hold across all of it.

**Nothing allocates in proportion to the input.** A validator that allocates is a
validator an unauthenticated caller can drive at their own rate. `tests/validation_fuzz_test.cc`
replaces global `operator new` and asserts a count of zero over a random corpus —
not "these examples do not allocate" but "no input in this corpus makes any of
them allocate".

**Bounds are in code points, never bytes.** A byte limit silently halves the
allowance for any non-Latin script, so a 200-byte field that accepts 200 English
characters accepts about 100 Arabic ones. Every limit here counts code points.

**No error carries the submitted value, and none carries a client-chosen key.**
Echoing input into a response is a reflected-XSS vector; echoing it into a log is
log injection; and with non-Latin text it is an encoding hazard on top. Field
names in errors are compile-time constants.

## 1. The arena

`RequestArena<N>` is a `std::pmr::monotonic_buffer_resource` over a stack array.
One bump-pointer region, one bulk release, zero individual frees.

```cpp
using BodyArena = RequestArena<65536>;
```

64 KiB, sized from the worst realistic *shape* rather than from the body size: a
6 KB document of many small objects needs roughly 40 KiB of parse nodes, because
the cost is per node, not per byte. Objects allocated from it must be trivially
destructible — `static_assert`ed — because nothing is destroyed individually.

`buffer_` is declared before `resource_`, and the order is load-bearing: the
resource is constructed with a pointer into the buffer. `-Werror=reorder` keeps
it that way.

## 2. The JSON parser

Hand-written, ~660 lines, and deliberately strict. There is no third-party JSON
library in either direction — reading is `input/json.h`, writing is
`http/json_writer.h`.

**Strings borrow.** A string containing no escape is a `string_view` into the
request body, not a copy. Only an escaped string is materialised, and it is
materialised from the arena. This is the single largest allocation saving on the
request path, and it comes with the single most dangerous lifetime rule in the
library: **a parsed document borrows from both the body and the arena, so it must
never cross a thread-pool boundary without them.**

**Every limit is enforced during the parse, not after.** A document 64 levels deep
is refused at level 9, not built and then rejected.

| Limit | Default |
|---|---|
| Body | 256 KB |
| String | 64 KB |
| Depth | 8 |
| Keys per object | 64 |
| Elements per array | 256 |

**Duplicate keys are an error.** Two parsers that disagree about which value wins
is a parser-differential primitive, and the asymmetry is invisible in review.

**Numbers are kept as source text** and converted on demand. Eager conversion to
`double` silently rounds a 64-bit identifier and loses the `1e309` overflow.

**No accessor coerces.** `as_string()` on a number returns `nullopt`. This is what
makes `{"email": {"$gt": ""}}` fail as *not a string* rather than being forwarded
into a BSON filter as an operator document — which is an authentication bypass.

Object lookup is linear over at most 64 entries, which beats a hash map at that
size and allocates nothing. `JsonValue` is 32 bytes with a tagged union sharing
one pointer between its array and object arms.

`json_error_name()` exists for **server-side logs only**. The wire answer is
always the same `VALIDATION_FAILED` with no field, because "DepthExceeded" would
disclose the depth cap.

The order is deliberate: size cap, then UTF-8 validation, then parse.

## 3. The schema binder

`ObjectBinder` walks a parsed object and binds declared fields. Two rules it
enforces rather than documents:

1. **An unknown field is an error**, never silently dropped. A field a binder does
   not know about is a field the client believes it sent.
2. **`finish()` is `[[nodiscard]]`**, and warnings are errors, so forgetting rule 1
   does not compile.

Consumed keys are tracked in a `std::bitset<64>` indexed by member position — the
key limit is 64, so the whole consumed-set is one register. `finish()` returns a
`FieldError` whose field name is **empty**, deliberately: naming the offending key
echoes client bytes into a response and a log.

Three binder shapes are worth knowing:

- `optional_nullable_*` uses `optional<optional<T>>` to distinguish **absent**
  (leave alone) from **present-null** (clear) from **present-value** (set). A
  partial edit cannot express "clear this field" without it.
- `array` is **required** — an absent key reports `Required`.
- `optional_array` reports `Ok` and returns `nullptr`, matching every other
  `optional_*`.

That last asymmetry is not a detail. It was the direct cause of four separately
shipped defects, each a route refusing a perfectly ordinary body with
`{"<key>":"INVALID"}` because the key simply was not there. Reach for
`optional_array` whenever the code after the call reads `if (value != nullptr)`,
because that branch says the absence is legal.

## 4. Scalar validators

All hand-written linear scanners. **`std::regex` is banned** on any path reachable
from a request: it is a backtracking engine, so a crafted input against an
RFC 5322-style pattern is catastrophic backtracking — one request pinning a core
for seconds — construction alone costs tens of microseconds, and its behaviour on
non-ASCII bytes follows the locale. `tools/check-source-bans.sh` enforces the ban
rather than review. RE2 is available behind `ANVIL_WITH_RE2` as an escape hatch;
the ban stands regardless.

| Validator | Notes |
|---|---|
| `check_email` | 254-byte cap, one unquoted `@`, local part ≤ 64, label structure, TLD present, **no non-ASCII domain** — which is also what kills the Cyrillic-homoglyph `pаypal.com` |
| `parse_date` / `parse_timestamp` | ISO-8601 with an explicit offset or a bare `Z`. Validates the **calendar**, not the digit pattern, using era arithmetic — no libc, no tzdata, no locale |
| `check_url` | Allow-list of `https://`, `mailto:` and site-relative. Rejects credentials in the authority, IP literals, and non-ASCII hosts. `UrlUse::ServerFetch` additionally rejects private and link-local addresses |
| `check_text` | Code-point bounds plus a text class from `i18n/bidi.h` |
| `check_password` | Length in code points, and a compile-time Bloom filter of breached passwords |

**Arabic-Indic digit folding runs before every numeric, date and identity check**,
into a stack buffer. A user typing an identifier on an Arabic keyboard produces
U+0660–U+0669, and a validator that rejects those is a validator that rejects that
user.

## 5. The HTML sanitiser

A single linear pass with an explicit element stack. No regex, and output that is
well-formed **by construction** rather than by validation afterwards.

Four `constexpr` tables: 14 allowed elements, 11 hostile ones, 2 void elements, 6
named entities. A hostile element is **dropped**, not escaped. Nesting depth is
bounded.

## 6. The breach filter

A 16384-bit, 4-hash `constexpr` Bloom filter in `.rodata`. A lookup is four shifts
and four tests against a 2 KiB table.

The list shipped in `input/breach_filter.h` is a **seed set of 40 passwords, and
the header says so**. A production deployment generates its own header offline
from a breach corpus with the same `constexpr` machinery. Shipping a real corpus
in the library would be a large binary nobody audits.

## 7. Locale-specific validators

`ANVIL_WITH_EGY` (default **OFF**) builds Egyptian national-identity and mobile
validators. They live in `locale_egy/` rather than `input/` because they are one
locale's rules, not anvil's.

They ship because they are a worked template for a `phone_<cc>` family, and the
shape generalises even where the digits do not:

- fold Arabic-Indic digits first;
- validate the **calendar** encoded in the number, not its digit pattern;
- keep the parsed value in automatic storage (`PhoneEgy` is 14 bytes, asserted);
- distinguish **NotAllowed** from **BadFormat**, so the interface can say which —
  a landline in a mobile field is a different message from nine digits;
- make the checksum mode explicit. Egypt publishes no authoritative check-digit
  specification, so the default is advisory: an incorrect implementation rejecting
  valid citizens is far more damaging than accepting an invalid number.
