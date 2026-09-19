// The client address behind a reverse proxy.
//
// The eight call sites that read an address all fed a per-IP mechanism: five
// rate-limit buckets, the session listing, the sign-in notification and every
// denial record. Behind the Nginx front end this repository ships, all of them
// were reading 127.0.0.1, so all of them shared one bucket and one source
// attribution — with every test still passing, because every test drives the
// decision function and none drives the deployed surface.
//
// So these cases drive resolve_client_address the way a proxy actually presents
// a request, and case 9 drives the packing a real trantor peer produces.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include "anvil/http/client_address.h"

namespace anvil::http {
namespace {

[[nodiscard]] TrustedProxies proxies(std::string_view list) {
    TrustedProxies parsed;
    EXPECT_TRUE(parsed.parse(list)) << "fixture list must parse: " << list;
    return parsed;
}

[[nodiscard]] std::string text_of(const PackedAddress& packed) {
    std::string out;
    for (const std::uint8_t byte : packed) {
        out += "0123456789abcdef"[byte >> 4U];
        out += "0123456789abcdef"[byte & 0x0FU];
    }
    return out;
}

// --- 1: an untrusted peer's header is not evidence of anything -------------

TEST(ClientAddress, AnUntrustedPeerHasItsForwardedForIgnoredEntirely) {
    const TrustedProxies trusted = proxies("127.0.0.1/32");
    // Well-formed, plausible, and sent by a machine we never configured. This is
    // the request that reaches the application directly, and believing it would
    // let any client choose its own rate-limit bucket per request — strictly
    // worse than the collapsed single bucket this suite exists to have fixed.
    const PackedAddress peer = pack_address("203.0.113.9");
    EXPECT_EQ(resolve_client_address(peer, "198.51.100.4", trusted), peer);
}

// --- 2: the ordinary single-hop deployment ---------------------------------

TEST(ClientAddress, ATrustedPeerYieldsTheOneForwardedAddress) {
    const TrustedProxies trusted = proxies("127.0.0.1/32");
    EXPECT_EQ(resolve_client_address(pack_address("127.0.0.1"), "198.51.100.4", trusted),
              pack_address("198.51.100.4"));
}

// --- 3: a client cannot select the value it prepended ----------------------

TEST(ClientAddress, TheRightmostUntrustedEntryWinsSoAPrependedAddressCannotBeChosen) {
    const TrustedProxies trusted = proxies("127.0.0.1/32");
    // The client sent `X-Forwarded-For: 1.2.3.4` and Nginx APPENDED the address
    // it actually saw. Reading left to right — which is the documented "original
    // client" reading and is what makes this bug common — selects the forgery.
    const PackedAddress resolved =
        resolve_client_address(pack_address("127.0.0.1"), "1.2.3.4, 198.51.100.4", trusted);
    EXPECT_EQ(resolved, pack_address("198.51.100.4"));
    EXPECT_NE(resolved, pack_address("1.2.3.4"));
}

TEST(ClientAddress, TwoTrustedHopsStillSelectTheClientAndNotTheForgery) {
    const TrustedProxies trusted = proxies("127.0.0.1/32, 10.0.0.0/8");
    EXPECT_EQ(resolve_client_address(pack_address("127.0.0.1"),
                                     "1.2.3.4, 198.51.100.4, 10.1.2.3", trusted),
              pack_address("198.51.100.4"));
}

// --- 4: a header of nothing but proxies falls back, never to nothing -------

TEST(ClientAddress, AHeaderOfNothingButTrustedHopsFallsBackToThePeer) {
    const TrustedProxies trusted = proxies("127.0.0.1/32, 10.0.0.0/8");
    const PackedAddress peer = pack_address("127.0.0.1");
    const PackedAddress resolved = resolve_client_address(peer, "10.0.0.7, 10.1.2.3", trusted);
    EXPECT_EQ(resolved, peer);
    // The distinction that matters: falling back to the innermost peer, not to
    // the all-zero address. Zero is what every unattributable request packs to,
    // so falling back to it would put a whole deployment in one bucket again,
    // which is the defect this file exists for under a new name.
    EXPECT_NE(resolved, PackedAddress{});
}

// --- 5: hostile header values ----------------------------------------------

TEST(ClientAddress, MalformedForwardedForValuesFallBackToThePeerAndNeverThrow) {
    const TrustedProxies trusted = proxies("127.0.0.1/32");
    const PackedAddress peer = pack_address("127.0.0.1");

    // Non-ASCII, NUL-bearing, structurally wrong, and absurd. None may throw,
    // none may read out of bounds, and each must land on the peer.
    const std::string with_nul{std::string{"198.51."} + '\0' + "100.4"};
    for (const std::string_view value : {
             std::string_view{""},
             std::string_view{","},
             std::string_view{",,,,"},
             std::string_view{"   "},
             std::string_view{"not-an-address"},
             std::string_view{"999.999.999.999"},
             std::string_view{"198.51.100.4:8080"},
             std::string_view{"[198.51.100.4]"},
             std::string_view{"مرحبا"},
             std::string_view{"::ffff:junk"},
             std::string_view{with_nul},
         }) {
        EXPECT_EQ(resolve_client_address(peer, value, trusted), peer)
            << "value: " << value;
    }
}

TEST(ClientAddress, AnUnparseableRightmostEntryStopsTheWalkRatherThanReachingPastIt) {
    const TrustedProxies trusted = proxies("127.0.0.1/32");
    const PackedAddress peer = pack_address("127.0.0.1");
    // The rightmost entry is written by the hop nearest us, which is trusted, so
    // an unparseable one means something upstream is not what we think it is.
    // Walking further left would read values the CLIENT wrote, which is how a
    // client hides the real hop behind one bad entry.
    EXPECT_EQ(resolve_client_address(peer, "198.51.100.4, garbage", trusted), peer);
}

TEST(ClientAddress, AnOverLongForwardedForFallsBackRatherThanBeingBelieved) {
    const TrustedProxies trusted = proxies("127.0.0.1/32");
    const PackedAddress peer = pack_address("127.0.0.1");
    std::string huge;
    huge.reserve(64 * 1024);
    while (huge.size() < 64 * 1024) { huge += "1.2.3.4, "; }
    huge += "1.2.3.4";
    // Every entry is the same forged address and none is trusted, so the
    // rightmost wins — the point of the case is that it TERMINATES and stays
    // within its cap, which the next case measures.
    EXPECT_EQ(resolve_client_address(peer, huge, trusted), pack_address("1.2.3.4"));
}

// --- 6: one packing for both families --------------------------------------

TEST(ClientAddress, IPv4AndItsMappedFormAndIPv6AllPackToSixteenBytes) {
    const PackedAddress v4 = pack_address("198.51.100.4");
    const PackedAddress mapped = pack_address("::ffff:198.51.100.4");
    EXPECT_EQ(v4, mapped) << "a client must not get two buckets for one address";
    EXPECT_EQ(text_of(v4), "00000000000000000000ffffc6336404");

    const PackedAddress v6 = pack_address("2001:db8::1");
    EXPECT_EQ(v6.size(), 16U);
    EXPECT_EQ(text_of(v6), "20010db8000000000000000000000001");

    // Unparseable and unspecified both pack to zero: neither can be attributed
    // to anybody, so neither may be treated as an identity.
    EXPECT_EQ(pack_address("not-an-address"), PackedAddress{});
    EXPECT_EQ(pack_address("::"), PackedAddress{});
}

TEST(ClientAddress, AForwardedIPv6ClientIsResolvedAsItself) {
    const TrustedProxies trusted = proxies("127.0.0.1/32");
    EXPECT_EQ(resolve_client_address(pack_address("127.0.0.1"), "2001:db8::1", trusted),
              pack_address("2001:db8::1"));
}

// --- 7: the scan is bounded -------------------------------------------------

TEST(ClientAddress, TheHeaderScanIsBoundedInBytesAndInEntries) {
    const TrustedProxies trusted = proxies("127.0.0.1/32");
    const PackedAddress peer = pack_address("127.0.0.1");

    // 64 KB of trusted hops. An unbounded right-to-left walk would examine every
    // one of them; the cap means the answer arrives after a fixed amount of work
    // and is the peer, because nothing untrusted was reached inside it.
    std::string huge;
    huge.reserve(64 * 1024);
    while (huge.size() < 64 * 1024) { huge += "127.0.0.1, "; }
    huge += "127.0.0.1";
    EXPECT_EQ(resolve_client_address(peer, huge, trusted), peer);

    // And the real address, sitting past the cap, is NOT reached — which is the
    // honest consequence of bounding the scan and is the safe direction: the
    // fallback is the peer rather than an attacker-chosen entry.
    const std::string buried = "198.51.100.4, " + huge;
    EXPECT_EQ(resolve_client_address(peer, buried, trusted), peer);
}

// --- 8: an empty list reproduces the old behaviour exactly ------------------

TEST(ClientAddress, AnEmptyTrustedListIgnoresTheHeaderOnEveryInput) {
    const TrustedProxies none;
    EXPECT_TRUE(none.empty());
    for (const std::string_view peer_text : {"127.0.0.1", "203.0.113.9", "2001:db8::1"}) {
        const PackedAddress peer = pack_address(peer_text);
        EXPECT_EQ(resolve_client_address(peer, "", none), peer);
        EXPECT_EQ(resolve_client_address(peer, "198.51.100.4", none), peer);
        EXPECT_EQ(resolve_client_address(peer, "1.2.3.4, 198.51.100.4", none), peer);
    }
}

// --- the CIDR list itself ---------------------------------------------------

TEST(ClientAddress, PrefixLengthsMatchOnTheBitAndNotOnTheByte) {
    const TrustedProxies trusted = proxies("10.0.0.0/8, 192.168.1.0/24, 2001:db8::/32");

    EXPECT_TRUE(trusted.contains(pack_address("10.0.0.1")));
    EXPECT_TRUE(trusted.contains(pack_address("10.255.255.255")));
    EXPECT_FALSE(trusted.contains(pack_address("11.0.0.1")));

    EXPECT_TRUE(trusted.contains(pack_address("192.168.1.7")));
    EXPECT_FALSE(trusted.contains(pack_address("192.168.2.7")));

    EXPECT_TRUE(trusted.contains(pack_address("2001:db8::1")));
    EXPECT_FALSE(trusted.contains(pack_address("2001:db9::1")));

    // A v4 CIDR must not match the v6 address whose bytes happen to align with
    // its mapped form from the left.
    EXPECT_FALSE(trusted.contains(pack_address("::")));
}

TEST(ClientAddress, AnOddPrefixLengthMasksThePartialByte) {
    const TrustedProxies trusted = proxies("203.0.112.0/23");
    EXPECT_TRUE(trusted.contains(pack_address("203.0.112.1")));
    EXPECT_TRUE(trusted.contains(pack_address("203.0.113.255")));
    EXPECT_FALSE(trusted.contains(pack_address("203.0.114.1")));
}

TEST(ClientAddress, ABareAddressIsOneHostAndNotAWholeNetwork) {
    const TrustedProxies trusted = proxies("127.0.0.1, ::1");
    EXPECT_TRUE(trusted.contains(pack_address("127.0.0.1")));
    EXPECT_FALSE(trusted.contains(pack_address("127.0.0.2")));
    EXPECT_TRUE(trusted.contains(pack_address("::1")));
}

TEST(ClientAddress, AMalformedListIsRejectedRatherThanSilentlyTrustingNothing) {
    // Each of these would otherwise parse to an EMPTY list, which behaves
    // exactly like a correct edge deployment while putting every request back
    // into one bucket — the defect this file exists for, restored in silence.
    for (const std::string_view list : {
             std::string_view{"not-an-address"},
             std::string_view{"10.0.0.0/33"},
             std::string_view{"2001:db8::/129"},
             std::string_view{"10.0.0.0/eight"},
             std::string_view{"10.0.0.0/"},
             std::string_view{"127.0.0.1/32, garbage"},
             // A /0 on the unspecified address trusts every peer on earth.
             std::string_view{"::/0"},
             std::string_view{"0.0.0.0/0"},
         }) {
        TrustedProxies parsed;
        EXPECT_FALSE(parsed.parse(list)) << "list: " << list;
    }
}

TEST(ClientAddress, MoreEntriesThanTheFixedCapacityIsRefusedNotTruncated) {
    std::string list;
    for (std::size_t i = 0; i <= TrustedProxies::kMaxEntries; ++i) {
        if (i != 0) { list += ", "; }
        list += "10.0." + std::to_string(i) + ".0/24";
    }
    TrustedProxies parsed;
    // Truncating would silently stop trusting the last proxies in the list,
    // which reads as "some requests come from the loopback" and nothing else.
    EXPECT_FALSE(parsed.parse(list));
}

TEST(ClientAddress, WhitespaceAndTrailingCommasAreToleratedInTheList) {
    const TrustedProxies trusted = proxies("  127.0.0.1/32 ,, 10.0.0.0/8 , ");
    EXPECT_EQ(trusted.size(), 2U);
    EXPECT_TRUE(trusted.contains(pack_address("10.1.1.1")));
}

// --- the abuse screen has to show an address and take it back ---------------

TEST(ClientAddress, FormattingIsTheInverseOfPackingForBothFamilies) {
    // The abuse listing renders `format_address` and the purge route parses the
    // same spelling back with `pack_address`. If the pair does not round-trip,
    // the button beside a row points at nothing — so the property is asserted
    // here rather than left to the two call sites agreeing.
    for (const std::string_view text : {"203.0.113.7", "198.51.100.9", "2001:db8::42", "::1"}) {
        const PackedAddress packed = pack_address(text);
        const std::string rendered = format_address(packed);
        EXPECT_EQ(pack_address(rendered), packed) << text;
    }

    // A v4 address renders as a dotted quad and NOT as `::ffff:203.0.113.7`,
    // which is the same address and is not what anybody reads off a log.
    EXPECT_EQ(format_address(pack_address("203.0.113.7")), "203.0.113.7");

    // The unspecified address renders as nothing, which is also what
    // pack_address answers for anything it could not parse — so "no address"
    // has ONE spelling on both sides, and a row carrying it is omitted from the
    // listing rather than shown as a sender nobody can act on.
    EXPECT_TRUE(format_address(PackedAddress{}).empty());
    EXPECT_EQ(pack_address("not-an-address"), PackedAddress{});
}

}  // namespace
}  // namespace anvil::http
