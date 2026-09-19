// The serving path: what goes into the redirect, what goes into the headers, and
// which variant a role resolves to.
//
// None of this touches a database or a disk. It is all the decisions that stand
// between a request and Nginx being told which file to send, and every one of
// them is a decision somebody could get wrong in a way nothing else would catch.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "anvil/fs/namespace.h"
#include "anvil/fs/paths.h"
#include "anvil/images/variants.h"
#include "anvil/media/serving.h"

namespace {

using anvil::fs::Format;
using anvil::fs::Ns;
using anvil::fs::VariantKey;
using anvil::images::VariantRecord;
using anvil::media::accel_redirect_response;
using anvil::media::encode_accel_path;
using anvil::media::negotiate_format;
using anvil::media::resolve_role;

[[nodiscard]] VariantRecord variant(std::uint16_t width, Format format) noexcept {
    return VariantRecord{.bytes = 1024,
                         .width = width,
                         .height = static_cast<std::uint16_t>(width / 2),
                         .format = format};
}

// --- the redirect value -----------------------------------------------------

TEST(AccelPath, LeavesUnreservedCharactersAndSeparatorsAlone) {
    EXPECT_EQ(encode_accel_path("/protected_storage/ns/media/ab/cd/0123456789abcdef.w640.avif"),
              "/protected_storage/ns/media/ab/cd/0123456789abcdef.w640.avif");
    EXPECT_EQ(encode_accel_path("aA0-_.~/"), "aA0-_.~/");
}

TEST(AccelPath, PercentEncodesCarriageReturnAndLineFeed) {
    // A CR or LF reaching a response header is response splitting. Every
    // component of this path is server-generated hex today, and "this value can
    // never contain a newline" is exactly the assumption a refactor breaks —
    // which is why the encoding is unconditional rather than conditional on the
    // value looking suspicious.
    EXPECT_EQ(encode_accel_path("a\rb"), "a%0Db");
    EXPECT_EQ(encode_accel_path("a\nb"), "a%0Ab");
    EXPECT_EQ(encode_accel_path("a\r\nX-Evil: 1"), "a%0D%0AX-Evil%3A%201");
}

TEST(AccelPath, PercentEncodesEveryOtherReservedByte) {
    EXPECT_EQ(encode_accel_path(" "), "%20");
    EXPECT_EQ(encode_accel_path("?"), "%3F");
    EXPECT_EQ(encode_accel_path("#"), "%23");
    EXPECT_EQ(encode_accel_path("%"), "%25");
    EXPECT_EQ(encode_accel_path(":"), "%3A");
}

TEST(AccelPath, PercentEncodesHighBytesAsTwoUppercaseHexDigits) {
    // A non-ASCII byte is not a thing a server-generated path contains, which is
    // precisely why the encoder must handle one rather than assume it away.
    const std::string encoded = encode_accel_path("\xC3\xA9");
    EXPECT_EQ(encoded, "%C3%A9");
}

TEST(AccelPath, EmptyInputIsEmptyOutput) { EXPECT_EQ(encode_accel_path(""), ""); }

// --- format negotiation -----------------------------------------------------

TEST(NegotiateFormat, PicksAvifOnlyWhenTheClientSaysItCanDecodeOne) {
    EXPECT_EQ(negotiate_format("image/avif,image/webp,*/*"), Format::Avif);
    EXPECT_EQ(negotiate_format("image/avif"), Format::Avif);
}

TEST(NegotiateFormat, FallsBackToWebpForEveryOtherHeader) {
    // Including an absent header and a bare `*/*`, which is what every
    // non-browser client sends.
    EXPECT_EQ(negotiate_format(""), Format::Webp);
    EXPECT_EQ(negotiate_format("*/*"), Format::Webp);
    EXPECT_EQ(negotiate_format("image/webp,image/png"), Format::Webp);
    EXPECT_EQ(negotiate_format("text/html"), Format::Webp);
}

TEST(NegotiateFormat, DoesNotThrowOrAllocateOnAHostileHeader) {
    // The header is attacker-controlled and unbounded. A substring scan is the
    // whole implementation on purpose: a full RFC 7231 parse would be a
    // request-path parser written to answer a boolean.
    const std::string hostile(64 * 1024, ',');
    EXPECT_EQ(negotiate_format(hostile), Format::Webp);
    EXPECT_EQ(negotiate_format(std::string(64 * 1024, 'a') + "image/avif"), Format::Avif);
}

// --- role resolution --------------------------------------------------------

TEST(ResolveRole, PicksTheLargestWrittenVariantAtOrBelowTheRoleWidth) {
    const std::vector<VariantRecord> variants{variant(320, Format::Avif),
                                              variant(640, Format::Avif),
                                              variant(1024, Format::Avif)};
    EXPECT_EQ(resolve_role(variants, 1024, Format::Avif).width, 1024);
    EXPECT_EQ(resolve_role(variants, 800, Format::Avif).width, 640);
    EXPECT_EQ(resolve_role(variants, 640, Format::Avif).width, 640);
}

TEST(ResolveRole, PrefersTheNegotiatedFormatWhenItHasAnythingUsable) {
    const std::vector<VariantRecord> variants{variant(320, Format::Avif),
                                              variant(640, Format::Webp)};
    // The negotiated format wins even though the other one has a WIDER variant:
    // the point of AVIF is the byte count, not the pixel count.
    EXPECT_EQ(resolve_role(variants, 1024, Format::Avif).width, 320);
    EXPECT_EQ(resolve_role(variants, 1024, Format::Avif).format, Format::Avif);
    EXPECT_EQ(resolve_role(variants, 1024, Format::Webp).width, 640);
    EXPECT_EQ(resolve_role(variants, 1024, Format::Webp).format, Format::Webp);
}

// The defect the first consumer hit, in one case. A build whose libvips has no
// libheif writes no AVIF variant at all, and negotiate_format answers Avif for
// essentially every current browser — so this used to serve the normalised
// master, a multi-megabyte PNG, to a phone, in place of a 90 KB WebP.
TEST(ResolveRole, DegradesToWebpRatherThanServingTheMaster) {
    const std::vector<VariantRecord> variants{variant(320, Format::Webp),
                                              variant(640, Format::Webp)};
    const VariantKey resolved = resolve_role(variants, 640, Format::Avif);

    EXPECT_FALSE(resolved.is_master());
    EXPECT_EQ(resolved.width, 640);
    EXPECT_EQ(resolved.format, Format::Webp);
}

// The direction is the whole of the rule. negotiate_format answers Webp
// PRECISELY for the clients that did not say they decode AVIF, so degrading the
// other way would hand those clients an image they cannot render — a symmetric
// "try the other format" is the obvious spelling and is wrong in exactly the
// direction that reaches the oldest clients.
TEST(ResolveRole, NeverDegradesWebpToAvif) {
    const std::vector<VariantRecord> variants{variant(320, Format::Avif),
                                              variant(640, Format::Avif)};
    const VariantKey resolved = resolve_role(variants, 640, Format::Webp);

    EXPECT_TRUE(resolved.is_master())
        << "a client that never advertised AVIF must not be sent one";
}

// A variant that FITS beats one that does not, even at the cost of the format:
// fewer bytes and the right dimensions both.
TEST(ResolveRole, AFittingDegradedVariantBeatsAnOversizedNegotiatedOne) {
    const std::vector<VariantRecord> variants{variant(1024, Format::Avif),
                                              variant(320, Format::Webp)};
    const VariantKey resolved = resolve_role(variants, 320, Format::Avif);

    EXPECT_EQ(resolved.width, 320);
    EXPECT_EQ(resolved.format, Format::Webp);
}

// Falling to the master WAS itself a failure upward, and a larger one than any
// variant: the master is the widest and least compressed object on disk. So a
// role narrower than every written variant takes the narrowest variant instead.
TEST(ResolveRole, TakesTheNarrowestVariantRatherThanTheWiderMaster) {
    const std::vector<VariantRecord> variants{variant(640, Format::Avif),
                                              variant(1024, Format::Avif)};
    const VariantKey resolved = resolve_role(variants, 320, Format::Avif);

    EXPECT_FALSE(resolved.is_master());
    EXPECT_EQ(resolved.width, 640);
}

TEST(ResolveRole, FallsBackToTheMasterWhenNoVariantWasWrittenAtAll) {
    // The one case the master is still right for. The object exists and still
    // has to serve, and its stored mime is a type every client decodes.
    EXPECT_TRUE(resolve_role({}, 2560, Format::Avif).is_master());
    EXPECT_TRUE(resolve_role({}, 2560, Format::Webp).is_master());
}

// --- the response -----------------------------------------------------------

TEST(AccelResponse, CarriesAnEmptyBodyAndTheRedirectHeader) {
    const anvil::Uuid id{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                         0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
    const Ns ns = *Ns::from_index(1);
    const auto response =
        accel_redirect_response(ns, id, VariantKey{640, Format::Avif}, anvil::fs::Mime::Jpeg);

    // Empty body: Nginx replaces it with the file, copied disk-to-socket inside
    // the kernel. A byte of the image entering this heap is the thing the whole
    // design exists to avoid.
    EXPECT_TRUE(response->body().empty());

    const std::string redirect{response->getHeader("X-Accel-Redirect")};
    EXPECT_TRUE(redirect.starts_with("/protected_storage/"));
    EXPECT_NE(redirect.find("w640.avif"), std::string::npos);
    // The encoding is applied unconditionally, so the header can carry no
    // control character whatever the path builder did.
    EXPECT_EQ(redirect.find('\r'), std::string::npos);
    EXPECT_EQ(redirect.find('\n'), std::string::npos);
}

TEST(AccelResponse, TakesTheContentTypeFromTheVariantNotTheStoredMime) {
    const anvil::Uuid id{};
    const Ns ns = *Ns::from_index(0);
    // The stored master is a JPEG; the variant being served is an AVIF, and the
    // header must describe the FILE that will be sent.
    const auto response =
        accel_redirect_response(ns, id, VariantKey{320, Format::Avif}, anvil::fs::Mime::Jpeg);
    // contentTypeString() rather than getHeader("Content-Type"): Drogon keeps the
    // content type out of the header map until the response is serialised, so
    // asserting through getHeader passes vacuously against an empty string.
    EXPECT_EQ(response->contentTypeString(), "image/avif");
}

TEST(AccelResponse, TakesTheContentTypeFromTheStoredEnumWhenServingTheMaster) {
    const anvil::Uuid id{};
    const Ns ns = *Ns::from_index(0);
    const auto response =
        accel_redirect_response(ns, id, anvil::fs::kMasterVariant, anvil::fs::Mime::Png);
    // From the STORED enum, never from anything the upload claimed. A file must
    // not be able to choose how a browser interprets it.
    EXPECT_EQ(response->contentTypeString(), "image/png");
}

TEST(AccelResponse, SetsNosniffPrivateCachingAndVaryOnAccept) {
    const anvil::Uuid id{};
    const Ns ns = *Ns::from_index(0);
    const auto response =
        accel_redirect_response(ns, id, VariantKey{320, Format::Webp}, anvil::fs::Mime::Jpeg);

    // Without nosniff a browser may sniff a crafted file as HTML and execute it
    // on the origin serving media.
    EXPECT_EQ(response->getHeader("X-Content-Type-Options"), "nosniff");
    // These routes are access-controlled, so a shared cache holding the response
    // would serve it to the next requester.
    EXPECT_NE(std::string{response->getHeader("Cache-Control")}.find("private"),
              std::string::npos);
    // The body varies with Accept and with nothing else a cache can see.
    EXPECT_EQ(response->getHeader("Vary"), "Accept");
}

TEST(AccelResponse, TheRedirectPathIsTheOneThePathBuilderProduces) {
    // Asserted against fs::media_relative_path rather than against a literal, so
    // a change to the storage layout cannot leave the redirect naming a file
    // that is no longer there — which would turn a clean 404 into a failure
    // inside the proxy.
    const anvil::Uuid id{0xaa, 0xbb, 0xcc, 0xdd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const Ns ns = *Ns::from_index(2);
    const VariantKey key{1024, Format::Webp};

    const auto response = accel_redirect_response(ns, id, key, anvil::fs::Mime::Jpeg);
    const anvil::fs::RelPath expected = anvil::fs::media_relative_path(ns, id, key);
    std::string want{"/protected_storage/"};
    want.append(expected.view());
    EXPECT_EQ(response->getHeader("X-Accel-Redirect"), encode_accel_path(want));
}

}  // namespace
