#pragma once

// The build-time half of the route table.
//
// `RoutePolicy` (anvil/accesscontrol/route_registry.h) carries what the FILTER
// needs on every protected request: a mask, a pattern, a method and an access
// class. This carries what a CLIENT GENERATOR needs and the filter never reads:
// a stable id, the capability the route consumes, the rate-limit bucket it
// counts into, whether repeating it is safe, and — for a list route — the
// cursor field and the ceiling on its limit.
//
// --- two tables rather than one wider one ----------------------------------
//
// The reason is where each is read. `policy_for()` scans the route table
// linearly on every protected request, so every byte added to `RoutePolicy` is a
// byte pulled through L1 on a path whose whole design is that it does no work.
// This struct is read once, by a program that prints a file and exits. Fusing
// them would put build-time metadata into the request path's cache lines to save
// an application one `static_assert`.
//
// The two are kept in agreement by `descriptions_match()`, which is constexpr
// and belongs in a `static_assert` beside both tables. A description naming a
// route that does not exist, or a route with no description, is then a build
// failure rather than a client generated with a hole in it.
//
// --- why the id is not the path --------------------------------------------
//
// The id is what a client names the route by, so it is compiled into whichever
// bundle calls it. A client that names routes by path publishes the path — and
// the path of an administrative route is the map the stealth 404 exists to
// withhold (docs/04-access-control.md §3). An opaque id leaks that a capability
// exists, not where it lives.
//
// Ids are STABLE. Renaming one breaks every client built against the old name,
// in the same way and for the same reason that renaming a permission does.

#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/accesscontrol/route_registry.h"
#include "anvil/http/response_spec.h"

namespace anvil::descriptor {

struct RouteDescription final {
    // Opaque, stable, and never the path. See the header comment.
    std::string_view id;             // 16

    // Must equal the `pattern` of exactly one RoutePolicy, under this method.
    std::string_view pattern;        // 16

    // The capability scope this route consumes, empty when it consumes none.
    // A generated client makes the requirement a parameter type, so a caller
    // cannot reach the route without holding one.
    std::string_view capability;     // 16

    // The rate-limit bucket this route counts into, empty when no rule names it.
    // The bucket rather than the route, for the reason RateLimitRule gives.
    std::string_view rate_bucket;    // 16

    // The indexed key a list route pages by, empty when it is not a list route.
    // A client that is handed a cursor field cannot express an offset, which is
    // the point: skip(n) is O(n) server-side (ENGINEERING_RULES.md §7).
    std::string_view cursor_field;   // 16

    // The ceiling this route accepts for `limit`, 0 when it is not a list route.
    std::uint32_t    limit_max;      //  4

    accesscontrol::RouteMethod method;  // 1

    // Whether REPEATING this request is safe — a property of the route, not of
    // the verb. A POST that is safe to repeat says so and needs no idempotency
    // key; a PUT that is not says so too. Deriving it from the method is how a
    // client's retry becomes a duplicate write.
    bool             idempotent;     //  1

    // Whether this route's PATH may be compiled into a client bundle although
    // the route itself is not Public.
    //
    // It exists because one predicate was answering two questions. "May this
    // path be in the bundle" and "is this route reachable with no credential"
    // are the same question for every route this library had until `/session`,
    // and they come apart there: that route demands a credential, and its
    // address has to be public anyway, because A CLIENT CANNOT BE TOLD WHERE TO
    // ASK FOR THE TABLE UNTIL IT HAS ASKED. The first read of the whole system
    // is otherwise unspellable and the application writes the path by hand,
    // which is the copy that drifts.
    //
    // `/auth/refresh` escaped the problem by being genuinely Public — its
    // credential is a cookie the filter does not read — and that is a
    // coincidence of that route rather than a pattern to copy. The alternative,
    // declaring the route Public and checking the credential in the handler,
    // moves an authentication check out of the filter, which is how a route
    // stops failing closed.
    //
    // It is a DISCLOSURE claim and never an authority one. `access` is emitted
    // unchanged beside it, so the route keeps its real 401 — the answer a client
    // needs to tell "re-authenticate" from "route gone".
    //
    // Legal ONLY on an `Authenticated` policy that requires no bit, which
    // descriptions_match enforces. Not on Guarded or Stealth, because their
    // paths are the map the stealth 404 exists to withhold; and not on Public
    // either, although that one is merely redundant rather than dangerous —
    // there the flag says nothing while looking like it says something, and
    // refusing it keeps exactly one reason per route for a path being in the
    // bundle.
    //
    // Defaulted, along with the two members below it, and the default is the
    // answer for a route that has made none of these claims. That is what lets a
    // table adopt either of them PER ROUTE — the eleven reference routes that
    // have not simply stop writing values — and it is why `-Wmissing-field-
    // initializers` stays quiet over a table that omits them. `idempotent` above
    // deliberately has NO default: it is a decision every route has to make, and
    // a defaulted one would silently answer "repeating this is unsafe" for a
    // route nobody thought about.
    bool             bootstrap = false;      //  1

    // Whether the success body is an ARRAY of the declared shape rather than one
    // object of it. Meaningless without a shape, which descriptions_match
    // refuses.
    bool             response_is_array = false;  // 1

    // The declared shape of this route's success body, EMPTY when the route's
    // response is hand-written and undescribed.
    //
    // Empty is the default and it is not a gap to be closed on a schedule: the
    // emitter writes `"response":null` for such a route and a generated client
    // falls back to its own declaration, which is what every route did before
    // this field existed. A body too rich for the grammar in
    // `http/response_writer.h` — anything nested — stays here as empty and
    // honestly undescribed, rather than approximated into a schema a client
    // would then trust.
    //
    // A span over a namespace-scope `constexpr` array, so the shape stays in
    // `.rodata` and this costs one 16-byte member on a struct nothing reads per
    // request.
    std::span<const http::ResponseField> response = {};  // 16
};

// Six 16-byte members and one word of flags, and the LAST of the six is the one
// out of alignment order that ENGINEERING_RULES.md §3.2 asks for. Two reasons, and the
// second is the one that decided it.
//
// The rule's purpose is to eliminate padding, and appending here eliminates it
// exactly: `limit_max`, the method and the four flags fill one 8-byte word with
// nothing left over, so the span lands naturally aligned at offset 88 and the
// struct is 104 bytes with no hole anywhere. Ordering the span among the views
// instead would produce the same 104 and buy nothing.
//
// And appending is what keeps an application's existing table COMPILING. A
// member inserted in the middle turns every positional row into a build error
// and makes adopting the response binder a flag day for the whole table, which
// is precisely the shape docs/15-tasks.md phase 12 argues against: adoption is
// per route, and a route that has not adopted it says so by leaving two trailing
// values off.
static_assert(sizeof(RouteDescription) ==
                  (6 * sizeof(std::string_view)) + sizeof(std::uint64_t),
              "RouteDescription must not grow padding");
static_assert(sizeof(std::span<const http::ResponseField>) == sizeof(std::string_view),
              "the formula above assumes a span is a pointer and a length");

// Whether a route's path may be compiled into a client bundle.
//
// A Public route is reachable with no credential at all, so its path discloses
// nothing that trying it would not. A `bootstrap` route's path is published for
// the separate reason above. Every other route's path is delivered at run time,
// scoped to the holder asking — a client bundle is a public file, and a
// lazily-loaded chunk is a public URL, so splitting a bundle was never a way to
// keep a path out of one.
//
// ONE predicate, called by the emitter and by the holder-scoped projection
// alike, so "the client already has this path" is decided in one place. It
// replaces `path_is_public(access)`, which answered the authority question and
// was read as the disclosure one.
//
// The two arms are mutually exclusive by construction: descriptions_match
// refuses `bootstrap` on anything but an unpermissioned `Authenticated` policy,
// so a reader of a route table can always tell WHICH reason put a path in the
// bundle.
[[nodiscard]] constexpr bool path_in_bundle(const RouteDescription& description,
                                            accesscontrol::RouteAccess access) noexcept {
    return description.bootstrap || access == accesscontrol::RouteAccess::Public;
}

// Whether `policy` is a shape a `bootstrap` description may name.
//
// Authenticated requires no bit by definition, so `required` is dead weight on
// such a policy and is checked anyway: a table that carries bits there has said
// something the filter ignores, and a disclosure claim is the last place to
// start trusting a field nothing reads.
[[nodiscard]] constexpr bool bootstrap_is_legal(
    const accesscontrol::RoutePolicy& policy) noexcept {
    return policy.access == accesscontrol::RouteAccess::Authenticated &&
           !policy.required.any();
}

// The method as it appears on the wire, and as a client spells it.
//
// `Any` has no wire spelling because it is not a method — it is a declaration
// that every POLICY method shares one policy. A DESCRIPTION can no longer carry
// it: `descriptions_match` refuses one, so the "ANY" spelling below is now
// unreachable from the emitter and is kept only because the switch has to be
// total.
//
// It was the defence before that check existed, and it was the wrong shape of
// defence. A generated client did fail loudly on it rather than defaulting to
// GET — which meant this repository shipped a reference route no client could
// call, and the failure landed on whoever generated the client rather than on
// whoever wrote the table.
[[nodiscard]] constexpr std::string_view method_name(accesscontrol::RouteMethod method) noexcept {
    switch (method) {
        case accesscontrol::RouteMethod::Any:    return "ANY";
        case accesscontrol::RouteMethod::Get:    return "GET";
        case accesscontrol::RouteMethod::Post:   return "POST";
        case accesscontrol::RouteMethod::Put:    return "PUT";
        case accesscontrol::RouteMethod::Patch:  return "PATCH";
        case accesscontrol::RouteMethod::Delete: return "DELETE";
    }
    return "ANY";
}

// The access class, for a client that must know a 404 here is not a 404
// elsewhere: on a Stealth route every denial is the byte-identical not-found
// body, so a client rendering "forbidden" for it rebuilds the oracle the server
// removed.
[[nodiscard]] constexpr std::string_view access_name(accesscontrol::RouteAccess access) noexcept {
    switch (access) {
        case accesscontrol::RouteAccess::Public:        return "public";
        case accesscontrol::RouteAccess::Authenticated: return "authenticated";
        case accesscontrol::RouteAccess::Guarded:       return "guarded";
        case accesscontrol::RouteAccess::Stealth:       return "stealth";
    }
    return "stealth";
}

[[nodiscard]] constexpr const RouteDescription* description_for(
    std::span<const RouteDescription> descriptions, std::string_view id) noexcept {
    for (const RouteDescription& d : descriptions) {
        if (d.id == id) { return &d; }
    }
    return nullptr;
}

// Every route described exactly once, every description naming a route that
// exists, every id present and unique, no description naming `Any`, no
// `bootstrap` claim on a route whose path must not be published, and no
// response shape that a client could not be generated from.
//
// --- the pairing goes through policy_for -----------------------------------
//
// It used to be an equality loop of its own — `r.pattern == d.pattern &&
// r.method == d.method` — which is the filter's question answered a second time,
// in a second place, and the two were already able to disagree. `policy_for`
// resolves an exact method first and falls back to an `Any` entry, so a
// description naming POST against an `Any` policy now pairs the way the filter
// would actually answer it. Two implementations of one resolution are two
// implementations that drift; this one is the filter's own.
//
// --- and why `Any` is refused in a description ------------------------------
//
// A POLICY may be `Any`: one handler that dispatches internally is a real shape,
// and tests/testapp/routes.h declares one on purpose. A DESCRIPTION is what a
// client CALLS with, and `Any` is not something a client can send. The emitter
// spells it "ANY" so a generated client fails loudly, which means the failure
// lands on whoever generates a client rather than on whoever wrote the table —
// and this repository shipped exactly that: a reference route no client could
// call, with every test green.
//
// An application that wants two methods callable declares two policies, which is
// already what routes.h demands of two methods that differ in authority.
//
// --- and why `bootstrap` is checked against the POLICY ----------------------
//
// It is the one field here that makes a claim about DISCLOSURE, and the thing it
// discloses is a path. A table is the wrong place to take that claim on trust:
// marking a Stealth route `bootstrap` would compile its path into a public
// bundle and undo the 404 that route exists for, silently, with every test
// green. So the claim is checked against the policy it names, and the only shape
// that survives is an `Authenticated` route requiring no bit — the shape the
// field was added for.
//
// --- one description per route, which size equality no longer implies -------
//
// Resolving through `policy_for` is what makes that a separate check: two
// descriptions naming two methods of one `Any` policy BOTH pair, and the route
// they left undescribed keeps the sizes equal while disappearing from the
// descriptor. So each route is counted, and it has to be reached exactly once.
//
// That count is also what catches a description naming a route that does not
// exist, and it is worth saying why it is not ALSO spelled out as
// `policy_for(...) == nullptr` beside the per-description checks, where it would
// read far better. **It does not compile.** GCC will not fold a comparison
// between null and a pointer INTO AN OBJECT WITH EXTERNAL LINKAGE into a
// constant expression, and `inline constexpr std::array<RoutePolicy, N>` in an
// application's header is exactly that object — so the `static_assert` this
// function exists for stops working the moment the check is written the obvious
// way. Comparing the same possibly-null pointer against `&route` folds fine,
// which is why the count is expressed that way.
//
// This is the THIRD time this repository has hit the wall, after
// `collection_is_declared` in phase 3 and `validators_are_present` in phase 5,
// and the diagnosis is sharper than either of those recorded: it is not the
// sanitiser, and it is not this compiler being strict about null. It is the
// linkage of the object being pointed INTO. A `well_formed()` that does not
// compile is worse than one that checks the same thing less directly.
//
// The count makes this O(routes x descriptions x routes), which is measured
// rather than guessed: a synthetic table of 256 routes evaluates inside GCC's
// default `-fconstexpr-ops-limit` and one of 512 does not. An application past
// that raises the limit; it does not get a weaker check.
[[nodiscard]] constexpr bool descriptions_match(
    std::span<const accesscontrol::RoutePolicy> routes,
    std::span<const RouteDescription> descriptions) noexcept {
    if (routes.size() != descriptions.size()) { return false; }

    for (std::size_t i = 0; i < descriptions.size(); ++i) {
        const RouteDescription& d = descriptions[i];
        if (d.id.empty() || d.pattern.empty()) { return false; }

        // Not a method, so not something a client can be told to send.
        if (d.method == accesscontrol::RouteMethod::Any) { return false; }

        // A list route needs both halves or neither: a cursor with no ceiling is
        // an unbounded result set, and a ceiling with no cursor is a page nobody
        // can advance.
        if (d.cursor_field.empty() != (d.limit_max == 0)) { return false; }

        // A declared shape has to be usable — no empty, non-UTF-8 or duplicated
        // key — and `response_is_array` has to have a shape to be about. The
        // second is the one worth checking: a route that said "array" and
        // declared nothing would emit a schema claiming a list of an unknown
        // thing, which a generated client would render as a list of nothing.
        if (!http::response_shape_is_well_formed(d.response)) { return false; }
        if (d.response_is_array && d.response.empty()) { return false; }

        for (std::size_t j = i + 1; j < descriptions.size(); ++j) {
            if (d.id == descriptions[j].id) { return false; }
            // On (pattern, method) and never on the pattern alone: one
            // description standing for two routes that differ in authority is
            // the case a route table gets wrong, and tests/testapp/routes.h has
            // exactly that shape on purpose.
            if (d.pattern == descriptions[j].pattern &&
                d.method == descriptions[j].method) {
                return false;
            }
        }
    }

    for (const accesscontrol::RoutePolicy& route : routes) {
        std::size_t described = 0;
        for (const RouteDescription& d : descriptions) {
            if (accesscontrol::policy_for(routes, d.pattern, d.method) == &route) {
                ++described;
                // The bootstrap guard lives HERE rather than in the loop above,
                // and not for tidiness: it needs the policy, and resolving one
                // up there means comparing `policy_for(...)` against `nullptr`,
                // which GCC will not fold into a constant expression when the
                // route table is an object with external linkage. Against
                // `&route` it folds, which is the same wall the count above
                // already walks around.
                if (d.bootstrap && !bootstrap_is_legal(route)) { return false; }
            }
        }
        if (described != 1) { return false; }
    }

    return true;
}

}  // namespace anvil::descriptor
