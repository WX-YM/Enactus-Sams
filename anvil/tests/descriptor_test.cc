// The client descriptor.
//
// What is asserted here is what a generated client is generated FROM. A
// descriptor that is wrong is not a wrong response; it is a client built with
// the wrong authority model, and nothing in the request path will notice.

#include <gtest/gtest.h>

#include <stdexcept>

#include <array>
#include <string>
#include <string_view>

#include "anvil/descriptor/descriptor.h"
#include "anvil/descriptor/route_description.h"
#include "anvil/http/errors.h"
#include "anvil/http/response_spec.h"

#include "capabilities.h"
#include "chat_kinds.h"
#include "events.h"
#include "field_types.h"
#include "perms.h"
#include "rate_limits.h"
#include "responses.h"
#include "route_descriptions.h"
#include "routes.h"
#include "sections.h"
#include "topics.h"

namespace anvil::descriptor {

namespace {

[[nodiscard]] DescriptorInput reference_input() {
    return DescriptorInput{
        .app_name = "testapp",
        .app_version = "0.0.0",
        .permissions = testapp::kPermNameTable,
        .routes = testapp::kRoutes,
        .route_descriptions = testapp::kRouteDescriptions,
        .capability_scopes = testapp::kScopes,
        .rate_limits = testapp::kRateLimits,
        .field_types = testapp::kFieldTypes,
        .sections = testapp::kSections,
        .topics = testapp::kTopics,
        .events = testapp::kEvents,
        .chat_kinds = testapp::kChatKinds,
        .limits = Limits{.upload_max_bytes = 26214400,
                         .body_max_bytes = 262144,
                         .page_limit_max = 100},
    };
}

// Substring search, because the assertion is about what the bytes CONTAIN. A
// JSON parser here would assert that a parser works.
// How many times `needle` occurs. Used where "absent" is too blunt an assertion:
// a token that may appear inside one longer string and nowhere else is a
// different property from a token that may not appear at all.
[[nodiscard]] std::size_t count_of(std::string_view haystack, std::string_view needle) {
    std::size_t found = 0;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + 1)) {
        ++found;
    }
    return found;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

TEST(Descriptor, ARoutePagingPastTheGlobalCeilingIsRefused) {
    // The reference tables agree with themselves...
    EXPECT_TRUE(page_ceiling_covers(testapp::kRouteDescriptions, reference_input().limits));
    // ...and a ceiling below the largest route (chat.list, chat.history and
    // chat.members page to 100) is a descriptor that contradicts itself.
    DescriptorInput lower = reference_input();
    lower.limits.page_limit_max = 64;
    EXPECT_FALSE(page_ceiling_covers(testapp::kRouteDescriptions, lower.limits));
    EXPECT_THROW((void)emit_descriptor(lower), std::invalid_argument);
}

TEST(Descriptor, IsDeterministic) {
    // The hash is over these bytes, so two runs producing two byte strings means
    // the staleness check reports drift on every deploy and is then ignored.
    const std::string first = emit_descriptor(reference_input());
    const std::string second = emit_descriptor(reference_input());
    EXPECT_EQ(first, second);
}

TEST(Descriptor, HashCoversTheTablesAndNotTheMetadata) {
    DescriptorInput input = reference_input();
    const std::string base = emit_descriptor(input);

    // A version bump changes no table, so it must not invalidate every client
    // that is running perfectly good code.
    input.app_version = "0.0.1";
    const std::string bumped = emit_descriptor(input);
    EXPECT_NE(base, bumped);

    const std::size_t base_hash = base.find("\"hash\":");
    const std::size_t bumped_hash = bumped.find("\"hash\":");
    ASSERT_NE(base_hash, std::string::npos);
    ASSERT_NE(bumped_hash, std::string::npos);
    EXPECT_EQ(base.substr(base_hash, 80), bumped.substr(bumped_hash, 80));

    // A table change must.
    constexpr std::array<PermName, 1> kFewer{{{"ContentRead", 0}}};
    input = reference_input();
    input.permissions = kFewer;
    EXPECT_NE(base.substr(base_hash, 80),
              emit_descriptor(input).substr(emit_descriptor(input).find("\"hash\":"), 80));
}

TEST(Descriptor, CarriesEveryErrorCodeAndReason) {
    const std::string doc = emit_descriptor(reference_input());

    // The two vocabularies a client maps to its own words. A code with no name
    // here is a code that reaches a user as a blank.
    EXPECT_TRUE(contains(doc, "\"VALIDATION_FAILED\""));
    EXPECT_TRUE(contains(doc, "\"VERSION_MISMATCH\""));
    EXPECT_TRUE(contains(doc, "\"INSUFFICIENT_STORAGE\""));
    EXPECT_TRUE(contains(doc, "\"BAD_FORMAT\""));
    EXPECT_TRUE(contains(doc, "\"BREACHED\""));

    // And the bound, so a decoder has one rather than a belief.
    EXPECT_TRUE(contains(doc, "\"max\":14"));
}

TEST(Descriptor, MarksWhichCodesTheStealthFilterRewrites) {
    const std::string doc = emit_descriptor(reference_input());
    const std::size_t forbidden = doc.find("\"FORBIDDEN\"");
    ASSERT_NE(forbidden, std::string::npos);
    EXPECT_TRUE(contains(doc.substr(forbidden, 120), "\"stealth_hidden\":true"));

    const std::size_t conflict = doc.find("\"CONFLICT\"");
    ASSERT_NE(conflict, std::string::npos);
    EXPECT_TRUE(contains(doc.substr(conflict, 120), "\"stealth_hidden\":false"));
}

TEST(Descriptor, SeparatesAPublicPathFromAHolderPath) {
    const std::string doc = emit_descriptor(reference_input());

    // /login is reachable with no credential, so a bundle may hold it.
    const std::size_t login = doc.find("\"auth.login\"");
    ASSERT_NE(login, std::string::npos);
    EXPECT_TRUE(contains(doc.substr(login, 200), "\"visibility\":\"public\""));

    // /audit is stealth. Its path is in the descriptor — which is a build
    // artefact and is never served — and the visibility is what tells a
    // generator never to emit it into a bundle.
    const std::size_t audit = doc.find("\"audit.list\"");
    ASSERT_NE(audit, std::string::npos);
    EXPECT_TRUE(contains(doc.substr(audit, 200), "\"visibility\":\"holder\""));
    EXPECT_TRUE(contains(doc.substr(audit, 200), "\"access\":\"stealth\""));
}

TEST(Descriptor, CarriesThePermissionsARouteRequiresByName) {
    const std::string doc = emit_descriptor(reference_input());
    const std::size_t del = doc.find("\"content.delete\"");
    ASSERT_NE(del, std::string::npos);

    const std::string_view entry{doc.data() + del, 260};
    EXPECT_TRUE(contains(entry, "\"ContentDelete\""));
    EXPECT_TRUE(contains(entry, "\"capability\":\"ContentDelete\""));
    // Not idempotent, and the scope is single-use: between them that is the
    // instruction never to retry this call.
    EXPECT_TRUE(contains(entry, "\"idempotent\":false"));
}

TEST(Descriptor, ListRoutesCarryACursorAndACeiling) {
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t list = doc.find("\"media.list\"");
    ASSERT_NE(list, std::string::npos);
    EXPECT_TRUE(contains(std::string_view{doc.data() + list, 260},
                         "\"page\":{\"cursor\":\"_id\",\"limit_max\":100}"));

    // A route that is not a list says so explicitly rather than omitting the
    // key: a client branching on "is this member present" branches on a typo.
    const std::size_t me = doc.find("\"identity.me\"");
    ASSERT_NE(me, std::string::npos);
    EXPECT_TRUE(contains(std::string_view{doc.data() + me, 200}, "\"page\":null"));
}

// --- the content tables -----------------------------------------------------

TEST(Descriptor, PutsTheFieldTypeFlagsInAVocabularyAClientAlreadySpeaks) {
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t multi = doc.find("\"CHECKBOX_MULTI\"");
    ASSERT_NE(multi, std::string::npos);
    const std::string_view entry{doc.data() + multi, 260};
    // Named booleans rather than the byte the server stores. A client handed
    // `"flags":17` needs a copy of anvil's enum to read it, and a copy of an enum
    // is the second table this whole document exists to remove.
    EXPECT_TRUE(contains(entry, "\"multi_select\":true"));
    EXPECT_TRUE(contains(entry, "\"multi_line\":false"));
    // The JSON shape an answer takes, derived from those same flags in ONE place
    // so a client and the submission service cannot disagree about it.
    EXPECT_TRUE(contains(entry, "\"answer\":\"choices\""));
}

TEST(Descriptor, MarksAPiiFieldTypeAsProducingNoAnswerAtAll) {
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t identity = doc.find("\"IDENTITY\"");
    ASSERT_NE(identity, std::string::npos);
    const std::string_view entry{doc.data() + identity, 260};
    EXPECT_TRUE(contains(entry, "\"pii\":true"));
    // NULL, not "text". A PII value is never echoed back, so a form renderer
    // that expects one in the document it reads afterwards renders a field that
    // has silently emptied itself.
    EXPECT_TRUE(contains(entry, "\"answer\":null"));
}

TEST(Descriptor, CarriesASectionsLabelsInEveryDeclaredLocale) {
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t hero = doc.find("\"home.hero\"");
    ASSERT_NE(hero, std::string::npos);
    const std::string_view entry{doc.data() + hero, 1400};
    // The page a preview URL has to land the reader on, which the registry holds
    // and nothing else should be deriving from the key.
    EXPECT_TRUE(contains(entry, "\"site_path\""));
    // One label per declared locale, in the locale table's order — the order the
    // stored one-byte index means. Without them the only string a generated
    // editor has is the key, so the control reads `cta_href`.
    EXPECT_TRUE(contains(entry, "\"Headline\""));
    EXPECT_TRUE(contains(entry, "الع"));
    // CODE POINTS in the name: a client enforcing the bound in UTF-16 code units
    // refuses text the server would have accepted.
    EXPECT_TRUE(contains(entry, "\"max_code_points\":80"));
}

TEST(Descriptor, EmitsANullAspectRatherThanAZeroedOne) {
    const std::string doc = emit_descriptor(reference_input());

    // An object is always truthy, so a slot with no constraint serialised as
    // `{"num":0,"den":0}` renders "shaped 0:0" beside the upload control — a
    // requirement a staff member can neither satisfy nor recognise as nothing
    // being asked.
    EXPECT_TRUE(contains(doc, "\"aspect\":null"));
    EXPECT_FALSE(contains(doc, "\"num\":0"));
}

TEST(Descriptor, SeparatesAPublicTopicFromAGatedOne) {
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t published = doc.find("\"content.published\"");
    ASSERT_NE(published, std::string::npos);
    EXPECT_TRUE(contains(std::string_view{doc.data() + published, 320},
                         "\"visibility\":\"public\""));

    // SUBSCRIPTION IS THE DISCLOSURE: a topic gated by a permission is one whose
    // existence is part of what the permission protects, so its name may not be
    // compiled into a bundle — the same split path_in_bundle draws over routes.
    const std::size_t submitted = doc.find("\"form.submitted\"");
    ASSERT_NE(submitted, std::string::npos);
    const std::string_view entry{doc.data() + submitted, 320};
    EXPECT_TRUE(contains(entry, "\"visibility\":\"holder\""));
    EXPECT_TRUE(contains(entry, "\"FormRead\""));
    // Channels by name, so a client needs no copy of the ClientType bit layout
    // to read a preference mask it is about to render.
    EXPECT_TRUE(contains(entry, "\"default_channels\":[\"in_app\",\"email\",\"webhook\"]"));
}

TEST(Descriptor, MarksATopicTheReaderMayNotSilence) {
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t device = doc.find("\"session.new_device\"");
    ASSERT_NE(device, std::string::npos);
    // A client that offers a switch here offers a switch the server refuses —
    // and an account that can silence its own sign-in alert has no alert.
    EXPECT_TRUE(contains(std::string_view{doc.data() + device, 320},
                         "\"user_optional\":false"));
}

TEST(Descriptor, CarriesTheClosedDimensionValuesAndTheConsentFlag) {
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t viewed = doc.find("\"PageViewed\"");
    ASSERT_NE(viewed, std::string::npos);
    const std::string_view entry{doc.data() + viewed, 400};
    // Refused at the door rather than filtered later, so a client that offers the
    // event without consent is reporting into a refusal it cannot see.
    EXPECT_TRUE(contains(entry, "\"requires_consent\":true"));
    // The CLOSED set, because the row stores the INDEX into it. A dimension whose
    // values came from a request is the cardinality explosion the set refuses.
    EXPECT_TRUE(contains(entry, "\"values\":[\"web\",\"ios\",\"android\"]"));
    // Every ordinary dimension says so explicitly, so a client can tell the two
    // kinds apart without inferring one from an empty "values" array.
    EXPECT_TRUE(contains(entry, "\"kind\":\"enum\""));
}

TEST(Descriptor, AnEntityDimensionCarriesItsKindAndAnEmptyValueSet) {
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t viewed = doc.find("\"ProjectViewed\"");
    ASSERT_NE(viewed, std::string::npos);
    const std::string_view entry{doc.data() + viewed, 400};
    // No closed set to enumerate — the value space is an application id
    // admitted one at a time at ingest, not a table a client could read
    // (docs/17-analytics.md §19) — so a client sees the kind and an empty
    // array rather than inferring "entity" from the array's emptiness alone.
    EXPECT_TRUE(contains(entry, "\"kind\":\"entity\""));
    EXPECT_TRUE(contains(entry, "\"values\":[]"));
}

TEST(Descriptor, AttachesAWidthToARoleAndShipsNoLadderToBuildPathsFrom) {
    const std::string doc = emit_descriptor(reference_input());

    // The reconciliation the phase found and deferred: a responsive srcset needs
    // width descriptors, and fs/namespace_spec.h refuses to let a client know the
    // ladder. Both hold, because the number is attached to the ROLE — the client
    // writes `/media/content/{id}/thumb 320w` and still cannot assemble a path.
    const std::size_t content = doc.find("\"ns\":\"content\"");
    ASSERT_NE(content, std::string::npos);
    EXPECT_TRUE(contains(std::string_view{doc.data() + content, 320},
                         "{\"role\":\"thumb\",\"width\":320}"));
    EXPECT_TRUE(contains(doc, "\"default_role\":\"card\""));

    // The edit recipe's bounds, so a client's validator and this server's
    // agree: the codec's two caps, and the ladder's two ends as NUMBERS beside
    // names — not a ladder a path could be assembled from.
    EXPECT_TRUE(contains(doc, "\"edit\":{\"max_strokes\":64,\"max_points\":4096,"
                              "\"max_edge_px\":2560,\"min_edge_px\":320}"));

    // And nothing a path could be built out of: no bare ladder, and no file
    // EXTENSION. A client that knows those is a client that will start building
    // `w640.avif` again.
    //
    // This read `EXPECT_FALSE(contains(doc, "avif"))` until the namespace accept
    // lists landed, and the blanket form had to become a precise one rather than
    // be deleted. `image/avif` is now published, because it is a fact about what
    // a namespace will TAKE — an upload picker's list — and `avif` is the
    // extension the variant filenames use, which is a fact about what the server
    // WRITES and stays unpublished. So the assertion is that every occurrence of
    // the token is inside a media type and none of it stands alone: a client can
    // read "this namespace accepts AVIF" and still cannot spell a variant's name.
    //
    // The distinction holds because the variant filename is not URL-addressable
    // at all. The public grammar is a ROLE (docs/08-images.md §4), the file is
    // reached through X-Accel-Redirect, and no width ever appears in a path a
    // client can send.
    EXPECT_EQ(count_of(doc, "avif"), count_of(doc, "image/avif"));
    EXPECT_EQ(count_of(doc, "webp"), count_of(doc, "image/webp"));
    EXPECT_GT(count_of(doc, "image/avif"), 0U);
    EXPECT_FALSE(contains(doc, "\"widths\""));
    // The extension itself, in the two spellings a path would carry it in.
    EXPECT_FALSE(contains(doc, ".avif"));
    EXPECT_FALSE(contains(doc, "\"avif\""));
}

TEST(Descriptor, TheMediaGrammarIsARouteRatherThanAFieldOnTheMediaObject) {
    const std::string doc = emit_descriptor(reference_input());

    // docs/08-images.md §4 settles the grammar and no table declared it, so every
    // application wrote the pattern by hand. It is a route like any other now,
    // which is where a claim of this kind gets checked.
    const std::size_t object = doc.find("\"media.object\"");
    ASSERT_NE(object, std::string::npos);
    const std::string_view entry{doc.data() + object, 300};
    EXPECT_TRUE(contains(entry, "\"path\":\"/media/{ns}/{id}/{role}\""));
    EXPECT_TRUE(contains(entry, "\"method\":\"GET\""));
    // Public, so a client compiles the path in — which the grammar being public
    // is exactly what means.
    EXPECT_TRUE(contains(entry, "\"visibility\":\"public\""));
    // And distinct from the LIST route at the shorter pattern: one serves an
    // object, the other pages a collection, and folding them would be the second
    // address again.
    EXPECT_TRUE(contains(entry, "\"page\":null"));

    // NOT a field on the media object. That table says what a role IS; the route
    // table says where a route LIVES.
    const std::size_t media = doc.find("\"media\":{");
    ASSERT_NE(media, std::string::npos);
    const std::string_view media_object{doc.data() + media, doc.size() - media};
    EXPECT_FALSE(contains(media_object, "/media/"));
    EXPECT_FALSE(contains(media_object, "\"path\""));
}

TEST(Descriptor, EveryMediaPathSegmentIsAKeyTheMediaObjectSuppliesValuesFor) {
    // The join a generated client makes, asserted rather than assumed. `{ns}` and
    // `{role}` are enumerations because the media object publishes their value
    // sets under those exact names; rename either segment and the join silently
    // degrades to a free string, which is a client that can spell a path the
    // server will 404.
    //
    // `{id}` is the exception and the reason this walks the pattern rather than
    // naming two segments: it is the caller's own object id and no table
    // enumerates it.
    const descriptor::RouteDescription* description =
        description_for(testapp::kRouteDescriptions, "media.object");
    ASSERT_NE(description, nullptr);

    const std::string doc = emit_descriptor(reference_input());
    const std::size_t media = doc.find("\"media\":{");
    ASSERT_NE(media, std::string::npos);
    const std::string_view media_object{doc.data() + media, doc.size() - media};

    std::size_t segments = 0;
    const std::string_view pattern = description->pattern;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '{') { continue; }
        const std::size_t close = pattern.find('}', i);
        ASSERT_NE(close, std::string_view::npos) << pattern;
        const std::string_view name = pattern.substr(i + 1, close - i - 1);
        i = close;
        if (name == "id") { continue; }
        ++segments;
        EXPECT_TRUE(contains(media_object, "\"" + std::string{name} + "\":"))
            << name << " is a path segment the media object enumerates no values for";
    }
    // Two of them, so a pattern that lost a segment cannot pass by having none.
    EXPECT_EQ(segments, 2U);
}

TEST(Descriptor, ServingAnObjectCountsIntoNoRateBucket) {
    // The decision not taken, pinned. The `media` rule is 20 a minute and is
    // sized for uploads — one libvips decode on cpu_pool each — and a gallery
    // page is thirty images in one paint. Counting serves into it would answer
    // 429 to an ordinary render, and the symptom would be images vanishing above
    // the fold rather than anything that looks like a limit.
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t object = doc.find("\"media.object\"");
    ASSERT_NE(object, std::string::npos);
    EXPECT_TRUE(contains(std::string_view{doc.data() + object, 300}, "\"rate_limit\":\"\""));

    // The upload and delete routes still carry it, so this is a statement about
    // the serving route rather than about the bucket having been dropped.
    const std::size_t del = doc.find("\"media.delete\"");
    ASSERT_NE(del, std::string::npos);
    EXPECT_TRUE(contains(std::string_view{doc.data() + del, 300}, "\"rate_limit\":\"media\""));
}

TEST(Descriptor, EachNamespacePublishesTheTypesItWillActuallyTake) {
    const std::string doc = emit_descriptor(reference_input());

    // The list a client's file picker is built from, published per namespace
    // rather than globally — because the moment one namespace is narrower, a
    // global list gets written into the client by hand again.
    const std::size_t content = doc.find("\"ns\":\"content\"");
    ASSERT_NE(content, std::string::npos);
    EXPECT_TRUE(contains(std::string_view{doc.data() + content, 200},
                         "\"accepts\":[\"image/jpeg\",\"image/png\",\"image/webp\","
                         "\"image/avif\"]"));

    // The narrowed one, and the reason this feature is not a global list.
    const std::size_t guest = doc.find("\"ns\":\"guest\"");
    ASSERT_NE(guest, std::string::npos);
    EXPECT_TRUE(contains(std::string_view{doc.data() + guest, 200},
                         "\"accepts\":[\"image/jpeg\",\"image/png\"]"));

    // SVG never appears, and not because nobody listed it: there is no Mime for
    // it at all, because fs/sniff.h refuses it EXPLICITLY so an upload attempt
    // is auditable as the probe it is rather than as an unrecognised file.
    EXPECT_FALSE(contains(doc, "svg"));
}

// --- the response half of the descriptor ------------------------------------

TEST(Descriptor, ADescribedRouteCarriesItsFieldsInDeclarationOrder) {
    // The one route in the reference table with a described body, emitted as the
    // shape a client generates a TYPE from. Order is asserted as bytes because a
    // client that reads the fields positionally is the one a reordering breaks
    // silently.
    const std::string doc = emit_descriptor(reference_input());
    EXPECT_TRUE(contains(doc,
                         R"("response":{"kind":"object","fields":[)"
                         R"({"name":"id","type":"uuid","nullable":false},)"
                         R"({"name":"session_id","type":"uuid","nullable":false},)"
                         R"({"name":"locale","type":"string","nullable":false},)"
                         R"({"name":"permissions","type":"strings","nullable":true}]})"))
        << doc;
}

TEST(Descriptor, AnUndescribedRouteSaysSoRatherThanOmittingTheKey) {
    // `null` and not a missing key, for the reason `"page":null` is not a missing
    // key either: a generator branching on "is this member present" branches on a
    // typo. Most reference routes are undescribed, which is the honest state of
    // a table where adoption is per route; three are — `/me` and the two image
    // edit routes, whose shapes are the library's own.
    const std::string doc = emit_descriptor(reference_input());
    EXPECT_EQ(count_of(doc, R"("response":null)"), testapp::kRouteDescriptions.size() - 3) << doc;
}

TEST(Descriptor, TheNestedSessionBodyIsLeftUndescribedRatherThanApproximated) {
    // `session.current` returns `{"routes":{…},"authority":{…}}`, which the flat
    // grammar cannot express. Saying so is the point of this case: a schema that
    // described the envelope and not its contents would be one a client trusts
    // and is wrong about, and the refusal is a decision rather than an oversight.
    const std::string doc = emit_descriptor(reference_input());
    const std::size_t at = std::string_view{doc}.find(R"("id":"session.current")");
    ASSERT_NE(at, std::string_view::npos);
    const std::string_view route = std::string_view{doc}.substr(at, 400);
    EXPECT_TRUE(contains(route, R"("response":null)")) << route;
}

TEST(Descriptor, TheFormatNumberSaysAResponseMayBeNonNull) {
    // The number a generator reads to know that `"response"` can carry a shape
    // at all. A generator written for 2 assumed null everywhere and was right;
    // what it cannot do is discover that it is now sometimes wrong.
    EXPECT_GE(kDescriptorFormat, 3);
    EXPECT_TRUE(contains(emit_descriptor(reference_input()),
                         R"("descriptor":)" + std::to_string(kDescriptorFormat)));
}

TEST(Descriptor, ChatPublishesEachKindFromTheTableTheServerEnforces) {
    const std::string doc = emit_descriptor(reference_input());
    const std::size_t at = doc.find(R"("chat":{"text_max_code_points":4096,)");
    ASSERT_NE(at, std::string::npos) << doc;
    // Up to the table after `limits`, so what is asserted absent is absent from
    // chat and not merely from the rest of the document.
    const std::size_t end = doc.find(R"("field_types":)", at);
    ASSERT_NE(end, std::string::npos);
    const std::string_view chat = std::string_view{doc}.substr(at, end - at);

    EXPECT_TRUE(contains(chat, R"("attachments_max":10,)"));
    EXPECT_TRUE(contains(chat, R"("mentions_max":32,)"));
    // Enums by name and rights by name: a client handed the stored byte would
    // need a copy of anvil's enum to read it.
    EXPECT_TRUE(contains(chat, R"({"key":"group","shape":"group","encryption":"optional",)"));
    EXPECT_TRUE(contains(chat, R"("create_requires":["ChatCreateGroup"])"));
    EXPECT_TRUE(contains(chat, R"({"key":"channel","shape":"channel","encryption":"never",)"));
    EXPECT_TRUE(contains(chat, R"("owner":["post","react","add_member")"));
    // Whether a kind takes attachments, and never where they are stored.
    EXPECT_TRUE(contains(chat, R"("attachments":true,)"));
    EXPECT_FALSE(contains(chat, "media_ns"));
    EXPECT_FALSE(contains(chat, "sealed"));
}

TEST(Descriptor, AnApplicationWithoutChatSaysSoRatherThanOmittingTheKey) {
    DescriptorInput input = reference_input();
    input.chat_kinds = {};
    EXPECT_TRUE(contains(emit_descriptor(input), R"("chat":null)"));
}

TEST(Descriptor, CarriesNoStorageVocabulary) {
    const std::string doc = emit_descriptor(reference_input());

    // A client has no business knowing the storage layout, and a collection name
    // in a bundle is a name in an attacker's notes.
    EXPECT_FALSE(contains(doc, "collection"));
    EXPECT_FALSE(contains(doc, "mongodb"));
    EXPECT_FALSE(contains(doc, "index"));
}

TEST(RouteDescriptions, RejectAMismatchedTable) {
    // The compile-time check the reference table asserts, exercised at run time
    // over tables that are wrong in each of the ways that matter.
    constexpr std::array<accesscontrol::RoutePolicy, 1> kOneRoute{{
        {PermSet{}, "/a", accesscontrol::RouteAccess::Public, accesscontrol::RouteMethod::Get},
    }};

    constexpr std::array<RouteDescription, 1> kWrongPattern{{
        {"a", "/b", "", "", "", 0, accesscontrol::RouteMethod::Get, true},
    }};
    static_assert(!descriptions_match(kOneRoute, kWrongPattern));

    constexpr std::array<RouteDescription, 1> kWrongMethod{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Post, true},
    }};
    static_assert(!descriptions_match(kOneRoute, kWrongMethod));

    constexpr std::array<RouteDescription, 1> kHalfAPage{{
        {"a", "/a", "", "", "_id", 0, accesscontrol::RouteMethod::Get, true},
    }};
    static_assert(!descriptions_match(kOneRoute, kHalfAPage));

    constexpr std::array<RouteDescription, 2> kDuplicateId{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Get, true},
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Post, true},
    }};
    static_assert(!descriptions_match(kOneRoute, kDuplicateId));

    SUCCEED();
}

// Two fields with one name. At namespace scope so a span into it is a constant
// expression; see the case below for why that matters here and nowhere else.
constexpr std::array<http::ResponseField, 2> kTwiceNamed{{
    {"id", http::FieldKind::Uuid, false},
    {"id", http::FieldKind::String, false},
}};

TEST(RouteDescriptions, AnUnusableResponseShapeIsRefusedAtCompileTime) {
    // The two ways a declared shape can be wrong in a manner a client generator
    // cannot recover from, refused where every other table mistake is: in the
    // static_assert beside the table.
    constexpr std::array<accesscontrol::RoutePolicy, 1> kOneRoute{{
        {PermSet{}, "/a", accesscontrol::RouteAccess::Public, accesscontrol::RouteMethod::Get},
    }};

    // A duplicated key. JSON does not forbid one and every parser resolves it
    // differently, so a client generated from the schema would declare one field
    // while the server sent two.
    //
    // `kTwiceNamed` is at namespace scope and not here, and that is not a style
    // choice: a span into a function-local `constexpr` array is a pointer to an
    // object with AUTOMATIC storage duration, so the description holding it is
    // not a constant expression and the `static_assert` below will not compile.
    // A real table is `inline constexpr` at namespace scope and has no such
    // problem — the restriction is the test's, not the seam's.
    constexpr std::array<RouteDescription, 1> kDuplicateKey{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Get, true, false, false,
         kTwiceNamed},
    }};
    static_assert(!descriptions_match(kOneRoute, kDuplicateKey));

    // "An array of nothing", which would emit a schema claiming a list of an
    // unknown thing — rendered by a generated client as a list of nothing.
    constexpr std::array<RouteDescription, 1> kArrayOfNothing{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Get, true, false, true},
    }};
    static_assert(!descriptions_match(kOneRoute, kArrayOfNothing));

    // And the shape the reference table actually declares still passes, so the
    // two refusals above are not refusing everything.
    constexpr std::array<RouteDescription, 1> kGood{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Get, true, false, false,
         testapp::kMeResponse},
    }};
    static_assert(descriptions_match(kOneRoute, kGood));

    SUCCEED();
}

// --- a description may not publish a path that must not be published --------

TEST(RouteDescriptions, BootstrapIsRefusedOnEveryShapeButTheOneItIsFor) {
    // `bootstrap` is the only field here that makes a claim about DISCLOSURE,
    // and what it discloses is a path. Taken on trust from the table, marking a
    // Stealth route would compile its path into a public bundle and undo the 404
    // that route exists for — silently, with every test green. So it is checked
    // against the POLICY it names.

    constexpr std::array<RouteDescription, 1> kClaims{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Get, true, true},
    }};

    // The shape it is for: Authenticated, no bit. A client cannot be told where
    // to ask for its table until it has asked.
    constexpr std::array<accesscontrol::RoutePolicy, 1> kAuthenticated{{
        {PermSet{}, "/a", accesscontrol::RouteAccess::Authenticated,
         accesscontrol::RouteMethod::Get},
    }};
    static_assert(descriptions_match(kAuthenticated, kClaims));

    // The map the stealth 404 exists to withhold.
    constexpr std::array<accesscontrol::RoutePolicy, 1> kStealth{{
        {perm_mask(testapp::Perm::AuditRead), "/a", accesscontrol::RouteAccess::Stealth,
         accesscontrol::RouteMethod::Get},
    }};
    static_assert(!descriptions_match(kStealth, kClaims));

    constexpr std::array<accesscontrol::RoutePolicy, 1> kGuarded{{
        {perm_mask(testapp::Perm::ContentWrite), "/a", accesscontrol::RouteAccess::Guarded,
         accesscontrol::RouteMethod::Get},
    }};
    static_assert(!descriptions_match(kGuarded, kClaims));

    // Authenticated but carrying bits. The filter ignores them on this class, so
    // the table has said something nothing reads — and a disclosure claim is the
    // last place to start trusting a field like that.
    constexpr std::array<accesscontrol::RoutePolicy, 1> kAuthenticatedWithBits{{
        {perm_mask(testapp::Perm::ContentRead), "/a",
         accesscontrol::RouteAccess::Authenticated, accesscontrol::RouteMethod::Get},
    }};
    static_assert(!descriptions_match(kAuthenticatedWithBits, kClaims));

    // Public. Redundant rather than dangerous — the path is in the bundle
    // either way — and refused so that exactly one reason ever puts it there.
    // A table where both can be true is a table a reader cannot interrogate.
    constexpr std::array<accesscontrol::RoutePolicy, 1> kPublic{{
        {PermSet{}, "/a", accesscontrol::RouteAccess::Public,
         accesscontrol::RouteMethod::Get},
    }};
    static_assert(!descriptions_match(kPublic, kClaims));

    // And the same policies with no claim on them are all fine, which is what
    // makes the five assertions above about the FLAG rather than about the
    // policies.
    constexpr std::array<RouteDescription, 1> kNoClaim{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Get, true, false},
    }};
    static_assert(descriptions_match(kStealth, kNoClaim));
    static_assert(descriptions_match(kGuarded, kNoClaim));
    static_assert(descriptions_match(kPublic, kNoClaim));
    static_assert(descriptions_match(kAuthenticatedWithBits, kNoClaim));

    SUCCEED();
}

TEST(RouteDescriptions, TheTwoArmsOfPathInBundleAreExclusive) {
    // path_in_bundle answers true for two different reasons, and the guard above
    // is what keeps them from ever both applying. That matters to a READER of a
    // route table: "why is this path public" always has one answer.
    for (const RouteDescription& d : testapp::kRouteDescriptions) {
        const accesscontrol::RoutePolicy* policy =
            accesscontrol::policy_for(testapp::kRoutes, d.pattern, d.method);
        ASSERT_NE(policy, nullptr) << d.id;
        EXPECT_FALSE(d.bootstrap && policy->access == accesscontrol::RouteAccess::Public)
            << d.id << " is in the bundle for two reasons at once";
    }
}

TEST(RouteDescriptions, TheBootstrapRouteIsToldTwoTrueThingsRatherThanOneFalseOne) {
    // The whole point of splitting the predicate. `session.current` is
    // Authenticated — so calling it without a session is a 401, which is what
    // lets a client tell "re-authenticate" from "route gone" — and its address
    // is public, because the address of the session arrives with the session,
    // which on a cold load does not exist.
    const std::string doc = emit_descriptor(reference_input());

    const std::size_t entry = doc.find("\"session.current\"");
    ASSERT_NE(entry, std::string::npos);
    const std::string_view view{doc.data() + entry, 320};

    EXPECT_TRUE(contains(view, "\"path\":\"/session\""));
    EXPECT_TRUE(contains(view, "\"access\":\"authenticated\""));
    EXPECT_TRUE(contains(view, "\"visibility\":\"public\""));
}

// --- a description may not say ANY ------------------------------------------

TEST(RouteDescriptions, AnAnyPolicyIsDescribedByTheMethodAClientSends) {
    // The POLICY may be Any — one handler dispatching internally is a real
    // shape. The DESCRIPTION is what a client calls with, and there is no method
    // called ANY: the emitter spells it "ANY" so a generated client fails
    // loudly, which put the failure on whoever generated a client rather than on
    // whoever wrote the table, and left this repository shipping a reference
    // route no client could call with every test green.
    constexpr std::array<accesscontrol::RoutePolicy, 1> kAnyPolicy{{
        {PermSet{}, "/a", accesscontrol::RouteAccess::Public, accesscontrol::RouteMethod::Any},
    }};

    constexpr std::array<RouteDescription, 1> kSaysAny{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Any, true},
    }};
    static_assert(!descriptions_match(kAnyPolicy, kSaysAny));

    // And it pairs through policy_for, so naming one real method against an Any
    // policy resolves exactly as the filter would resolve the request.
    constexpr std::array<RouteDescription, 1> kSaysPost{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Post, true},
    }};
    static_assert(descriptions_match(kAnyPolicy, kSaysPost));

    constexpr std::array<RouteDescription, 1> kSaysDelete{{
        {"a", "/a", "", "", "", 0, accesscontrol::RouteMethod::Delete, true},
    }};
    static_assert(descriptions_match(kAnyPolicy, kSaysDelete));

    SUCCEED();
}

TEST(RouteDescriptions, TwoMethodsOfOneAnyPolicyLeaveARouteUndescribed) {
    // The hole that resolving through policy_for opens, and the reason "every
    // route described exactly once" is a separate count rather than a
    // consequence of the two tables being the same size.
    //
    // Both descriptions below pair — an Any policy answers for POST and for
    // DELETE alike — so the sizes match, every id is unique, no (pattern,
    // method) repeats, and `/b` is described by nobody. Under the old equality
    // loop neither paired and the table was refused for the wrong reason; under
    // policy_for alone it would be accepted.
    constexpr std::array<accesscontrol::RoutePolicy, 2> kRoutes{{
        {PermSet{}, "/a", accesscontrol::RouteAccess::Public, accesscontrol::RouteMethod::Any},
        {PermSet{}, "/b", accesscontrol::RouteAccess::Public, accesscontrol::RouteMethod::Get},
    }};

    constexpr std::array<RouteDescription, 2> kBothOnA{{
        {"a.post", "/a", "", "", "", 0, accesscontrol::RouteMethod::Post, true},
        {"a.delete", "/a", "", "", "", 0, accesscontrol::RouteMethod::Delete, true},
    }};
    static_assert(!descriptions_match(kRoutes, kBothOnA));

    constexpr std::array<RouteDescription, 2> kOneEach{{
        {"a.post", "/a", "", "", "", 0, accesscontrol::RouteMethod::Post, true},
        {"b.get", "/b", "", "", "", 0, accesscontrol::RouteMethod::Get, true},
    }};
    static_assert(descriptions_match(kRoutes, kOneEach));

    SUCCEED();
}

TEST(RouteDescriptions, AnExactMethodWinsOverTheAnyFallbackJustAsTheFilterResolvesIt) {
    // Both entries share a pattern and one of them is Any, which is the shape
    // policy_for exists for: the exact method answers, and the Any entry catches
    // everything else. Describing both is then legal and each description
    // reaches a different policy.
    constexpr std::array<accesscontrol::RoutePolicy, 2> kExactAndFallback{{
        {PermSet{}, "/a", accesscontrol::RouteAccess::Public, accesscontrol::RouteMethod::Get},
        {PermSet{}, "/a", accesscontrol::RouteAccess::Public, accesscontrol::RouteMethod::Any},
    }};

    constexpr std::array<RouteDescription, 2> kExactAndOther{{
        {"a.get", "/a", "", "", "", 0, accesscontrol::RouteMethod::Get, true},
        {"a.post", "/a", "", "", "", 0, accesscontrol::RouteMethod::Post, true},
    }};
    static_assert(descriptions_match(kExactAndFallback, kExactAndOther));

    // Two descriptions that both fall through to the Any entry leave the exact
    // GET policy undescribed.
    constexpr std::array<RouteDescription, 2> kBothFallThrough{{
        {"a.post", "/a", "", "", "", 0, accesscontrol::RouteMethod::Post, true},
        {"a.put", "/a", "", "", "", 0, accesscontrol::RouteMethod::Put, true},
    }};
    static_assert(!descriptions_match(kExactAndFallback, kBothFallThrough));

    SUCCEED();
}

}  // namespace anvil::descriptor
