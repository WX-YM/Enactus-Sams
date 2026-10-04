#pragma once

// The identifier kinds an account can be looked up by, in a header of its own so
// that code with no database — the account schema, the descriptor writer — can
// name one without pulling the driver in through identity/users.h.

#include <cstdint>

namespace anvil::identity {

// Which unique index answers a login lookup. The branch is chosen from the
// identifier's SHAPE before the query is issued: one equality on one field,
// never an `$or` across three. An `$or` cannot use a single index and turns the
// login path — the one path an attacker can drive hardest — into a scan.
enum class LoginIdentity : std::uint8_t { Email = 0, Username = 1, Phone = 2 };

}  // namespace anvil::identity
