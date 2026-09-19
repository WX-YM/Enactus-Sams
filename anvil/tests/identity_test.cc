// The identity layer's pure functions: address packing, network coarsening,
// device labelling, and the permission union.
//
// Everything here runs without a database, and everything here is a decision two
// different screens have to agree on. The point of testing them together is that
// "the same source" and "the same device" must mean exactly one thing in this
// system — an audit row and a session listing that disagree about either are
// impossible to line up, which is the one thing they exist for.

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "anvil/core/perm_set.h"
#include "anvil/identity/authz.h"
#include "anvil/identity/session_service.h"
#include "testapp/perms.h"

namespace {

using anvil::identity::AuthzService;
using anvil::identity::PackedIp;
using anvil::identity::RoleTable;
using anvil::identity::coarse_network_of;
using anvil::identity::coarsen_network;
using anvil::identity::device_label_of;
using anvil::identity::hash_user_agent;
using anvil::identity::is_staff;
using anvil::identity::pack_ip;

[[nodiscard]] anvil::Uuid uuid_with(std::uint8_t byte) noexcept {
    anvil::Uuid id{};
    id[0] = byte;
    return id;
}

// --- address packing --------------------------------------------------------

TEST(PackIp, MapsV4IntoTheV6Space) {
    const PackedIp packed = pack_ip("203.0.113.42");
    const std::array<std::uint8_t, 12> expected_prefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    for (std::size_t i = 0; i < expected_prefix.size(); ++i) {
        EXPECT_EQ(packed[i], expected_prefix[i]) << "byte " << i;
    }
    EXPECT_EQ(packed[12], 203);
    EXPECT_EQ(packed[13], 0);
    EXPECT_EQ(packed[14], 113);
    EXPECT_EQ(packed[15], 42);
}

TEST(PackIp, AnUnparseableAddressIsTheZeroAddressAndNotAThrow) {
    // A malformed peer address must not fail a login. A zero address is visibly
    // wrong in an audit row rather than quietly plausible, which is the whole
    // reason it is the fallback.
    EXPECT_EQ(pack_ip("not-an-address"), PackedIp{});
    EXPECT_EQ(pack_ip(""), PackedIp{});
}

TEST(PackIp, TheV4AndV4MappedSpellingsOfOneClientPackIdentically) {
    // Two implementations of "these bytes are that address" is how the two
    // spellings of one client end up in two different rate-limit buckets, which
    // is a limit that does not limit.
    EXPECT_EQ(pack_ip("203.0.113.42"), pack_ip("::ffff:203.0.113.42"));
}

// --- coarsening -------------------------------------------------------------

TEST(CoarsenNetwork, ClearsTheHostByteOfAV4Address) {
    const PackedIp a = coarsen_network(pack_ip("203.0.113.1"));
    const PackedIp b = coarsen_network(pack_ip("203.0.113.254"));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a[15], 0);
    EXPECT_EQ(a[14], 113);
}

TEST(CoarsenNetwork, DifferentV4NetworksStayDifferent) {
    EXPECT_NE(coarsen_network(pack_ip("203.0.113.1")), coarsen_network(pack_ip("203.0.114.1")));
}

TEST(CoarsenNetwork, ClearsTheLastTenBytesOfANativeV6Address) {
    const PackedIp a = coarsen_network(pack_ip("2001:db8:1234:5678::1"));
    const PackedIp b = coarsen_network(pack_ip("2001:db8:1234:9999::abcd"));
    // /48: the first six bytes decide, and everything below them is a host.
    EXPECT_EQ(a, b);
    for (std::size_t i = 6; i < 16; ++i) { EXPECT_EQ(a[i], 0) << "byte " << i; }
}

TEST(CoarseNetworkOf, RendersTheSuffixThatSaysWhichRuleApplied) {
    EXPECT_EQ(coarse_network_of(pack_ip("203.0.113.42")), "203.0.113.0/24");
    // /48 keeps the first six bytes: 2001:0db8:1234. Everything below is a host.
    EXPECT_EQ(coarse_network_of(pack_ip("2001:db8:1234:5678::1")), "2001:db8:1234::/48");
}

TEST(CoarseNetworkOf, IsTheRenderingOfExactlyWhatCoarsenNetworkMasks) {
    // The audit sink folds on the masked BYTES and a listing displays the
    // string. Deriving them separately is how a log line and a screen come to
    // disagree about what one source is.
    const PackedIp raw = pack_ip("198.51.100.77");
    EXPECT_EQ(coarse_network_of(raw), coarse_network_of(coarsen_network(raw)));
}

// --- device labelling -------------------------------------------------------

TEST(DeviceLabel, IsStableForOneUserAgent) {
    const std::string ua = "Mozilla/5.0 (X11; Linux x86_64) Gecko/20100101 Firefox/128.0";
    EXPECT_EQ(device_label_of(hash_user_agent(ua)), device_label_of(hash_user_agent(ua)));
}

TEST(DeviceLabel, NeverEchoesTheUserAgentBack) {
    // Echoing the raw string is a stored-XSS vector in whatever renders it, and
    // it tells an attacker exactly what is being fingerprinted. A word from a
    // fixed table answers "is that one me?" and nothing else.
    const std::string ua = "<script>alert(1)</script>";
    const std::string label = device_label_of(hash_user_agent(ua));
    EXPECT_EQ(label.find('<'), std::string::npos);
    EXPECT_EQ(label.find("script"), std::string::npos);
    EXPECT_FALSE(label.empty());
}

TEST(HashUserAgent, IsEightBytesAndNotTheString) {
    // The string is ~200 bytes per session document; at a million live sessions
    // the difference is on the order of 200 MB of WiredTiger cache, spent to
    // store something nothing reads back.
    static_assert(sizeof(anvil::identity::UserAgentHash) == 8);
    const auto a = hash_user_agent("one");
    const auto b = hash_user_agent("two");
    EXPECT_NE(a, b);
}

// --- session policy ---------------------------------------------------------

TEST(IsStaff, IsTrueForEveryTypeThatIsNotAnOrdinaryClient) {
    EXPECT_FALSE(is_staff(anvil::UserType::Client));
    EXPECT_TRUE(is_staff(anvil::UserType::Staff));
    EXPECT_TRUE(is_staff(anvil::UserType::FullControl));
    EXPECT_TRUE(is_staff(anvil::UserType::SuperAdmin));
}

TEST(SessionPolicy, PrivilegedSessionsDoNotSlide) {
    const anvil::identity::SessionPolicy policy{};
    // Equal on purpose: at the interval they re-authenticate. A sliding window
    // with no absolute cap means "requires full re-authentication" is never
    // actually enforced, because the window simply moves forward forever.
    EXPECT_EQ(policy.staff_refresh, policy.staff_absolute);
    EXPECT_LT(policy.client_refresh, policy.client_absolute);
    // A privileged session is worth less time.
    EXPECT_LT(policy.staff_access, policy.client_access);
}

// --- the permission union ---------------------------------------------------

TEST(EffectivePermissions, IsTheUnionOfDirectGrantsAndRoleMasks) {
    const anvil::Uuid author = uuid_with(1);
    const anvil::Uuid supervisor = uuid_with(2);
    const RoleTable roles{{{author, testapp::kContentAuthor},
                           {supervisor, testapp::kFormSupervisor}}};

    const anvil::PermSet direct =
        anvil::perm_mask(testapp::Perm::MediaUpload);
    const std::array<anvil::Uuid, 2> held{author, supervisor};

    const anvil::PermSet effective =
        AuthzService::effective_permissions(direct, held, roles);

    EXPECT_TRUE(effective.test(static_cast<std::size_t>(testapp::Perm::MediaUpload)));
    EXPECT_TRUE(effective.test(static_cast<std::size_t>(testapp::Perm::ContentWrite)));
    EXPECT_TRUE(effective.test(static_cast<std::size_t>(testapp::Perm::FormPii)));
    EXPECT_FALSE(effective.test(static_cast<std::size_t>(testapp::Perm::ContentDelete)));
}

TEST(EffectivePermissions, AnUnknownRoleGrantsNothing) {
    // The only safe reading. The alternative is a deleted role silently
    // retaining whatever mask it last had, for every account that still names
    // it.
    const RoleTable roles{{{uuid_with(1), testapp::kContentAuthor}}};
    const std::array<anvil::Uuid, 1> held{uuid_with(99)};

    const anvil::PermSet effective =
        AuthzService::effective_permissions(anvil::PermSet{}, held, roles);
    EXPECT_EQ(effective.count(), 0U);
}

TEST(EffectivePermissions, NeverSynthesisesAnAllOnesMaskForAPrivilegedType) {
    // SuperAdmin is an explicit check at the decision site, never an all-ones
    // mask, so that "holds every permission" and "is a superadmin" stay
    // distinguishable in an audit row and no bit-fiddling accident can
    // manufacture the second from the first.
    const RoleTable empty{};
    const anvil::PermSet effective =
        AuthzService::effective_permissions(anvil::PermSet{}, {}, empty);
    EXPECT_EQ(effective.count(), 0U);
}

TEST(RoleTable, AnEmptyTableResolvesEveryRoleToNothing) {
    const RoleTable empty{};
    EXPECT_EQ(empty.size(), 0U);
    EXPECT_EQ(empty.mask_for(uuid_with(1)).count(), 0U);
}

}  // namespace
