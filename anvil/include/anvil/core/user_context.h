#pragma once

// What the access filter proves about a request.
//
// Drogon's req->attributes() is a std::map<std::string, std::any>: every
// attribute costs a string compare per level of the tree, a map node, and — a
// shared_ptr does not fit std::any's small-buffer slot — a heap allocation
// beside the control block. Five attributes cost five of each, on every
// protected request. So there is ONE, and this struct is its first member rather
// than its whole contents: http/request_scope.h holds the attribute, and it
// carries the request id past the cache line this occupies.
//
// Layout is load-bearing, not incidental. This struct is read on every protected
// request and crosses thread-pool boundaries by value, so it is ordered
// largest-alignment-first, fits one 64-byte cache line, and is trivially
// copyable. The static_asserts make a future field addition a deliberate act
// rather than a silent regression.

#include <array>
#include <cstdint>
#include <type_traits>

#include "anvil/core/locale.h"
#include "anvil/core/perm_set.h"
#include "anvil/core/types.h"

namespace anvil {

struct UserContext final {
    Uuid          user_id;      // 16  UUID bytes, not a string
    Uuid          session_id;   // 16
    PermSet       permissions;  // 16  one AND per check
    std::uint64_t perm_epoch;   //  8  the revocation channel
    UserType      user_type;    //  1
    Locale        locale;       //  1
    // Keeps sizeof stable across additions: a field added here is free until the
    // reserve is spent, and spending it is the moment to re-measure rather than
    // to quietly cross a cache line.
    std::array<std::uint8_t, 6> reserved;  // 6
};

static_assert(sizeof(UserContext) == 64, "UserContext must fit one cache line");
static_assert(alignof(UserContext) == 8);
static_assert(std::is_trivially_copyable_v<UserContext>,
              "it must cross thread-pool boundaries by value with no synchronisation");

// Deliberately NOT asserted trivial: Locale has a user-provided default
// constructor, because only validating constructors may produce one. Trivially
// COPYABLE is the property that matters here, and it holds.

// The token's issued-at is deliberately not a member. Carrying it would push this
// struct to 72 bytes and across a second cache line, and nothing on the request
// path reads it — expiry is enforced during token verification, before the
// context is built.

// The key this used to declare is now `anvil::http::kRequestScopeKey`, in
// http/request_scope.h, beside the struct that carries this one as its first
// member. It moved because the attribute no longer holds a UserContext: it holds
// a RequestScope, whose other half is the request id, and a key declared beside
// a type it does not name is the next reader's wrong turn.
//
// There is deliberately nothing left here — not a renamed alias, not a
// deprecated constant. There were two constants naming this attribute once:
// accesscontrol/access_filter.h held a second one with a different string, and
// that was the one the filter actually wrote, so code written against this
// header read an attribute nothing had ever set and got a null context back with
// no error anywhere. Leaving a second spelling behind here is that trap rebuilt
// by hand.
//
// What has not moved is the argument for there being ONE attribute, which is at
// the top of this file and is why the id shares this one rather than taking a
// second.

}  // namespace anvil
