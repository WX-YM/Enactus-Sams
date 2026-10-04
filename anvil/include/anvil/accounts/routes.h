#pragma once

// The account flows on the wire: one Drogon handler per role, installed at the
// path the APPLICATION declared for the route id it gave that role
// (schema.h's AccountDescription), through register_route so each gets exactly
// the filter its policy names.
//
// What a handler does is the same for every role and is the whole of this
// file's job: parse the body through input/json.h (limits enforced during the
// parse, duplicate keys refused, `as_string()` the type assertion), hand the
// strings to AccountService, and write the answer — the success body, the
// session cookies, or append_error_body. Nothing here decides anything about an
// account; the service does.
//
// The bodies are docs/05-auth-sessions.md §13's table. Which secret field a
// body carries — `credential` or `password` — follows the service's hashing and
// nothing else: a request that sent the other one is answered as malformed
// rather than guessed at.

#include <span>

#include "anvil/accesscontrol/route_registry.h"
#include "anvil/accounts/service.h"
#include "anvil/descriptor/route_description.h"

namespace anvil::accounts {

// Registers a handler for every role the service's description names, at the
// pattern and method its RouteDescription gives. Throws std::invalid_argument
// when a role names a route id the table does not describe — at boot, where a
// missing route is a message, rather than as a 404 somebody reports.
//
// `service` must outlive the process's HTTP serving: the handlers hold it by
// reference, as every Drogon handler in an application holds its services.
void install_account_routes(const AccountService& service,
                            std::span<const accesscontrol::RoutePolicy> routes,
                            std::span<const descriptor::RouteDescription> descriptions);

}  // namespace anvil::accounts
