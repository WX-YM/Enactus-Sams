#include "anvil/accesscontrol/route_projection.h"

#include "anvil/accesscontrol/decision.h"
#include "anvil/http/json_writer.h"

namespace anvil::accesscontrol {

void append_reachable_routes(std::string& out, std::span<const RoutePolicy> routes,
                             std::span<const descriptor::RouteDescription> descriptions,
                             const PermSet& held, UserType type, bool include_bundled) {
    // One reservation for the whole object. The upper bound is every route, and
    // a route entry is an id, a method and a path plus six punctuation bytes;
    // 96 bytes each is generous for the tables this is built for and is one
    // allocation either way.
    out.reserve(out.size() + (descriptions.size() * 96) + 2);

    out += '{';
    bool first = true;

    for (const descriptor::RouteDescription& d : descriptions) {
        const RoutePolicy* policy = policy_for(routes, d.pattern, d.method);
        if (policy == nullptr) { continue; }

        // One predicate, shared with the emitter, so "the client already has this
        // path" is decided in one place rather than by a Public test here and a
        // different one there.
        if (!include_bundled && descriptor::path_in_bundle(d, policy->access)) { continue; }
        if (!reachable(*policy, held, type)) { continue; }

        if (!first) { out += ','; }
        first = false;

        http::append_json_key(out, d.id);

        // Method and path in one string rather than an object, because the
        // client needs both and never one: a table of two-field objects is three
        // extra bytes of punctuation per route on a response that is sent to
        // every signed-in tab.
        out += '"';
        out += descriptor::method_name(d.method);
        out += ' ';
        // The pattern is a compile-time constant from the route table and never
        // request data, but it goes through the escaper anyway: the day one of
        // these is built from configuration, the escape is already there.
        std::string escaped;
        http::append_json_string(escaped, d.pattern);
        out.append(escaped, 1, escaped.size() - 2);
        out += '"';
    }

    out += '}';
}

void append_holder_authority(std::string& out, const PermSet& held, UserType type,
                             const PermCatalogue& catalogue) {
    out += '{';
    http::append_json_key(out, "superadmin");
    // The same predicate the filter's short-circuit reads. A second test of "is
    // this account a superadmin" is a second one to keep in agreement, and the
    // one that drifts is the one no attacker is reading.
    out += is_superadmin(type) ? "true" : "false";
    out += ',';
    http::append_json_key(out, "perms");
    out += '[';
    bool first = true;
    // BIT order, and only DECLARED bits: for_each_name skips the reserved gaps
    // between an application's blocks, so a bit nothing names never reaches a
    // client as a control that authorises nothing.
    catalogue.for_each_name(held, [&out, &first](std::string_view name) {
        if (!first) { out += ','; }
        first = false;
        http::append_json_string(out, name);
    });
    out += ']';
    out += '}';
}

}  // namespace anvil::accesscontrol
