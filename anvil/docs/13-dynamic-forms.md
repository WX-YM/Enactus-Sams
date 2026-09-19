# 13 — Dynamic forms

A form maker: staff declare questions at runtime, the public answers them, and no
schema migration happens in between. Bilingual labels, one storage collection, and an
identity number that never sits in plaintext anywhere.

anvil ships the machinery. The FIELD TYPES are yours — see `docs/01-seams.md` §5 — because a
table of eleven field types is one application's vocabulary and a table anvil populated would
be a table anvil had to guess.

---

## 1. Storage: one collection, not one per form

The design everybody reaches for first gives each form its own `answers_<id>` collection and
adds a guardrail at some thousand forms. Both halves are wrong, and both are worth stating
because the alternative looks so reasonable:

- Every collection is at least one WiredTiger file plus one per index, so 1 000 forms is
  3 000 descriptors against a common `ulimit -n` of 1 024. **The real ceiling is around 300
  forms and it arrives as `Too many open files` — an outage, not a slowdown.**
- Each table carries persistent in-memory metadata. Thousands of them consume cache that
  should hold hot documents, and they slow every startup and every `listCollections`.
- Dropping a form would be **DDL**, which takes locks and is *not* transactional with the
  definition delete. A crash between them leaves a form pointing at nothing, or an orphan
  collection nothing references.
- **A collection name derived from request data is injection.** `answers_` plus an id is safe
  only while the id is a server-generated UUID nobody echoes back, which is one refactor away
  from a name like `answers_../admin`.

And the migration out is worse than starting partitioned: merging N collections into one is a
data move under production load with a dual-write window and a cutover, to avoid a single
indexed field today. The threshold is also undetectable in aggregate — nobody notices form
950.

So: **`form_submissions` with a `form` discriminator from day one.** Cost: 16 bytes and one
compound index per document. Benefit: every problem above disappears, and dropping a form
becomes `delete_many` plus `delete_one` in one transaction.

Both collections live in **one database**, and the constructor takes them together
(`FormRepository{database, definitions, submissions, types}`). A replica-set transaction can
span databases, so splitting them would work — but the drop's invariant is easier to state
than to reconstruct, and two places to look is how two places drift.

---

## 2. The definition

```jsonc
{
  "_id": BinData(4),
  "creator": BinData(4),
  "title":  { "en": "Membership Application", "ar": "طلب عضوية" },
  "fields": [
    { "fid": "f1", "label": {"en":"Full Name","ar":"الاسم الكامل"},
      "type": 2, "opt": false, "max_cp": 120 },
    { "fid": "f2", "label": {"en":"Select Branch","ar":"اختر الفرع"},
      "type": 6, "opt": false,
      "options": [ {"value":"b1","label":{"en":"Cairo","ar":"القاهرة"}},
                   {"value":"b2","label":{"en":"Alexandria","ar":"الإسكندرية"}} ] }
  ],
  "has_pii": true,
  "status": 1,
  "max_submissions": 1000,
  "one_per_user": true,
  "closes_at": ISODate|null,
  "submission_count": 0,
  "v": 1
}
```

`type` is an **int on disk**, never `"NAME"`. A type string repeated across every field of
every form is wasted bytes and a string compare per field at validation time, where an int is
a direct index.

**On the wire it is the name**, in both directions, and that is the opposite trade for the
opposite reason: a client reading `9` has to carry a copy of the table and goes wrong
*silently* the moment the two disagree, and the cost of a name is paid once per request rather
than once per stored document. `FieldTypeSpec::wire_name` is the one string both directions
use.

Limits, all enforced at creation: ≤ 100 fields, ≤ 50 options per field, labels ≤ 200 code
points **in every declared locale**, option values `[A-Za-z0-9_-]` and ≤ 64 bytes.

### `Fid` is structural, not a validation rule

A `fid` becomes a key inside the `ans` subdocument, which is the one piece of
client-influenced text that reaches a document's key space. There, a leading `$` is a BSON
operator the server would execute and a dot is a path separator.

`anvil::forms::Fid` is a 5-byte value object whose **only** constructor is `Fid::parse`, and
it accepts exactly `f[0-9]{1,3}`. `$set` and `a.b` are therefore *unrepresentable* rather than
filtered: there is no code path anywhere that can produce an unvalidated one, so no future
caller can forget to filter. It is also the cheapest thing to compare on the submission path —
a 4-byte array compare, not a string compare against a heap-allocated key.

Arabic-Indic digits are **rejected** in a `fid` rather than folded, unlike in an answer. A
field id is an internal identifier an author never types.

---

## 3. Creating and editing

`validate_schema(types, schema)` checks the shape, the per-type constraints and every limit.
It is pure — no database, no clock — and its `Failure` names the **rule** as a compile-time
string, never the offending `fid` and never the submitted value.

It also **resolves** every field against the table: `bind_field_type` fills the flag copy and
the code-point cap, so the schema that comes out is the one that gets stored. Resolving is
part of validating rather than a second step, because a second step is a step somebody forgets
— and a `FieldSpec` bound from a request body and the same field read back from the database
must answer "is this multi-line" identically.

`has_pii` is **derived** from the table, never accepted from a client. A form that
under-declares its PII gets the wrong storage treatment and nothing downstream notices.

### Editing a form that already has submissions

This is the sharp edge of the whole feature. Changing a field's type or removing a field makes
existing answers *unreadable* against the new schema — not wrong, unreadable, because the
decoder asserts an answer's BSON type against its declared field type and refuses to coerce.

| Change | Once `submission_count > 0` |
|---|---|
| Adding an **optional** field | allowed |
| Changing a label | allowed |
| Adding an option | allowed |
| Removing a field | **blocked** — orphans every answer stored under its id |
| Changing a type | **blocked** — stored answers stop decoding |
| Removing an option | **blocked** — strands the submissions that chose it |
| Making an optional field required | **blocked** — invalidates every submission that omitted it |
| Adding a **required** field | **blocked** — same, from the other direction |

"Blocked" is `Conflict`, not `ValidationFailed`: the schema is perfectly well formed and what
it conflicts with is the data already stored. A caller can only offer "duplicate this form
instead" if it can tell the two apart.

Every accepted change bumps the version through `db/versioned.h`, and each submission records
the version it was validated against, so old rows stay interpretable.

### The definition cache is a parsed struct

The obvious cache is a Redis key holding the serialised definition, and it costs a round trip
**and a parse** on every submission. Validation has to be a loop over a
`std::span<const FieldSpec>` with no allocation, so the cache is process-local and holds
`shared_ptr<const FormDefinition>` — already parsed, shared by every thread, read with one
atomic load.

It is **direct-mapped over 256 slots and fixed-capacity**, for the same reason the epoch cache
is: which form is cached is decided by whoever is submitting, so an unbounded map is a
memory-exhaustion vector and eviction is the correct failure. A collision lands on the same
slot with a different id, and the id check is what makes that a *miss* rather than the wrong
form. A thousand forms must cost what ten forms cost.

The cost of dropping Redis from this path is cross-instance invalidation: an edit on instance A
is invisible to instance B until B's entry expires, bounded by `kDefinitionCacheTtl` (60 s).
That is **safe**, and the edit rules above are why: a stale definition can only be *more*
restrictive than the current one, so a submission validated against it is still valid against
the new one.

---

## 4. Submitting

The hot path, and the most exposed write endpoint a system built on this library has: a public
form is unauthenticated by design, so every check runs against input from nobody in
particular.

1. **Status, closing time and the ceiling.** A closed form that still accepts writes is a form
   nobody is watching. A Draft or Closed form answers `NotFound` rather than `Conflict` — a
   draft's existence is not public information, and answering differently is an enumeration
   oracle on an administrator's work in progress.
2. **Answer keys in both directions.** Every key in `ans` must name a field, and every
   required field must have a key. An unknown key is **rejected, never dropped**: dropping
   hides both client bugs and probing, and an accepted unknown key is stored data nobody
   validated.
3. **Per-type validation, dispatched through the table.** One indexed load and one indirect
   call. The shape is checked before the value in every validator, so `{"f1": {"$gt": ""}}`
   fails as "not a string" before anything compares it to anything.
4. **Option values checked against the SERVER's list.** The list rendered in the browser is
   not a constraint. A multi-select additionally rejects duplicates — a repeated choice is not
   a stronger answer, it is a client bug or an attempt to inflate a tally — and enforces the
   selection ceiling.
5. **Attachments resolved through the application's hooks** (§5).
6. **PII split out of `ans` entirely and sealed** (§6).
7. **Insert**, and only when there are attachments, a transaction.

An optional field sent empty is stored as **nothing** rather than as an empty string: an
absent answer and a blank one must not be two different things in an export. The same value on
a required field is *missing*, not blank.

### What is enforced by an index rather than by this code

Two rules are races if they are checked here and then acted on:

| Rule | Index |
|---|---|
| one submission per person | `{form, uniq}` unique, partial on `$exists` |
| one object, one submission | `{media}` unique, multikey, partial on `$exists` |

Both surface as a duplicate-key error, which `repo::translate` turns into `Conflict`. A
count-then-insert loses to a double-click every time.

`uniq` is a **second copy** of the user id, written only when the form asks for one submission
per person — `uid` carries the attribution unconditionally. Two fields rather than a unique
index on the attribution, because attribution and the one-per-user rule are different
questions: a form that allows several submissions per person still needs to know who sent each
one. The partial filter is what keeps every anonymous row out of the index, and without it the
first row with no `uniq` would lock out every other one.

### The transaction is skipped when there is nothing to bind

A submission with no attachment is **one document and one write**, which the server already
applies atomically — the first of the two options ENGINEERING_RULES.md §6 allows, and there is no second
document for a transaction to bind it to. This is the common case rather than an optimisation
for a rare one: an anonymous submitter cannot upload anything, so every submission to a public
form takes it. The transaction it skips costs a session, a commit round trip and a
majority-commit wait.

### The counter is not on the request path at all

Every submission to a form would increment the **same** definition document. Inside a
transaction that is a write conflict that rolls back the insert and the attachment references
with it, and even outside one it is a second blocking round trip and a second durable write
the client waits for.

So increments are buffered in-process and flushed as one unordered bulk write: N submissions
to one form collapse into a single `$inc` of N. The application drives the flush —
`flush_counts(fn)` takes the callable that reaches the database, because *which pool a flush
runs on* is a deployment decision and a library that reached for a process-wide singleton
would make it for you. Call it on a timer, and once more during shutdown on the calling thread
**before the pools drain**: a flush posted to a draining pool may never run, and these counts
exist nowhere else.

The counter therefore lags by up to one flush interval. That is within the contract step 1
already states — the ceiling is advisory under concurrency, which is why it is a business
limit rather than a safety property. A crash loses at most one interval of counts and nothing
else: the submissions are durable the moment they commit, and the count is derivable from them.

What it is *not* within is the drop/submit race the in-transaction increment used to settle for
free. See §7.

---

## 5. Attachments arrive through hooks

A field type flagged `Attachment` resolves through three callables the application supplies,
and anvil cannot answer any of the three questions itself:

```cpp
struct AttachmentHooks final {
    AttachmentResolver may_bind;   // db_pool: may THIS submitter bind THIS object?
    AttachmentBinder   bind;       // +1, inside the caller's transaction
    AttachmentBinder   release;    // -1, likewise
};
```

**`may_bind` is the one with teeth.** Which storage namespace a form's attachments live in is
the application's table, and what "owned by" means for a submitter with no account is its
policy. Getting it wrong is an IDOR with a confidentiality impact: a submitter references
somebody else's private object by id and an authenticated staff member's review screen renders
it. It must answer with the **same** response for "does not exist" and "is not yours" — which
ids exist is not something a failed submission confirms.

**`bind` and `release` take the caller's session** because the count must move inside the same
transaction as the row that owns it. A count that commits while the submission aborts is a
leak no sweep can distinguish from a live reference; the reverse is a file deleted out from
under a live row.

All three empty means the application declares no attachment field type, and a submission
carrying one is refused with the same not-found. That is the correct direction to fail —
accepting an id nothing will ever reference is how a file becomes unreachable and
uncollectable at the same time.

---

## 6. PII — the identity envelope

An identity number is the worst thing a submissions collection can hold in plaintext. An
Egyptian National ID encodes date of birth, governorate and gender; a passport number is a
travel document. Either way **one database dump discloses full identity records for every
applicant.**

Four properties answer that, and each is a decision rather than an implementation detail:

1. **Never in `ans`.** The value is extracted before the submission document is built. A PII
   validator writes the normalised identity to its `identity` out-parameter and leaves `out`
   untouched; the submission service *checks* that rather than trusting it, and the decoder
   reports a PII field carrying a value in `ans` as an integrity fault rather than decoding it.
2. **Sealed with a key that is not the database credential.** AES-256-GCM, IV generated inside
   `seal()` and never a parameter, so a caller cannot supply one by mistake. Holding the dump
   is not enough.
3. **Searchable without decryption**, via HMAC-SHA256 under a **second** key. A plain SHA-256
   of a structured 14-digit number is exhaustible in seconds — 10^14 candidates against a
   7 GH/s hash is under a day on one GPU, and the century and governorate digits cut the space
   by two more orders of magnitude. The keyed construction is what makes the index safe to
   store beside the ciphertext it indexes.
4. **Redaction hides the prefix.** In a structured number the *front* is the birth date, so a
   partial reveal of the front leaks far more than the last four do. A value of four
   characters or fewer is masked entirely — revealing all of it because it is short is the
   opposite of the rule. The mask is one U+2022 per hidden **code point**, and the visible
   suffix is sliced on a code-point boundary rather than at a byte offset.

### Two keys, never one

`PiiKeys` holds both and refuses to be constructed from one: a caller cannot pass the index key
where the sealing key belongs, and deriving both from one master would mean a compromise of
either purpose is a compromise of both. The index key must be online for every write; the
sealing key need only be online for an unseal. The equality check is `CRYPTO_memcmp` even
though neither value is attacker-supplied — ENGINEERING_RULES.md §5 admits no exception for "this one
cannot leak", because the next reader cannot tell which comparisons were exempt and why.

### The AAD binds an envelope to one form and one field

`pii_aad(form_id, field_id)` is authenticated but not encrypted, so a ciphertext lifted into
another field or another form **fails to open** rather than decrypting into the wrong record.
The whole context is 16 bytes plus at most four, so it fits a stack array and never allocates.

### Normalisation and redaction are policy

Both were hard-coded in the system this was extracted from, and both are wrong for somebody.
anvil's default normaliser folds Arabic-Indic digits, strips spaces, hyphens and underscores,
and uppercases ASCII — right for a national id and a passport number, wrong for anything whose
separators are significant. The default redactor keeps the last four characters — right where
the front carries the structure, wrong where the back does.

So they are function pointers in a `PiiPolicy`, with anvil's defaults named. What is **not**
negotiable is that the blind index is over the *normalised* value: two spellings of one
identity must produce one digest, or duplicate detection silently stops working. Somebody who
types "AB-123456" on one form and "AB123456" on the next is the same person.

Changing the normaliser after the first row is written changes what "duplicate" means for
every row before it. Decide it once, next to the keys.

### The redacted form is stored

Computed once, at seal time, from the value that is about to stop existing in plaintext. That
is what keeps the **default read path key-free**: the ordinary submissions view shows a
redacted identity and unsealing requires a separate authority, and both can only be true if
the redacted form does not come from a decryption. It discloses exactly what the redacted view
discloses and nothing more.

### One identity field per form

A library rule, not a flag (`kMaxPiiFieldsPerForm == 1`). It is a property of the storage
format — one envelope, one blind index, one AAD binding one envelope to one field id — not of
any field type. A per-type flag would let an application declare two PII types and get a
silently single-valued write, where the second identity is validated, accepted and dropped.

### Duplicate detection is advisory

`blind_index_seen` is a read, not a unique index, and two concurrent submissions of one
identity may both pass it. That is deliberate: the value is in catching the ordinary repeat
submission, and a unique index here would make one person's identity globally unusable if a
form were ever restored from a backup.

---

## 7. Dropping a form

`drop` is one transaction:

```
count submissions          refuse above kMaxDropSubmissions rather than attempt it
collect bound attachments  projected to the media array only
delete_many({form: id})    no DDL, no locks, no orphans
delete_one({_id: id})
release every attachment   inside the SAME transaction
                           ── then ──
invalidate the cache
reap stragglers
```

The count is refused above 10 000 rather than attempted, because a transaction that large hits
the server's oplog and time budgets and fails halfway — the same outcome with none of the
explanation. The operator exports first, and the limit is a number in a config review rather
than an outage.

### Reaping the drop/submit race

The counter increment used to be inside the submission transaction, and while it was there it
did a second, undocumented job: it made a drop and a concurrent submission write the *same*
definition document, so one of them always lost. Moving it out for the throughput reason in §4
removes that serialisation, and with it the guarantee.

Without it the two transactions touch disjoint documents. A submission that commits just after
the drop commits is invisible to the drop's `delete_many` — its row was not in that snapshot —
and it survives as a submission whose form no longer exists. That is not merely untidy: the row
holds attachment references, and a reference nothing can ever release is a file the sweeper
will not collect.

So the drop reaps. **Invalidation runs before the reap, deliberately:** once the definition is
invalidated no reader can obtain it, so the only submissions that can still land are those
already in flight. Each pass therefore sees strictly fewer rows than the last and the loop
terminates; the pass bound exists so that a defect elsewhere costs a logged warning rather
than a thread.

The reap runs after the drop has already answered, so a failure is logged rather than
returned. The drop itself succeeded, and its report is not made wrong by a straggler surviving
one more pass.

---

## 8. The CSV export

anvil is a library and holds no routes, so what ships is the half a controller cannot get
right on its own: the row assembly, the formula-injection guard, the BOM, and the paging that
keeps memory bounded. The application owns the route, the permission, the audit row and how the
finished file reaches the client — `X-Accel-Redirect` over a temporary file, never a response
body assembled in the heap.

**Never materialised.** `export_submissions` streams rows to a sink in cursor order, one page
resident at a time and one 64 KB buffer reused. A form with 100 000 submissions never becomes
one string, and never becomes one vector either.

**Formula injection is neutralised.** A cell beginning `=`, `+`, `-`, `@`, tab or CR is a
formula to a spreadsheet, so a submitter's answer of `=HYPERLINK("http://evil","click")`
executes the moment a staff member opens the export. The submitter chose that text and the
staff member is authenticated: this is stored code execution with a delivery mechanism built
into the product. Every cell goes through `append_csv_cell`, including the **header labels** —
a form author is a likelier source of `=cmd|…` than a form filler, because they can put it in
every export of that form rather than in one row of one.

**A UTF-8 BOM, once, at the head of the file.** Excel decodes a CSV in the system codepage
without it and renders Arabic as mojibake. The bytes are correct either way; the file is
unreadable to the people it was exported for.

**The column order is the definition's field order**, not the row's answer order: a
spreadsheet column has to mean one thing all the way down. A field with no answer is an empty
cell, which is also what an optional field sent blank produces — the two really are the same
thing here, which is why an empty optional answer is stored as nothing.

A multi-select is **one cell** with its values joined by `; `, not N columns. Otherwise the
column count depends on how many boxes the widest respondent ticked, and every row after that
one is misaligned.

**The identity column is opt-in and separate.** `include_identity` is a decision the caller
makes after checking a *different* authority from the one that let them read the submissions,
and after writing the audit row — bulk PII access is exactly what an audit trail exists for.
The default carries the stored redacted form, so the ordinary export path never touches key
material. An envelope that will not open writes an empty cell rather than failing: one
unreadable row must not cost an operator the other 99 999.

An export of a form with no submissions is still a file with its columns in it, which is what
tells an operator the form is empty rather than the export broken.

---

## 9. The dispatch benchmark

The `switch` this replaces was a real piece of code, and replacing a jump table with an
indirect call through `.rodata` is the kind of change that is assumed to be free and sometimes
is not. So it was measured rather than assumed.

`-O2`, `NDEBUG`, six field types (TEXT_SHORT, TEXT_LONG, NAME, NUMBER, EMAIL, DATE), 1 200 000
validations per run, best of five rounds after a warm pass each, on a 12th Gen Intel Core
i7-12650H:

| | ns per answer |
|---|---|
| `switch` on the type | 31.1 – 31.9 |
| table dispatch | 30.9 – 31.8 |

**Between −1.5% and −0.4%** across three runs — which is to say indistinguishable, and if
anything marginally in the table's favour. The validator bodies dominate completely: a UTF-8
scan, a code-point count and a bidi check are tens of nanoseconds, and one indexed load plus
one indirect call is a fraction of one.

That is the number, and the conclusion is that the seam costs nothing measurable. The
correctness half of the comparison is a test — `FormDispatch.TableDispatchAgreesWithTheSwitchItReplaces`
— because a wall-clock threshold on a shared runner is a flaky test and jitter is not a fix for
anything.

---

## 10. Notes that are not obvious

**`answer_kind_of` is derived in one place.** The BSON shape an answer takes comes from the
type's flags, and the writer, the reader and the submission service all read the same
derivation. It is also the one hazard the custom-validator extension point carries: a
validator that fills `out.number` for a type with no `Ranged` flag would produce a row the
decoder refuses, months later, with an integrity fault nobody can trace back. So the
submission service checks the kind the validator produced against `answer_kind_of` and refuses
the write at the moment the table is wrong.

**A null validator is not in `table_is_well_formed`, and it is not an oversight.** Under
`-fsanitize=undefined` this compiler refuses to fold a function-pointer null comparison into a
constant expression, so a `static_assert` over the check stops compiling the moment the null
test is inside it — and the sanitiser build is the gate. A `well_formed()` that does not
compile is worse than one that checks less, which is exactly the trade
`query_catalogue_is_well_formed` already makes. `validators_are_present()` is a runtime check
instead, asserted by the seam test, callable at boot, and backed by the submission path
refusing a null validator **by name** rather than jumping through it.

**A field's flags and code-point cap are resolved, never stored.** They come from the build's
table. A stored copy would be a second answer to "what does type 6 mean" that a deploy could
make disagree with the first, and `bind_field_type` is the one place either is written.

**A stored cap above the global ceiling is a failure, not a clamp.** Zero is the one value
that resolves, because zero is how "the author named no cap" is stored. Resolving an absurd
value to something plausible would launder a definition this build cannot honour into one that
looks honoured.

**A type this build does not declare decodes to nothing.** That is a real state during a
rolling deploy — a definition written by a newer process — and refusing is the correct
direction to fail. It is also why a retired code must never be reused.

**Two validators are all `anvil_locale_egy` would be.** Writing a custom validator is the
extension point: a free function matching `ValidateFn`, dropped into the table.
`tests/testapp/field_types.h` ships one — a generic identity document, deliberately without a
national-id checksum, because no authoritative specification is published and an incorrect
implementation rejecting real citizens is far more damaging than accepting an invalid number.

**The forms subsystem is split across two link targets on purpose.** `fid`, `answer`,
`field_type`, `validators` and `pii` are in `anvil::foundation` and link no driver, which is
what lets a locale module — pure CPU, OpenSSL and ICU only — ship a field-type validator of its
own. A header that dragged bsoncxx into the seam would put it out of reach.

---

## 11. Deferred, deliberately: backup, export-before-drop, and retention

Three things a production deployment wants and this phase does not ship, recorded rather than
silently dropped:

- **An automatic export before a drop.** The drop refuses above 10 000 submissions and expects
  the operator to export first, which is a procedure rather than a mechanism. The mechanism
  would be a job that writes the export, records where, and only then consumes the capability.
- **PII retention.** An identity should be purged on a schedule — twelve months is a common
  default — even when the submission is kept: the submission remains for statistics, the
  identity does not. That is a job over `pii_index`, not a TTL index, because the row survives
  and only three fields are unset.
- **A soft delete for the definition.** A dropped form is gone. A 30-day tombstone would make
  the one destructive operation recoverable, at the cost of a second lifecycle state every
  read has to filter on.

None of the three is blocked on anything here; all three are a phase of their own.
