// Access control, for everything that does not need an HTTP stack: the epoch
// cache, the cookie reader, the authorization decision, the route registry, and
// the origin check.
//
// One case at the end does construct a Drogon request, because the property it
// asserts is about Drogon's attribute map and cannot be stated anywhere else.
//
// The stealth-404 timing claim is a claim about accesscontrol::evaluate(), not
// about Drogon, so it is measured here. Measuring it through a socket would
// measure the socket.

#include <gtest/gtest.h>

#include "perms.h"
#include "route_descriptions.h"
#include "routes.h"

#include "anvil/core/locale.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/cookies.h"
#include "anvil/accesscontrol/decision.h"
#include "anvil/accesscontrol/epoch_resolver.h"
#include "anvil/accesscontrol/route_projection.h"
#include "anvil/accesscontrol/route_registry.h"
#include "anvil/auth/epoch_cache.h"
#include "anvil/descriptor/route_description.h"
#include "anvil/auth/token.h"
#include "anvil/core/user_context.h"
#include "anvil/core/uuid.h"
#include "anvil/http/origin_check.h"
#include "anvil/http/request_id.h"
#include "anvil/http/request_scope.h"

namespace anvil {

using testapp::Perm;
namespace {

using auth::EpochCache;

[[nodiscard]] std::array<std::uint8_t, 32> test_key(std::uint8_t seed) {
    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(seed + i);
    }
    return key;
}

// ---------------------------------------------------------------------------
// The local epoch cache
// ---------------------------------------------------------------------------

TEST(EpochCache, HitReturnsTheStoredEpoch) {
    EpochCache cache{std::chrono::milliseconds{10'000}};
    const Uuid user = uuid::generate_v7();
    const auto now = EpochCache::Clock::now();

    EXPECT_FALSE(cache.get(user, now).has_value());
    cache.put(user, 42, now);
    ASSERT_TRUE(cache.get(user, now).has_value());
    EXPECT_EQ(*cache.get(user, now), 42U);
}

// Case 5 — a permission removed is honoured within the cache TTL, which means
// the entry must actually stop answering.
TEST(EpochCache, EntryExpiresAtTheTtl) {
    EpochCache cache{std::chrono::milliseconds{10'000}};
    const Uuid user = uuid::generate_v7();
    const auto now = EpochCache::Clock::now();

    cache.put(user, 7, now);
    EXPECT_TRUE(cache.get(user, now + std::chrono::milliseconds{9'999}).has_value());
    EXPECT_FALSE(cache.get(user, now + std::chrono::milliseconds{10'000}).has_value());
    EXPECT_FALSE(cache.get(user, now + std::chrono::seconds{30}).has_value());
}

// A slot collision must never answer for the wrong user, however fresh it is.
TEST(EpochCache, ADifferentUserInTheSlotIsAMissNotAWrongAnswer) {
    EpochCache cache{std::chrono::milliseconds{10'000}};
    const auto now = EpochCache::Clock::now();

    // Fill far more slots than the cache holds; every entry must be either this
    // user's own epoch or absent, never another user's.
    std::vector<Uuid> users;
    users.reserve(EpochCache::kSlots * 4);
    for (std::size_t i = 0; i < EpochCache::kSlots * 4; ++i) {
        users.push_back(uuid::generate_v7());
        cache.put(users.back(), i, now);
    }
    for (std::size_t i = 0; i < users.size(); ++i) {
        const std::optional<std::uint64_t> cached = cache.get(users[i], now);
        if (cached.has_value()) { EXPECT_EQ(*cached, i); }
    }
}

// Case 6 — fixed capacity. The key is attacker-chosen, so growth would be a
// memory-exhaustion vector reachable with forged tokens.
TEST(EpochCache, CapacityIsFixedAndEvictionIsTheFailureMode) {
    EpochCache cache{std::chrono::milliseconds{10'000}};
    const auto now = EpochCache::Clock::now();
    EXPECT_EQ(cache.capacity(), EpochCache::kSlots);

    std::size_t resident = 0;
    std::vector<Uuid> users;
    users.reserve(EpochCache::kSlots * 8);
    for (std::size_t i = 0; i < EpochCache::kSlots * 8; ++i) {
        users.push_back(uuid::generate_v7());
        cache.put(users.back(), 1, now);
    }
    for (const Uuid& user : users) {
        if (cache.get(user, now).has_value()) { ++resident; }
    }
    EXPECT_LE(resident, EpochCache::kSlots);
}

TEST(EpochCache, InvalidateDropsOnlyTheNamedUser) {
    EpochCache cache{std::chrono::milliseconds{10'000}};
    const Uuid kept = uuid::generate_v7();
    const Uuid dropped = uuid::generate_v7();
    const auto now = EpochCache::Clock::now();

    cache.put(kept, 1, now);
    cache.put(dropped, 2, now);
    cache.invalidate(dropped);

    EXPECT_TRUE(cache.get(kept, now).has_value());
    EXPECT_FALSE(cache.get(dropped, now).has_value());
}

// ---------------------------------------------------------------------------
// The cookie reader
// ---------------------------------------------------------------------------

TEST(Cookies, ReadsTheNamedValue) {
    using accesscontrol::read_cookie;
    EXPECT_EQ(read_cookie("__Host-at=abc", "__Host-at"), "abc");
    EXPECT_EQ(read_cookie("a=1; __Host-at=abc; b=2", "__Host-at"), "abc");
    EXPECT_EQ(read_cookie("a=1;__Host-at=abc", "__Host-at"), "abc");
    EXPECT_EQ(read_cookie("  __Host-at=abc  ", "__Host-at"), "abc  ");
}

TEST(Cookies, AbsentOrMalformedYieldsEmpty) {
    using accesscontrol::read_cookie;
    EXPECT_TRUE(read_cookie("", "__Host-at").empty());
    EXPECT_TRUE(read_cookie("b=2", "__Host-at").empty());
    EXPECT_TRUE(read_cookie("__Host-at", "__Host-at").empty());
    EXPECT_TRUE(read_cookie("__Host-at=", "__Host-at").empty());
}

// Cookie names are case-sensitive, and so is the `__Host-` prefix a browser
// enforces. Matching loosely would accept a name a subdomain is allowed to set.
TEST(Cookies, MatchingIsCaseSensitive) {
    EXPECT_TRUE(accesscontrol::read_cookie("__host-at=abc", "__Host-at").empty());
}

// A hostile 64 KB cookie costs a length compare, not a scan.
TEST(Cookies, OversizedHeaderIsRejectedOnLength) {
    const std::string huge(accesscontrol::kMaxCookieHeaderBytes + 1, 'a');
    EXPECT_TRUE(accesscontrol::read_cookie(huge, "__Host-at").empty());
}

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

class FakeEpochs final : public accesscontrol::EpochResolver {
public:
    accesscontrol::EpochVerdict verdict{accesscontrol::EpochVerdict::Match};
    mutable int                 cached_calls{0};
    int                         async_calls{0};

    [[nodiscard]] accesscontrol::EpochVerdict check_cached(
        const Uuid&, std::uint64_t) const noexcept override {
        ++cached_calls;
        return verdict;
    }
    void resolve_async(const Uuid&,
                       std::function<void(Result<std::uint64_t>)> done) override {
        ++async_calls;
        done(fail(ErrorCode::ServiceUnavailable));
    }
};

class DecisionTest : public ::testing::Test {
protected:
    DecisionTest()
        : keys_{1, test_key(7)},
          now_{1'900'000'000},
          stealth_{perm_mask(Perm::ContentWrite), "/content/edit",
                   accesscontrol::RouteAccess::Stealth} {}

    [[nodiscard]] std::string cookie_for(const PermSet& permissions, UserType type,
                                         std::uint64_t epoch, std::int64_t expires_at) const {
        const auth::AccessClaims claims{
            .user_id = user_,
            .session_id = session_,
            .permissions = permissions,
            .perm_epoch = epoch,
            .expires_at = static_cast<std::uint32_t>(expires_at),
            .user_type = type,
            .locale = *Locale::from_tag("en"),
            .reserved = {},
        };
        return "__Host-at=" + auth::encode(claims, keys_);
    }

    const auth::TokenKeys           keys_;
    const std::int64_t              now_;
    const accesscontrol::RoutePolicy stealth_;
    FakeEpochs                      epochs_;
    const Uuid                      user_{uuid::generate_v7()};
    const Uuid                      session_{uuid::generate_v7()};
};

TEST_F(DecisionTest, AllowsWhenPermissionsAndEpochAgree) {
    const std::string cookie =
        cookie_for(perm_mask(Perm::ContentWrite, Perm::ContentRead), UserType::Staff, 5,
                   now_ + 900);
    const accesscontrol::Evaluation evaluation =
        evaluate(cookie, stealth_, keys_, epochs_, now_);

    EXPECT_EQ(evaluation.step, accesscontrol::Step::Allow);
    EXPECT_EQ(evaluation.code, ErrorCode::Ok);
    EXPECT_EQ(evaluation.ctx.user_id, user_);
    EXPECT_EQ(evaluation.ctx.session_id, session_);
}

TEST_F(DecisionTest, SubsetOfRequiredPermissionsIsDenied) {
    const std::string cookie =
        cookie_for(perm_mask(Perm::ContentRead), UserType::Staff, 5, now_ + 900);
    const accesscontrol::Evaluation evaluation =
        evaluate(cookie, stealth_, keys_, epochs_, now_);

    EXPECT_EQ(evaluation.step, accesscontrol::Step::Deny);
    // The TRUE code, even though the client will receive a 404.
    EXPECT_EQ(evaluation.code, ErrorCode::Forbidden);
}

TEST_F(DecisionTest, MissingCookieIsUnauthenticated) {
    const accesscontrol::Evaluation evaluation = evaluate("", stealth_, keys_, epochs_, now_);
    EXPECT_EQ(evaluation.step, accesscontrol::Step::Deny);
    EXPECT_EQ(evaluation.code, ErrorCode::Unauthenticated);
}

TEST_F(DecisionTest, ExpiredTokenIsDenied) {
    const std::string cookie =
        cookie_for(perm_mask(Perm::ContentWrite), UserType::Staff, 5, now_ - 3600);
    const accesscontrol::Evaluation evaluation =
        evaluate(cookie, stealth_, keys_, epochs_, now_);
    EXPECT_EQ(evaluation.step, accesscontrol::Step::Deny);
}

// A token minted before a permission change must stop working within one cache
// TTL rather than at its own expiry: the epoch is what makes a revocation take
// effect before the credential it revokes runs out on its own.
TEST_F(DecisionTest, EpochMismatchIsDeniedEvenWithTheRightPermissions) {
    epochs_.verdict = accesscontrol::EpochVerdict::Mismatch;
    const std::string cookie =
        cookie_for(perm_mask(Perm::ContentWrite), UserType::Staff, 5, now_ + 900);
    const accesscontrol::Evaluation evaluation =
        evaluate(cookie, stealth_, keys_, epochs_, now_);

    EXPECT_EQ(evaluation.step, accesscontrol::Step::Deny);
    EXPECT_EQ(evaluation.code, ErrorCode::Unauthenticated);
}

TEST_F(DecisionTest, UnknownEpochDefersRatherThanGuessing) {
    epochs_.verdict = accesscontrol::EpochVerdict::Unknown;
    const std::string cookie =
        cookie_for(perm_mask(Perm::ContentWrite), UserType::Staff, 5, now_ + 900);
    const accesscontrol::Evaluation evaluation =
        evaluate(cookie, stealth_, keys_, epochs_, now_);

    ASSERT_EQ(evaluation.step, accesscontrol::Step::ResolveEpoch);
    EXPECT_EQ(evaluation.token_epoch, 5U);

    // The resume path applies the same permission check as the synchronous one.
    EXPECT_EQ(resume_after_epoch(evaluation, stealth_, 5).step, accesscontrol::Step::Allow);
    EXPECT_EQ(resume_after_epoch(evaluation, stealth_, 6).step, accesscontrol::Step::Deny);
}

// docs/04-access-control.md §2 — superadmin is an explicit utype check, NOT an all-ones mask, so
// "holds every permission" and "is superadmin" stay distinguishable.
TEST_F(DecisionTest, SuperadminShortCircuitsWithoutSynthesisingAMask) {
    const std::string cookie = cookie_for(PermSet{}, UserType::SuperAdmin, 5, now_ + 900);
    const accesscontrol::Evaluation evaluation =
        evaluate(cookie, stealth_, keys_, epochs_, now_);

    EXPECT_EQ(evaluation.step, accesscontrol::Step::Allow);
    // The stored mask is still empty. Nothing manufactured permissions.
    EXPECT_TRUE(evaluation.ctx.permissions.none());
}

// Case 3 — the deny path performs no lookup at all when there is no token,
// which is what keeps a denial and a nonexistent route indistinguishable.
TEST_F(DecisionTest, DenyWithoutATokenConsultsNoEpochAuthority) {
    (void)evaluate("", stealth_, keys_, epochs_, now_);
    EXPECT_EQ(epochs_.cached_calls, 0);
    EXPECT_EQ(epochs_.async_calls, 0);
}

// A public route recognises a signed-in visitor but never denies, and never
// consults the epoch authority — doing so would put a Redis GET behind
// unauthenticated traffic for no security benefit.
TEST_F(DecisionTest, PublicRouteAllowsWithAndWithoutAToken) {
    const accesscontrol::RoutePolicy pub{PermSet{}, "/login",
                                         accesscontrol::RouteAccess::Public};

    const accesscontrol::Evaluation anonymous = evaluate("", pub, keys_, epochs_, now_);
    EXPECT_EQ(anonymous.step, accesscontrol::Step::Allow);
    EXPECT_TRUE(is_nil(anonymous.ctx.user_id));

    const std::string cookie = cookie_for(PermSet{}, UserType::Client, 5, now_ + 900);
    const accesscontrol::Evaluation signed_in = evaluate(cookie, pub, keys_, epochs_, now_);
    EXPECT_EQ(signed_in.step, accesscontrol::Step::Allow);
    EXPECT_EQ(signed_in.ctx.user_id, user_);
    EXPECT_EQ(epochs_.cached_calls, 0);

    // A garbage token on a public route is ignored, not an error.
    const accesscontrol::Evaluation garbage =
        evaluate("__Host-at=not-a-token", pub, keys_, epochs_, now_);
    EXPECT_EQ(garbage.step, accesscontrol::Step::Allow);
    EXPECT_TRUE(is_nil(garbage.ctx.user_id));
}

// A token signed with a key we do not hold must not be distinguishable, to the
// client, from one whose signature simply failed.
TEST_F(DecisionTest, ForeignKeyAndBadSignatureBothDenyIdentically) {
    const auth::TokenKeys other{1, test_key(99)};
    const auth::AccessClaims claims{
        .user_id = user_,
        .session_id = session_,
        .permissions = perm_mask(Perm::ContentWrite),
        .perm_epoch = 5,
        .expires_at = static_cast<std::uint32_t>(now_ + 900),
        .user_type = UserType::Staff,
        .locale = *Locale::from_tag("en"),
        .reserved = {},
    };
    const std::string cookie = "__Host-at=" + auth::encode(claims, other);
    const accesscontrol::Evaluation evaluation =
        evaluate(cookie, stealth_, keys_, epochs_, now_);

    EXPECT_EQ(evaluation.step, accesscontrol::Step::Deny);
    EXPECT_EQ(evaluation.code, ErrorCode::Unauthenticated);
}

// Case 2 — the timing distributions of a denial and of a nonexistent route must
// overlap. Structurally they must: both are pure CPU with no I/O. This asserts
// the structural claim rather than a wall-clock one, which would be flaky under
// ASan on a shared CI machine.
TEST_F(DecisionTest, DenyAndAllowBothCompleteWithoutIo) {
    const std::string denied =
        cookie_for(perm_mask(Perm::ContentRead), UserType::Staff, 5, now_ + 900);
    const std::string allowed =
        cookie_for(perm_mask(Perm::ContentWrite), UserType::Staff, 5, now_ + 900);

    for (int i = 0; i < 1000; ++i) {
        ASSERT_NE(evaluate(denied, stealth_, keys_, epochs_, now_).step,
                  accesscontrol::Step::ResolveEpoch);
        ASSERT_NE(evaluate(allowed, stealth_, keys_, epochs_, now_).step,
                  accesscontrol::Step::ResolveEpoch);
    }
    // Every one of those 2000 decisions consulted only the LOCAL cache.
    //
    // The step, and NOT epochs_.async_calls: evaluate() has no resolver call in
    // it to make, so a zero there is a tautology. ResolveEpoch is the return
    // value that puts a round trip on the request, so that is what has to be
    // absent for this to mean anything.
    EXPECT_EQ(epochs_.cached_calls, 2000);
}

// The other half of case 2, and the one that was missing: the structural claim
// has to hold when the authority is NOT cached, which is the state every user is
// in on their first request after a deploy, an eviction or a TTL expiry.
//
// Deferring there made a denied stealth route cost a Redis GET while a
// nonexistent route cost nothing, so an attacker holding any valid token could
// separate the two with a stopwatch and map the admin surface — the exact
// existence oracle stealth exists to close (docs/04-access-control.md §3).
TEST_F(DecisionTest, StealthDenialNeverDefersWhateverTheCachedVerdict) {
    const std::string denied =
        cookie_for(perm_mask(Perm::ContentRead), UserType::Staff, 5, now_ + 900);

    for (const accesscontrol::EpochVerdict verdict :
         {accesscontrol::EpochVerdict::Match, accesscontrol::EpochVerdict::Mismatch,
          accesscontrol::EpochVerdict::Unknown}) {
        epochs_.verdict = verdict;
        const accesscontrol::Evaluation evaluation =
            evaluate(denied, stealth_, keys_, epochs_, now_);
        EXPECT_EQ(evaluation.step, accesscontrol::Step::Deny)
            << "cached verdict " << static_cast<int>(verdict);
    }
}

// An uncached epoch on a stealth route still defers for a token that DOES hold
// the bits — the revocation channel is not weakened, only the denial is made
// cheap. This is the case that must keep costing a round trip.
TEST_F(DecisionTest, StealthAllowStillResolvesAnUncachedEpoch) {
    epochs_.verdict = accesscontrol::EpochVerdict::Unknown;
    const std::string allowed =
        cookie_for(perm_mask(Perm::ContentWrite), UserType::Staff, 5, now_ + 900);

    EXPECT_EQ(evaluate(allowed, stealth_, keys_, epochs_, now_).step,
              accesscontrol::Step::ResolveEpoch);
}

// The short-circuit is stealth-ONLY, on purpose. A guarded route's 401 and 403
// are both visible to the client, and the 401 is load-bearing: a permission that
// was just GRANTED reaches the user as 401 -> refresh -> retry. Answering 403
// from the stale token would strand them until it expired.
TEST_F(DecisionTest, GuardedDenialStillDefersSoARefreshCanRecoverIt) {
    const accesscontrol::RoutePolicy guarded{perm_mask(Perm::ContentWrite), "/content/edit",
                                             accesscontrol::RouteAccess::Guarded};
    epochs_.verdict = accesscontrol::EpochVerdict::Unknown;
    const std::string denied =
        cookie_for(perm_mask(Perm::ContentRead), UserType::Staff, 5, now_ + 900);

    EXPECT_EQ(evaluate(denied, guarded, keys_, epochs_, now_).step,
              accesscontrol::Step::ResolveEpoch);
}

// ---------------------------------------------------------------------------
// Every route declares a policy
// ---------------------------------------------------------------------------

TEST(RouteRegistry, EveryEntryIsResolvableByItsOwnPatternAndMethod) {
    for (const accesscontrol::RoutePolicy& route : testapp::kRoutes) {
        const accesscontrol::RoutePolicy* found =
            accesscontrol::policy_for(testapp::kRoutes, route.pattern, route.method);
        ASSERT_NE(found, nullptr) << route.pattern;
        EXPECT_EQ(found->pattern, route.pattern);
        EXPECT_EQ(found->method, route.method);
    }
}

// The key is (pattern, method), not the pattern alone: `GET /media/{ns}/{id}`
// is public while `DELETE` on the same pattern requires MediaDelete, and one
// policy per pattern would have to weaken one of the two.
TEST(RouteRegistry, PatternAndMethodPairsAreUnique) {
    std::vector<std::pair<std::string_view, accesscontrol::RouteMethod>> keys;
    keys.reserve(testapp::kRoutes.size());
    for (const accesscontrol::RoutePolicy& route : testapp::kRoutes) {
        keys.emplace_back(route.pattern, route.method);
    }
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(std::adjacent_find(keys.begin(), keys.end()), keys.end());
}

// A pattern that declares only method-specific entries must DENY every other
// method rather than falling back to the most permissive one it has.
TEST(RouteRegistry, AnUndeclaredMethodOnADeclaredPatternHasNoPolicy) {
    using accesscontrol::RouteMethod;
    ASSERT_NE(accesscontrol::policy_for(testapp::kRoutes, "/media/{ns}/{id}", RouteMethod::Get), nullptr);
    ASSERT_NE(accesscontrol::policy_for(testapp::kRoutes, "/media/{ns}/{id}", RouteMethod::Delete), nullptr);
    EXPECT_EQ(accesscontrol::policy_for(testapp::kRoutes, "/media/{ns}/{id}", RouteMethod::Put), nullptr);
    EXPECT_EQ(accesscontrol::policy_for(testapp::kRoutes, "/media/{ns}/{id}", RouteMethod::Post), nullptr);

    // What each method REQUIRES is the application's claim, not anvil's, and
    // tests/testapp/routes.h static_asserts it over its own table. What is
    // asserted here is the lookup: a declared method resolves and an undeclared
    // one does not, whatever the entries happen to say.
}

// An entry declared for one method must not be reachable through another, and
// an `Any` entry must answer every method (the shape every Phase 3 route uses).
TEST(RouteRegistry, MethodSpecificEntriesDoNotLeakAcrossMethods) {
    using accesscontrol::RouteMethod;
    for (const RouteMethod method :
         {RouteMethod::Get, RouteMethod::Post, RouteMethod::Delete, RouteMethod::Put}) {
        EXPECT_NE(accesscontrol::policy_for(testapp::kRoutes, "/session/logout", method), nullptr);
    }
    EXPECT_EQ(accesscontrol::method_from_string("DELETE"), RouteMethod::Delete);
    EXPECT_EQ(accesscontrol::method_from_string("HEAD"), RouteMethod::Get);
    // An unrecognised verb matches only an `Any` entry, so a media route denies
    // it rather than picking one of its two policies.
    EXPECT_EQ(accesscontrol::policy_for(testapp::kRoutes, "/media/{ns}/{id}", accesscontrol::method_from_string("TRACE")),
              nullptr);
}

TEST(RouteRegistry, AnUndeclaredRouteIsNotSilentlyPermitted) {
    // Patterns no phase will ever declare, rather than ones a later phase
    // might. An earlier version of this case named a pattern that a later phase
    // then registered, at which point it started asserting the opposite of what
    // it means — silently, because "is not declared" keeps passing right up
    // until the moment it is wrong.
    EXPECT_FALSE(accesscontrol::is_pattern_declared(testapp::kRoutes, "/nope"));
    EXPECT_FALSE(accesscontrol::is_pattern_declared(testapp::kRoutes, "/sectionsdata/{key}/../secret"));
    EXPECT_EQ(accesscontrol::policy_for(testapp::kRoutes, "/nope"), nullptr);
}

// `testapp::kRoutes` declares its own size, and an array with FEWER initialisers
// than that size compiles: the remainder is value-initialised, which is an entry
// with an empty pattern, no permissions and — because Public is enumerator 0 —
// PUBLIC access. One fewer than the truth is therefore not a
// build error, and this is what makes it a test failure instead. One MORE than
// the truth already is a build error, which is the other half of the pin.
TEST(RouteRegistry, NoEntryIsValueInitialisedByAnOverCountedSize) {
    for (const accesscontrol::RoutePolicy& route : testapp::kRoutes) {
        ASSERT_FALSE(route.pattern.empty())
            << "testapp::kRoutes is declared larger than its initialiser list, and the gap is a "
               "public route with no pattern";
        EXPECT_EQ(route.pattern.front(), '/') << route.pattern;
    }
}

// A route that requires no permissions must not be marked Stealth: stealth is
// about hiding a permission failure, and there is none to hide.
TEST(RouteRegistry, StealthRoutesRequireAPermission) {
    for (const accesscontrol::RoutePolicy& route : testapp::kRoutes) {
        if (route.access == accesscontrol::RouteAccess::Stealth) {
            EXPECT_TRUE(route.required.any()) << route.pattern;
        }
    }
}

// ---------------------------------------------------------------------------
// The origin check
// ---------------------------------------------------------------------------

TEST(OriginCheck, ExactMatchOnly) {
    constexpr std::array<std::string_view, 1> allowed{"https://example.test"};

    EXPECT_EQ(http::check_origin("POST", "https://example.test", allowed),
              http::OriginVerdict::Allowed);
    EXPECT_EQ(http::check_origin("POST", "https://evil.test", allowed),
              http::OriginVerdict::Mismatched);
    // Both of these satisfy a naive suffix test and neither is ours.
    EXPECT_EQ(http::check_origin("POST", "https://evil-example.test", allowed),
              http::OriginVerdict::Mismatched);
    EXPECT_EQ(http::check_origin("POST", "https://example.test.attacker.test", allowed),
              http::OriginVerdict::Mismatched);
    // A scheme downgrade is a different origin.
    EXPECT_EQ(http::check_origin("POST", "http://example.test", allowed),
              http::OriginVerdict::Mismatched);
    // OUR OWN content origin, and still a rejection. It serves uploaded bytes
    // and unpublished drafts, so a script that does run there must not be able
    // to reach a state change — which is the whole reason it is a separate host
    // and not a path on this one. A same-site sibling is exactly the
    // case SameSite=Lax does not cover, so this check is what covers it.
    EXPECT_EQ(http::check_origin("POST", "https://www.example.test", allowed),
              http::OriginVerdict::Mismatched);
}

// Case 4 — absence on a state-changing method is a rejection, not a pass.
TEST(OriginCheck, AbsentOriginOnAMutatingMethodIsRejected) {
    constexpr std::array<std::string_view, 1> allowed{"https://example.test"};

    EXPECT_EQ(http::check_origin("POST", "", allowed), http::OriginVerdict::Missing);
    EXPECT_EQ(http::check_origin("DELETE", "", allowed), http::OriginVerdict::Missing);
    // "null" is what a sandboxed iframe or a redirected cross-origin form sends.
    EXPECT_EQ(http::check_origin("POST", "null", allowed), http::OriginVerdict::Missing);
    EXPECT_TRUE(http::is_rejection(http::check_origin("POST", "", allowed)));
}

TEST(OriginCheck, SafeMethodsNeedNoOrigin) {
    constexpr std::array<std::string_view, 1> allowed{"https://example.test"};

    EXPECT_EQ(http::check_origin("GET", "", allowed), http::OriginVerdict::NotRequired);
    EXPECT_EQ(http::check_origin("HEAD", "", allowed), http::OriginVerdict::NotRequired);
    // OPTIONS must not confirm anything about an admin route either, so it
    // is never rejected on origin grounds — it is simply not answered with CORS.
    EXPECT_EQ(http::check_origin("OPTIONS", "https://evil.test", allowed),
              http::OriginVerdict::NotRequired);
    EXPECT_FALSE(http::is_rejection(http::check_origin("GET", "", allowed)));
}

// --- the re-check, against the decision it must not be able to disagree with -

TEST_F(DecisionTest, TheConnectionRecheckAgreesWithTheRequestDecision) {
    // The case the whole shape exists for. Two authorization paths that CAN
    // disagree will, and the way this one would is silent: a connection that
    // keeps an authority a request would refuse is a revocation that did not
    // happen, and nothing anywhere logs it.
    //
    // Driven as a table through BOTH functions over the same context, policy and
    // epoch, so the assertion is the agreement rather than two separate lists of
    // expectations that somebody keeps in step by hand.
    struct Row final {
        accesscontrol::RouteAccess access;
        PermSet                    held;
        UserType                   type;
        accesscontrol::EpochVerdict epoch;
        std::string_view           why;
    };
    const PermSet required = perm_mask(Perm::ContentWrite);
    const std::array<Row, 8> kRows = {{
        {accesscontrol::RouteAccess::Guarded, perm_mask(Perm::ContentWrite), UserType::Staff,
         accesscontrol::EpochVerdict::Match, "holds the bit, epoch agrees"},
        {accesscontrol::RouteAccess::Guarded, PermSet{}, UserType::Staff,
         accesscontrol::EpochVerdict::Match, "the bit was taken away"},
        {accesscontrol::RouteAccess::Guarded, perm_mask(Perm::ContentWrite), UserType::Staff,
         accesscontrol::EpochVerdict::Mismatch, "minted before a permission change"},
        {accesscontrol::RouteAccess::Guarded, perm_mask(Perm::ContentWrite), UserType::Staff,
         accesscontrol::EpochVerdict::Unknown, "the authority is not cached"},
        {accesscontrol::RouteAccess::Stealth, PermSet{}, UserType::Staff,
         accesscontrol::EpochVerdict::Unknown, "stealth, unsatisfied, uncached"},
        {accesscontrol::RouteAccess::Stealth, perm_mask(Perm::ContentWrite), UserType::Staff,
         accesscontrol::EpochVerdict::Unknown, "stealth, satisfied, uncached"},
        {accesscontrol::RouteAccess::Authenticated, PermSet{}, UserType::Staff,
         accesscontrol::EpochVerdict::Match, "authenticated needs no bit"},
        {accesscontrol::RouteAccess::Guarded, PermSet{}, UserType::SuperAdmin,
         accesscontrol::EpochVerdict::Match, "superadmin holds no bit and passes anyway"},
    }};

    for (const Row& row : kRows) {
        const accesscontrol::RoutePolicy policy{required, "/content/edit", row.access};
        epochs_.verdict = row.epoch;

        const std::string cookie = cookie_for(row.held, row.type, 5, now_ + 900);
        const accesscontrol::Evaluation decided =
            accesscontrol::evaluate(cookie, policy, keys_, epochs_, now_);

        const UserContext ctx{
            .user_id = user_,
            .session_id = session_,
            .permissions = row.held,
            .perm_epoch = 5,
            .user_type = row.type,
            .locale = *Locale::from_tag("en"),
            .reserved = {},
        };
        const accesscontrol::ConnectionVerdict kept = accesscontrol::still_authorized(
            ctx, static_cast<std::uint32_t>(now_ + 900), policy, epochs_, now_);

        const accesscontrol::ConnectionVerdict expected =
            decided.step == accesscontrol::Step::Allow    ? accesscontrol::ConnectionVerdict::Keep
            : decided.step == accesscontrol::Step::Deny   ? accesscontrol::ConnectionVerdict::Close
                                                          : accesscontrol::ConnectionVerdict::ResolveEpoch;
        EXPECT_EQ(kept, expected) << row.why;
    }
}

TEST_F(DecisionTest, AConnectionClosesWhenTheCredentialThatOpenedItExpires) {
    // A connection that outlives its token is a session with no end, and the
    // epoch cannot answer for it: the epoch says whether the AUTHORITY changed,
    // not whether the credential is still valid.
    const accesscontrol::RoutePolicy policy{PermSet{}, "/feed",
                                            accesscontrol::RouteAccess::Authenticated};
    const UserContext ctx{
        .user_id = user_,
        .session_id = session_,
        .permissions = PermSet{},
        .perm_epoch = 5,
        .user_type = UserType::Staff,
        .locale = *Locale::from_tag("en"),
        .reserved = {},
    };

    EXPECT_EQ(accesscontrol::still_authorized(ctx, static_cast<std::uint32_t>(now_ + 900),
                                              policy, epochs_, now_),
              accesscontrol::ConnectionVerdict::Keep);

    // Exactly the comparison `auth::decode` makes, tolerance included. A
    // re-check one minute stricter than the verification would close a
    // connection whose token a fresh request still accepts — and the client
    // would reconnect, hand over that same token, and be let straight back in.
    const std::uint32_t expires_at = static_cast<std::uint32_t>(now_);
    EXPECT_EQ(accesscontrol::still_authorized(
                  ctx, expires_at, policy, epochs_, now_ + auth::kClockSkewToleranceSeconds),
              accesscontrol::ConnectionVerdict::Keep)
        << "inside the tolerance, so a fresh request would still accept this token";
    EXPECT_EQ(accesscontrol::still_authorized(
                  ctx, expires_at, policy, epochs_,
                  now_ + auth::kClockSkewToleranceSeconds + 1),
              accesscontrol::ConnectionVerdict::Close);
}

TEST_F(DecisionTest, APublicConnectionIsStillBoundedByTheCredentialItCarries) {
    // Agreement is "both produce the same authority", not "both allow".
    // `evaluate_token` on a Public route with an expired token allows the
    // request and builds an EMPTY context, so a connection still holding a
    // populated one past that instant disagrees with what a fresh request would
    // make of the same credential — whatever either of them answers about
    // access.
    const accesscontrol::RoutePolicy policy{PermSet{}, "/feed",
                                            accesscontrol::RouteAccess::Public};
    const UserContext ctx{
        .user_id = user_,
        .session_id = session_,
        .permissions = perm_mask(Perm::ContentRead),
        .perm_epoch = 5,
        .user_type = UserType::Staff,
        .locale = *Locale::from_tag("en"),
        .reserved = {},
    };

    // The epoch is never consulted on a public route, here as in the filter: it
    // grants nothing a revoked permission could have unlocked, so a round trip
    // behind unauthenticated traffic would be a denial-of-service lever with no
    // security benefit.
    epochs_.verdict = accesscontrol::EpochVerdict::Mismatch;
    EXPECT_EQ(accesscontrol::still_authorized(ctx, static_cast<std::uint32_t>(now_ + 900),
                                              policy, epochs_, now_),
              accesscontrol::ConnectionVerdict::Keep);

    EXPECT_EQ(accesscontrol::still_authorized(ctx, static_cast<std::uint32_t>(now_ - 3600),
                                              policy, epochs_, now_),
              accesscontrol::ConnectionVerdict::Close);
}

TEST_F(DecisionTest, TheResolvedPathCanOnlyEverCloseAConnection) {
    // The mirror of `resume_after_epoch`, and the property that makes the early
    // answer on an uncached stealth connection safe: resolving re-checks THIS
    // context's own mask, so an authority can turn a keep into a close and never
    // a close into a keep.
    const accesscontrol::RoutePolicy policy{perm_mask(Perm::ContentWrite), "/content/edit",
                                            accesscontrol::RouteAccess::Guarded};
    const UserContext holder{
        .user_id = user_,
        .session_id = session_,
        .permissions = perm_mask(Perm::ContentWrite),
        .perm_epoch = 5,
        .user_type = UserType::Staff,
        .locale = *Locale::from_tag("en"),
        .reserved = {},
    };
    EXPECT_EQ(accesscontrol::resume_connection_after_epoch(holder, policy, 5),
              accesscontrol::ConnectionVerdict::Keep);
    EXPECT_EQ(accesscontrol::resume_connection_after_epoch(holder, policy, 6),
              accesscontrol::ConnectionVerdict::Close)
        << "the authority moved past the epoch this connection was opened under";

    UserContext stripped = holder;
    stripped.permissions = PermSet{};
    EXPECT_EQ(accesscontrol::resume_connection_after_epoch(stripped, policy, 5),
              accesscontrol::ConnectionVerdict::Close);
}

TEST(OriginCheck, AnUpgradeIsAGetAndTheMethodIsTheWrongQuestion) {
    // The case that would have caught it. A WebSocket handshake is an HTTP GET
    // with `Upgrade: websocket`, so by method it is safe — and it is not: same
    // origin policy does not constrain WebSockets, the browser attaches the
    // cookie anyway, and the origin check is therefore the only CSRF defence on
    // that path rather than defence in depth behind "GET does not mutate".
    //
    // The failure shape is why this needed a case and not a comment: the wrong
    // answer is a PASS. There is no log line, no metric and no error — the
    // control is present, called, and answering the wrong thing, which is
    // indistinguishable from working.
    constexpr std::array<std::string_view, 1> allowed{"https://example.test"};

    // What the handshake gets by default, and what it must not get.
    EXPECT_EQ(http::check_origin("GET", "https://evil.test", allowed),
              http::OriginVerdict::NotRequired);
    EXPECT_FALSE(http::is_rejection(http::check_origin("GET", "https://evil.test", allowed)));

    EXPECT_EQ(http::check_origin("GET", "https://evil.test", allowed,
                                 http::OriginRequirement::Always),
              http::OriginVerdict::Mismatched);
    EXPECT_TRUE(http::is_rejection(http::check_origin(
        "GET", "https://evil.test", allowed, http::OriginRequirement::Always)));

    // Absent is a rejection under Always, for the reason absence is a rejection
    // on a POST: "no header means not a browser" is an opt-out an attacker takes.
    EXPECT_EQ(http::check_origin("GET", "", allowed, http::OriginRequirement::Always),
              http::OriginVerdict::Missing);
    // And `null` — a sandboxed iframe or a redirected cross-origin request — is
    // a value that must never reach the compare loop.
    EXPECT_EQ(http::check_origin("GET", "null", allowed, http::OriginRequirement::Always),
              http::OriginVerdict::Missing);

    // A legitimate handshake from a configured origin still passes, or the
    // control is a denial of service against its own application.
    EXPECT_EQ(http::check_origin("GET", "https://example.test", allowed,
                                 http::OriginRequirement::Always),
              http::OriginVerdict::Allowed);

    // `Always` narrows the requirement and never widens it: a state-changing
    // method answers exactly what it always did.
    EXPECT_EQ(http::check_origin("POST", "https://evil.test", allowed,
                                 http::OriginRequirement::Always),
              http::OriginVerdict::Mismatched);
    EXPECT_EQ(http::check_origin("POST", "https://example.test", allowed,
                                 http::OriginRequirement::Always),
              http::OriginVerdict::Allowed);
}

// --- the allow-list a process holds -----------------------------------------

TEST(AllowedOriginList, ParsesTheFormABrowserActuallySends) {
    http::AllowedOrigins origins;
    ASSERT_TRUE(origins.parse("https://example.test, https://admin.example.test:8443"));
    EXPECT_EQ(origins.size(), 2U);
    EXPECT_TRUE(origins.contains("https://example.test"));
    EXPECT_TRUE(origins.contains("https://admin.example.test:8443"));

    // Exact, byte-for-byte, the same rule the span overload has always had.
    EXPECT_FALSE(origins.contains("https://evil-example.test"));
    EXPECT_FALSE(origins.contains("https://example.test.attacker.test"));
    EXPECT_FALSE(origins.contains("http://example.test"));
    EXPECT_FALSE(origins.contains("https://example.test/"));
}

TEST(AllowedOriginList, RefusesAnEntryThatCouldNeverMatchAHeader) {
    // Every row here would parse into a list that looks configured and rejects
    // every request, because a browser's `Origin` is scheme, host and port and
    // nothing else. Refusing at parse turns a silent, total outage of whatever
    // the list guards into a boot failure naming the variable.
    const std::array<std::string_view, 7> kMalformed = {{
        "https://example.test/",           // the trailing slash off an address bar
        "https://example.test/admin",      // a path
        "https://example.test?x=1",        // a query
        "example.test",                    // no scheme
        "://example.test",                 // no scheme, with the separator
        "https://",                        // no authority
        "https://example.test,,https://other.test",  // a stray comma
    }};
    for (const std::string_view row : kMalformed) {
        http::AllowedOrigins origins;
        EXPECT_FALSE(origins.parse(row)) << row;
        EXPECT_TRUE(origins.empty()) << row;
    }

    // An empty list is not a list. It parses to nothing and says so, so a
    // deployment that set the variable to "" fails at boot rather than refusing
    // every upgrade at three in the morning.
    http::AllowedOrigins none;
    EXPECT_FALSE(none.parse(""));
}

TEST(AllowedOriginList, RefusesNullAsAnEntry) {
    // `null` is what a browser sends from a sandboxed iframe, a data: document
    // or a redirected cross-origin request. `check_origin` already treats the
    // HEADER as absent before it reaches any compare, so an entry of "null"
    // could not match today — this refuses it on the other side as well, so the
    // property does not rest on one line staying where it is.
    http::AllowedOrigins origins;
    EXPECT_FALSE(origins.parse("null"));
    EXPECT_FALSE(origins.parse("https://example.test, null"));
}

TEST(AllowedOriginList, RefusesMoreEntriesThanItHolds) {
    std::string many;
    for (std::size_t i = 0; i <= http::AllowedOrigins::kMaxEntries; ++i) {
        if (i != 0) { many += ','; }
        many += "https://host" + std::to_string(i) + ".test";
    }
    http::AllowedOrigins origins;
    EXPECT_FALSE(origins.parse(many));
    EXPECT_TRUE(origins.empty()) << "a refused parse must not leave a partial list behind";
}

TEST(AllowedOriginList, TheTwoLookupsAreOneRule) {
    // Two ways to look an origin up and one decision, so the absent-is-a-
    // rejection rule cannot hold on one path and not the other.
    constexpr std::array<std::string_view, 1> span_form{"https://example.test"};
    http::AllowedOrigins list;
    ASSERT_TRUE(list.parse("https://example.test"));

    const std::array<std::string_view, 4> kOrigins = {
        {"https://example.test", "https://evil.test", "", "null"}};
    for (const std::string_view origin : kOrigins) {
        for (const http::OriginRequirement requirement :
             {http::OriginRequirement::ByMethod, http::OriginRequirement::Always}) {
            for (const std::string_view method : {"GET", "POST"}) {
                EXPECT_EQ(http::check_origin(method, origin, span_form, requirement),
                          http::check_origin(method, origin, list, requirement))
                    << method << ' ' << origin;
            }
        }
    }
}

// The installed list is process-wide, so it is put back whatever a case does.
class InstalledOrigins final {
public:
    explicit InstalledOrigins(std::string_view list) {
        auto origins = std::make_shared<http::AllowedOrigins>();
        EXPECT_TRUE(origins->parse(list));
        http::install_allowed_origins(std::move(origins));
    }
    ~InstalledOrigins() { http::install_allowed_origins(nullptr); }

    InstalledOrigins(const InstalledOrigins&) = delete;
    InstalledOrigins& operator=(const InstalledOrigins&) = delete;
};

[[nodiscard]] drogon::HttpRequestPtr request_from(std::string_view origin) {
    drogon::HttpRequestPtr req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/ws/feed");
    if (!origin.empty()) { req->addHeader("Origin", std::string{origin}); }
    return req;
}

TEST(AllowedOriginList, NothingInstalledAcceptsNoOrigin) {
    // Deliberately NOT the `install_trusted_proxies` answer. An absent proxy
    // list means "this process is the edge", which is a real deployment; an
    // absent origin list means nobody said which origins are ours, and there is
    // no safe way to guess.
    http::install_allowed_origins(nullptr);
    EXPECT_EQ(http::check_request_origin(request_from("https://example.test"),
                                         http::OriginRequirement::Always),
              http::OriginVerdict::Mismatched);

    // And the method rule still runs first, so a route that required no Origin
    // is not turned into a rejection by a list nobody installed.
    EXPECT_EQ(http::check_request_origin(request_from("https://example.test"),
                                         http::OriginRequirement::ByMethod),
              http::OriginVerdict::NotRequired);
}

TEST(AllowedOriginList, AnInstalledListDecidesALiveRequest) {
    const InstalledOrigins installed{"https://example.test"};

    EXPECT_EQ(http::check_request_origin(request_from("https://example.test"),
                                         http::OriginRequirement::Always),
              http::OriginVerdict::Allowed);
    EXPECT_EQ(http::check_request_origin(request_from("https://evil.test"),
                                         http::OriginRequirement::Always),
              http::OriginVerdict::Mismatched);
    EXPECT_EQ(http::check_request_origin(request_from(""), http::OriginRequirement::Always),
              http::OriginVerdict::Missing);
    EXPECT_EQ(http::check_request_origin(nullptr, http::OriginRequirement::Always),
              http::OriginVerdict::Mismatched);
}

TEST(OriginCheck, TheDefaultIsWhatEveryExistingCallSiteHad) {
    // The other half of "a defaulted parameter, so no call site changes". A
    // default that had silently become `Always` would turn every GET in every
    // consuming application into a rejection, which is the failure mode of
    // fixing this the other way round.
    constexpr std::array<std::string_view, 1> allowed{"https://example.test"};
    EXPECT_EQ(http::check_origin("GET", "", allowed),
              http::check_origin("GET", "", allowed, http::OriginRequirement::ByMethod));
    EXPECT_EQ(http::check_origin("GET", "", allowed), http::OriginVerdict::NotRequired);
}

// --- the boot guard keys on what the filter keys on -------------------------
//
// `declared()` checked `is_declared(testapp::kRoutes, pattern)` while the filter resolves
// `(pattern, method)`. A handler registered under a method the registry does not
// declare therefore booted CLEAN and then denied every request to itself,
// logging an error nobody watches for. It fails closed, so this is availability
// rather than security — and it is the shape every outage of this kind has: a
// total failure of one route, with the whole suite green.

TEST(RouteRegistry, TheDeclarationGuardAnswersPerMethodAndNotPerPattern) {
    using accesscontrol::RouteMethod;
    using accesscontrol::is_declared;

    // `/me` is declared for GET and for nothing else. The PATTERN exists, which
    // is all a guard keyed on the pattern alone would ever ask.
    EXPECT_TRUE(accesscontrol::is_pattern_declared(testapp::kRoutes, "/me"));
    EXPECT_TRUE(is_declared(testapp::kRoutes, "/me", RouteMethod::Get));

    // And a method-specific entry satisfies its own method and no other. This is
    // the registration that used to boot clean and then deny every request to
    // itself: a guard asking only whether the pattern appeared somewhere said
    // yes, while the filter — which resolves (pattern, method) — said no.
    EXPECT_FALSE(is_declared(testapp::kRoutes, "/me", RouteMethod::Post));
    EXPECT_FALSE(is_declared(testapp::kRoutes, "/me", RouteMethod::Delete));
    EXPECT_FALSE(is_declared(testapp::kRoutes, "/me", RouteMethod::Patch));
}

TEST(RouteRegistry, AnAnyEntryStillSatisfiesEveryMethod) {
    using accesscontrol::RouteMethod;
    using accesscontrol::is_declared;

    // Most patterns answer one method and declare `Any`; the guard must not
    // start rejecting them, or every such route fails to boot.
    for (const RouteMethod method : {RouteMethod::Get, RouteMethod::Post, RouteMethod::Put,
                                     RouteMethod::Patch, RouteMethod::Delete,
                                     RouteMethod::Any}) {
        EXPECT_TRUE(is_declared(testapp::kRoutes, "/session/logout", method))
            << "an Any entry must satisfy every method";
    }
}

TEST(RouteRegistry, AMediaPatternSplitAcrossTwoMethodsIsDeclaredForBothAndNoOthers) {
    using accesscontrol::RouteMethod;
    using accesscontrol::is_declared;

    // One pattern, a public GET and a stealthed DELETE. Both must pass the
    // guard, and nothing else may.
    EXPECT_TRUE(is_declared(testapp::kRoutes, "/media/{ns}/{id}", RouteMethod::Get));
    EXPECT_TRUE(is_declared(testapp::kRoutes, "/media/{ns}/{id}", RouteMethod::Delete));
    EXPECT_FALSE(is_declared(testapp::kRoutes, "/media/{ns}/{id}", RouteMethod::Post));
    EXPECT_FALSE(is_declared(testapp::kRoutes, "/media/{ns}/{id}", RouteMethod::Patch));
}

TEST(RouteRegistry, AnUndeclaredPatternIsRefusedUnderEveryMethod) {
    using accesscontrol::RouteMethod;
    using accesscontrol::is_declared;

    for (const RouteMethod method : {RouteMethod::Get, RouteMethod::Post, RouteMethod::Put,
                                     RouteMethod::Patch, RouteMethod::Delete,
                                     RouteMethod::Any}) {
        EXPECT_FALSE(is_declared(testapp::kRoutes, "/nope", method));
    }
}

// The guard and the filter must resolve identically for EVERY entry in the
// table, which is the invariant a per-pattern guard broke. Asserting it over the
// whole registry rather than over a sample means a future entry cannot
// reintroduce the gap.
TEST(RouteRegistry, TheGuardAgreesWithPolicyForOnEveryDeclaredRoute) {
    using accesscontrol::RouteMethod;
    for (const accesscontrol::RoutePolicy& route : testapp::kRoutes) {
        for (const RouteMethod method : {RouteMethod::Get, RouteMethod::Post, RouteMethod::Put,
                                         RouteMethod::Patch, RouteMethod::Delete}) {
            EXPECT_EQ(accesscontrol::is_declared(testapp::kRoutes, route.pattern, method),
                      accesscontrol::policy_for(testapp::kRoutes, route.pattern, method) != nullptr)
                << route.pattern;
        }
    }
}


// --- the one attribute ------------------------------------------------------
//
// The filter wrote the attribute under a constant declared in
// accesscontrol/access_filter.h and the public header declared a different one,
// so anything reading the key a consumer would naturally reach for got a null
// context at run time with no error. Every test drove evaluate() directly, which
// is below the attribute map, so nothing saw it.
//
// These are the assertions that would have caught it, now over the scope that
// carries the context: what the public header names is what the reader finds.
// Nothing here boots a listener — the advice is not what is under test, the map
// is — so each case fills the attribute the way the advice would.

namespace {

// The pre-routing advice's write, without the advice: one scope, id minted, no
// context yet. A request built this way is exactly the shape a handler sees on a
// public route nobody is signed in for.
[[nodiscard]] drogon::HttpRequestPtr request_with_scope() {
    const drogon::HttpRequestPtr request = drogon::HttpRequest::newHttpRequest();
    auto scope = std::make_shared<http::RequestScope>();
    scope->request_id = http::mint_request_id(1'700'000'000'000);
    scope->has_context = false;
    request->attributes()->insert(std::string{http::kRequestScopeKey}, std::move(scope));
    return request;
}

}  // namespace

TEST(RequestScope, ARequestWithNoScopeReadsAsNullAndAsANilId) {
    const drogon::HttpRequestPtr request = drogon::HttpRequest::newHttpRequest();
    EXPECT_EQ(accesscontrol::user_context(request), nullptr)
        << "a request nothing has attached a context to must read as null";
    EXPECT_EQ(http::request_scope(request), nullptr);
    // All-zero rather than a fresh mint, so "install_request_scope() was never
    // called" reads as one unmistakable value instead of as 26 plausible
    // characters that correlate to nothing.
    EXPECT_EQ(http::request_id_of(request).bytes, (std::array<std::uint8_t, 16>{}));
}

TEST(RequestScope, AScopeWithNoContextIsNotAContext) {
    const drogon::HttpRequestPtr request = request_with_scope();
    EXPECT_NE(http::request_scope(request), nullptr);
    // The id is there for a log line and a body; the context is not, because no
    // filter has run. A reader that confused the two would hand a handler a
    // zeroed UserContext and let it read user_id 0 as a user.
    EXPECT_EQ(accesscontrol::user_context(request), nullptr);
}

TEST(RequestScope, TheKeyThePublicHeaderDeclaresIsTheOneTheReaderUses) {
    const drogon::HttpRequestPtr request = request_with_scope();

    UserContext attached{};
    attached.user_id = uuid::generate_v4();
    ASSERT_TRUE(http::attach_user_context(request, attached));

    const std::shared_ptr<const UserContext> found = accesscontrol::user_context(request);
    ASSERT_NE(found, nullptr)
        << "attached under anvil::http::kRequestScopeKey and the reader did not find it";
    EXPECT_EQ(found->user_id, attached.user_id);
}

TEST(RequestScope, TheContextTheReaderGetsAliasesTheScopeRatherThanACopy) {
    // The aliasing constructor is what makes a context safe to carry onto a
    // thread pool without the copy ENGINEERING_RULES.md §2.2 would otherwise demand: the
    // pointer a handler holds keeps the whole scope alive, so the id is still
    // readable from the same request and nothing was allocated to achieve it.
    const drogon::HttpRequestPtr request = request_with_scope();
    UserContext attached{};
    attached.user_id = uuid::generate_v4();
    ASSERT_TRUE(http::attach_user_context(request, attached));

    const std::shared_ptr<const UserContext> found = accesscontrol::user_context(request);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found.get(), &http::request_scope(request)->ctx);
}

TEST(RequestScope, ASecondContextIsRefusedRatherThanSubstituted) {
    // A second fill would replace the authority the filter established with one
    // nothing checked. It is a registration bug rather than a reachable attack —
    // and a registration bug that silently succeeded is how it would become one.
    const drogon::HttpRequestPtr request = request_with_scope();

    UserContext first{};
    first.user_id = uuid::generate_v4();
    ASSERT_TRUE(http::attach_user_context(request, first));

    UserContext second{};
    second.user_id = uuid::generate_v4();
    EXPECT_FALSE(http::attach_user_context(request, second));
    EXPECT_EQ(accesscontrol::user_context(request)->user_id, first.user_id);
}

TEST(RequestScope, AContextCannotBeAttachedWithoutAScope) {
    // Which is the boot mistake the filter logs about: no advice, no scope, and
    // every handler on the deployment reading a null context.
    const drogon::HttpRequestPtr request = drogon::HttpRequest::newHttpRequest();
    EXPECT_FALSE(http::attach_user_context(request, UserContext{}));
}

TEST(RequestScope, TheContextStartsOnTheAllocationsFirstCacheLine) {
    // The layout claim, asserted over a real object rather than only in the
    // header: a scope whose context did not start at offset 0 would put the
    // filter's read across two lines on every protected request.
    const http::RequestScope scope{};
    EXPECT_EQ(static_cast<const void*>(&scope), static_cast<const void*>(&scope.ctx));
    EXPECT_EQ(sizeof(scope.ctx), 64U);
}

// --- an application's routes ------------------------------------------------
//
// Roughly thirty cases here asserted one application's ROUTE TABLE: that its
// preview listing was stealthed behind the create bit, that its member directory
// offered one read and one write and no other verb, that no route existed for
// creating a superadmin. Every one of them was worth having, and not one of them
// is anvil's to assert — they are statements about a product's API surface, in
// the same way permission bit indices are statements about its authorities.
//
// They belong in the application's own tests, beside the table they describe.
// What survives here is the structural half, which holds for ANY table: that
// every entry resolves by its own pattern and method, that pairs are unique, that
// a method-specific entry does not leak across methods, that an undeclared route
// is not silently permitted, that a stealth route requires a permission, and that
// the boot guard agrees with policy_for on every declared route.
//
// --- the projection: the routes a holder is told about --------------------

TEST(RouteProjection, OmitsWhatTheHolderCannotReach) {
    std::string out;
    accesscontrol::append_reachable_routes(out, testapp::kRoutes,
                                           testapp::kRouteDescriptions,
                                           perm_mask(testapp::Perm::ContentRead),
                                           UserType::Staff);

    // Held, so its path is sent.
    EXPECT_NE(out.find("\"content.get\":\"GET /content/{id}\""), std::string::npos);

    // Not held. The path of a route this holder cannot call never reaches them,
    // which is the whole purpose: a bundle is public and a chunk is a URL.
    EXPECT_EQ(out.find("content.delete"), std::string::npos);

    // Stealth and not held. Its existence is the thing being protected.
    EXPECT_EQ(out.find("audit.list"), std::string::npos);
    EXPECT_EQ(out.find("/audit"), std::string::npos);
}

TEST(RouteProjection, IncludesAuthenticatedRoutesWithNoBit) {
    std::string out;
    accesscontrol::append_reachable_routes(out, testapp::kRoutes,
                                           testapp::kRouteDescriptions, PermSet{},
                                           UserType::Client);

    EXPECT_NE(out.find("identity.me"), std::string::npos);
    EXPECT_NE(out.find("auth.logout"), std::string::npos);
}

TEST(RouteProjection, OmitsPathsTheBundleAlreadyHasUnlessAsked) {
    std::string held_only;
    accesscontrol::append_reachable_routes(held_only, testapp::kRoutes,
                                           testapp::kRouteDescriptions, PermSet{},
                                           UserType::Client);
    // A public path is already compiled into the bundle; sending it again is
    // bytes on every session response.
    EXPECT_EQ(held_only.find("auth.login"), std::string::npos);

    // And a BOOTSTRAP path is in the bundle for the other reason, so it drops
    // out here for the same one. This is the half of the split that is easy to
    // forget: the route is Authenticated, every holder reaches it, and it is
    // still not worth a byte on this response.
    EXPECT_EQ(held_only.find("session.current"), std::string::npos);

    std::string everything;
    accesscontrol::append_reachable_routes(everything, testapp::kRoutes,
                                           testapp::kRouteDescriptions, PermSet{},
                                           UserType::Client, true);
    EXPECT_NE(everything.find("auth.login"), std::string::npos);
    EXPECT_NE(everything.find("session.current"), std::string::npos);
}

TEST(RouteProjection, ABootstrapRouteIsStillReachableItIsJustNotWorthSending) {
    // The distinction the flag exists to draw. `reachable()` answers the
    // authority question and says yes; the projection omits it anyway. A reader
    // who conflates the two would "fix" this by listing it.
    const accesscontrol::RoutePolicy* policy =
        accesscontrol::policy_for(testapp::kRoutes, "/session",
                                  accesscontrol::RouteMethod::Get);
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->access, accesscontrol::RouteAccess::Authenticated);
    EXPECT_TRUE(accesscontrol::reachable(*policy, PermSet{}, UserType::Client));

    const descriptor::RouteDescription* description =
        descriptor::description_for(testapp::kRouteDescriptions, "session.current");
    ASSERT_NE(description, nullptr);
    EXPECT_TRUE(description->bootstrap);
    EXPECT_TRUE(descriptor::path_in_bundle(*description, policy->access));
}

TEST(RouteProjection, AgreesWithTheFilterOnEveryRoute) {
    // The property that matters, stated as a comparison rather than as a belief:
    // for every route and a range of holders, "is it in the table" must equal
    // "does satisfies() admit it". A second implementation of this question is a
    // second one to keep in agreement, and this is the test that would catch one
    // being introduced.
    const std::array<std::pair<PermSet, UserType>, 4> holders{{
        {PermSet{}, UserType::Client},
        {perm_mask(testapp::Perm::ContentRead), UserType::Staff},
        {perm_mask(testapp::Perm::ContentRead, testapp::Perm::ContentDelete,
                   testapp::Perm::AuditRead), UserType::Staff},
        {PermSet{}, UserType::SuperAdmin},
    }};

    for (const auto& [held, type] : holders) {
        std::string out;
        accesscontrol::append_reachable_routes(out, testapp::kRoutes,
                                               testapp::kRouteDescriptions, held, type);

        for (const auto& d : testapp::kRouteDescriptions) {
            const accesscontrol::RoutePolicy* policy =
                accesscontrol::policy_for(testapp::kRoutes, d.pattern, d.method);
            ASSERT_NE(policy, nullptr);
            // Excluded for a DISCLOSURE reason rather than an authority one, so
            // it has to be excluded from an authority comparison too. This line
            // used to test `access == Public` and caught the bootstrap flag the
            // moment it landed: `session.current` is reachable by every holder
            // and listed to none of them, which is the split being real rather
            // than decorative.
            if (descriptor::path_in_bundle(d, policy->access)) { continue; }

            const bool listed = out.find(std::string{"\""} + std::string{d.id} + "\":") !=
                                std::string::npos;
            EXPECT_EQ(listed, accesscontrol::satisfies(held, type, policy->required))
                << "route " << d.id << " is listed to a holder the filter would deny, "
                << "or withheld from one it would admit";
        }
    }
}

TEST(RouteProjection, SuperAdminReachesEverything) {
    std::string out;
    accesscontrol::append_reachable_routes(out, testapp::kRoutes,
                                           testapp::kRouteDescriptions, PermSet{},
                                           UserType::SuperAdmin);
    // Superadmin is an explicit type check rather than an all-ones mask, and the
    // projection inherits that by calling satisfies() rather than re-deciding.
    EXPECT_NE(out.find("audit.list"), std::string::npos);
}

// --- what a holder is told they ARE ----------------------------------------

TEST(HolderAuthority, ASuperAdminIsToldSoRatherThanBeingHandedNoBits) {
    // The defect, reproduced. A superadmin's permission set is deliberately NOT
    // all-ones, so counting bits answers zero for the one account that reaches
    // everything — and a client rendering a non-route affordance from bits hid
    // every one of them from exactly that account. It looks like a missing
    // feature rather than like a bug, which is why it survived.
    std::string out;
    accesscontrol::append_holder_authority(out, PermSet{}, UserType::SuperAdmin,
                                           testapp::kPerms);

    EXPECT_EQ(out, R"({"superadmin":true,"perms":[]})");

    // And the bits really are empty, which is what makes the boolean the only
    // answer a client could have had.
    EXPECT_TRUE(PermSet{}.none());
}

TEST(HolderAuthority, TheFlagComesFromTheSamePredicateTheFilterShortCircuitsOn) {
    // Two tests of "is this a superadmin" is two things to keep in agreement,
    // and the one that drifts is the one no attacker is reading. Asserted as an
    // equality over every UserType rather than as a belief.
    for (const UserType type : {UserType::Client, UserType::Staff, UserType::SuperAdmin}) {
        std::string out;
        accesscontrol::append_holder_authority(out, PermSet{}, type, testapp::kPerms);
        const bool said = out.find("\"superadmin\":true") != std::string::npos;
        EXPECT_EQ(said, accesscontrol::is_superadmin(type));
        // satisfies() is the filter's own reading of the same fact.
        EXPECT_EQ(said, accesscontrol::satisfies(PermSet{}, type,
                                                 perm_mask(testapp::Perm::AuditRead)));
    }
}

TEST(HolderAuthority, NamesAreTheHeldBitsInBitOrderAndNeverAnAllOnesMask) {
    // Declared out of bit order on purpose: the response must not depend on how
    // a caller happened to write the arguments, because the session response
    // carries an ETag and two spellings of one holder would make every
    // revalidation a miss.
    const PermSet held = perm_mask(testapp::Perm::AuditRead, testapp::Perm::ContentRead,
                                   testapp::Perm::MediaUpload);
    std::string out;
    accesscontrol::append_holder_authority(out, held, UserType::Staff, testapp::kPerms);

    EXPECT_EQ(out,
              R"({"superadmin":false,"perms":["ContentRead","MediaUpload","AuditRead"]})");
}

TEST(HolderAuthority, ASuperAdminWhoAlsoHoldsBitsReportsBoth) {
    // The flag is not a substitute for the list. A superadmin who was also
    // granted bits has both facts, and folding either into the other is the
    // conflation core/types.h keeps apart.
    std::string out;
    accesscontrol::append_holder_authority(out, perm_mask(testapp::Perm::ContentRead),
                                           UserType::SuperAdmin, testapp::kPerms);

    EXPECT_EQ(out, R"({"superadmin":true,"perms":["ContentRead"]})");
}

TEST(HolderAuthority, ABitNoPermissionDeclaresNeverReachesAClient) {
    // The reserved gaps between an application's blocks are not permissions the
    // server understands, and a control rendered from one authorises nothing. A
    // mask built with ~PermSet{} would hand them all over, which is the other
    // reason this is not an all-ones mask.
    PermSet held{};
    held.set(7);    // between ContentDelete (2) and MediaUpload (8)
    held.set(100);  // between AuditRead (25) and SystemAnnounce (120)
    ASSERT_TRUE(testapp::kPerms.name_for_bit(7).empty());
    ASSERT_TRUE(testapp::kPerms.name_for_bit(100).empty());

    std::string out;
    accesscontrol::append_holder_authority(out, held, UserType::Staff, testapp::kPerms);
    EXPECT_EQ(out, R"({"superadmin":false,"perms":[]})");
}

TEST(HolderAuthority, IsDeterministic) {
    const PermSet held = perm_mask(testapp::Perm::ContentRead, testapp::Perm::AuditRead);
    std::string a;
    std::string b;
    accesscontrol::append_holder_authority(a, held, UserType::Staff, testapp::kPerms);
    accesscontrol::append_holder_authority(b, held, UserType::Staff, testapp::kPerms);
    EXPECT_EQ(a, b);
}

TEST(RouteProjection, IsDeterministic) {
    // The response carries an ETag keyed to perm_epoch. Two emissions that
    // differ by key order would make every revalidation a miss.
    std::string a;
    std::string b;
    accesscontrol::append_reachable_routes(a, testapp::kRoutes, testapp::kRouteDescriptions,
                                           perm_mask(testapp::Perm::ContentRead),
                                           UserType::Staff);
    accesscontrol::append_reachable_routes(b, testapp::kRoutes, testapp::kRouteDescriptions,
                                           perm_mask(testapp::Perm::ContentRead),
                                           UserType::Staff);
    EXPECT_EQ(a, b);
}

// tests/testapp/routes.h carries the same product-shaped assertions as
// static_asserts over its own table, which is where an application should put
// them: a route that violates one then fails the build rather than a test.

}  // namespace
}  // namespace anvil
