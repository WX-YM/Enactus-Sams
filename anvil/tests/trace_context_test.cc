// The parser and the formatter, pinned against the standard's own example.
//
// What is under test here is the GRAMMAR, not the wiring. That a context
// reaches a pool thread is asserted in thread_pools_test.cc, where the threads
// are; that no response ever carries one back is asserted over a real socket in
// session_listener_test.cc, where the sockets are. Splitting it that way is
// deliberate: a parser case that needed a listener would be a parser case nobody
// runs on every save.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "anvil/http/trace_context.h"

namespace anvil {
namespace {

// The example from the standard, which is also the shortest description of the
// grammar: version, trace-id, parent-id, flags, hex, three hyphens.
constexpr std::string_view kSample = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

[[nodiscard]] std::string rendered(const http::TraceContext& ctx) {
    const std::array<char, http::kTraceparentChars> out = http::format_traceparent(ctx);
    return std::string{out.data(), out.size()};
}

TEST(TraceContext, TheSampleParsesToTheBytesTheStandardSpells) {
    const std::optional<http::TraceContext> ctx = http::parse_traceparent(kSample);
    ASSERT_TRUE(ctx.has_value());

    const std::array<std::uint8_t, 16> expected_trace = {0x4b, 0xf9, 0x2f, 0x35, 0x77, 0xb3,
                                                         0x4d, 0xa6, 0xa3, 0xce, 0x92, 0x9d,
                                                         0x0e, 0x0e, 0x47, 0x36};
    const std::array<std::uint8_t, 8> expected_span = {0x00, 0xf0, 0x67, 0xaa,
                                                       0x0b, 0xa9, 0x02, 0xb7};
    EXPECT_EQ(ctx->trace_id, expected_trace);
    EXPECT_EQ(ctx->span_id, expected_span);
    EXPECT_EQ(ctx->flags, 0x01);
    EXPECT_TRUE(ctx->present());
    EXPECT_TRUE(ctx->sampled());
}

TEST(TraceContext, TheHeaderRoundTripsByteForByte) {
    // The property that keeps the pair honest. A formatter that dropped a
    // leading zero would still produce a value a lenient parser reads back, and
    // the two would then disagree with every other hop in the deployment about
    // what this trace is called.
    const std::optional<http::TraceContext> ctx = http::parse_traceparent(kSample);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(rendered(*ctx), kSample);
}

TEST(TraceContext, AnUnsampledFlagSurvivesTheRoundTrip) {
    const std::string unsampled =
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00";
    const std::optional<http::TraceContext> ctx = http::parse_traceparent(unsampled);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_FALSE(ctx->sampled());
    EXPECT_TRUE(ctx->present());
    EXPECT_EQ(rendered(*ctx), unsampled);
}

TEST(TraceContext, AReservedFlagBitIsCarriedRatherThanErased) {
    // The reason `flags` is the raw byte and not a bool. Seven of the eight bits
    // are reserved by the standard, and a hop that normalised them to whatever
    // it understands would silently downgrade every context it forwarded.
    const std::string reserved =
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-fd";
    const std::optional<http::TraceContext> ctx = http::parse_traceparent(reserved);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->flags, 0xfd);
    EXPECT_TRUE(ctx->sampled());
    EXPECT_EQ(rendered(*ctx), reserved);
}

TEST(TraceContext, EveryMalformationReadsAsAbsent) {
    // Table-driven, one row per malformation, because the failure mode this
    // guards against is a parser that accepts one shape it should not — and a
    // single hand-written negative case tests the shape its author thought of.
    struct Row final {
        std::string_view header;
        std::string_view why;
    };
    constexpr std::array<Row, 14> kRows = {{
        {"", "no header at all"},
        {"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0", "one byte short"},
        {"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-011", "one byte long"},
        {"00-00000000000000000000000000000000-00f067aa0ba902b7-01", "all-zero trace-id"},
        {"00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01", "all-zero parent-id"},
        {"00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01", "a non-hex nibble"},
        {"00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01", "uppercase hex"},
        {"0x-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "a non-hex version"},
        {"ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "the reserved version"},
        {"00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "no hyphen after version"},
        {"00-4bf92f3577b34da6a3ce929d0e0e4736_00f067aa0ba902b7-01", "no hyphen after trace-id"},
        {"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7_01", "no hyphen before flags"},
        {"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0z", "non-hex flags"},
        {"  00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
         "leading whitespace, which the fixed offsets make a different header"},
    }};

    for (const Row& row : kRows) {
        EXPECT_FALSE(http::parse_traceparent(row.header).has_value()) << row.why;
    }
}

TEST(TraceContext, AnUnknownVersionIsReadRatherThanRefused) {
    // Forward compatibility, and it is not free-form: a longer header is read as
    // version 00 over its first 55 bytes ONLY when the 56th is the separator
    // that proves the extra bytes are a new field. Refusing unknown versions
    // outright would make every deployment running this code the reason a future
    // version cannot be rolled out.
    const std::optional<http::TraceContext> future = http::parse_traceparent(
        "cc-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-whatever-comes-next");
    ASSERT_TRUE(future.has_value());
    EXPECT_TRUE(future->present());

    // Version 00 is a grammar that is fully specified, so trailing bytes on one
    // are not a field this process does not know about — they are a header
    // nothing agrees on.
    EXPECT_FALSE(http::parse_traceparent(
                     "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-trailing")
                     .has_value());

    // And the separator is what carries the distinction, so its absence is
    // refused even under an unknown version.
    EXPECT_FALSE(http::parse_traceparent(
                     "cc-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01xwhatever")
                     .has_value());
}

TEST(TraceContext, AFutureVersionIsEmittedAsTheOneThisProcessSpeaks) {
    // Read `cc`, write `00`. A hop that echoed the version it received would be
    // claiming to speak a grammar it has never read a field of, and the next hop
    // would believe it.
    const std::optional<http::TraceContext> ctx = http::parse_traceparent(
        "cc-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(rendered(*ctx), kSample);
}

TEST(TraceContext, TheDefaultValueIsTheAbsentOne) {
    // Why there is no `has_trace` flag anywhere beside one of these. The
    // standard forbids an all-zero trace-id, so the zero value is already spoken
    // for, and `parse_traceparent` is what guarantees nothing else can mint one.
    constexpr http::TraceContext empty{};
    static_assert(!empty.present());
    static_assert(!empty.sampled());
    EXPECT_FALSE(empty.present());
}

// --- who may set it ---------------------------------------------------------

TEST(TraceIngestPolicy, TheHeaderIsBelievedOnlyFromAHopThePolicyTrusts) {
    // The truth table, because the interesting half of it is the rows that
    // produce NOTHING — and a control that fails open produces nothing to look
    // at either. `client_address.h` records the shape: a control that is
    // present, called, and answering the wrong thing.
    struct Row final {
        http::TraceIngest policy;
        bool              peer_is_trusted;
        std::string_view  header;
        bool              believed;
        std::string_view  why;
    };
    const std::array<Row, 6> kRows = {{
        {http::TraceIngest::Off, true, kSample, false,
         "off is off, even from a hop that could be believed"},
        {http::TraceIngest::Off, false, kSample, false, "off, from an arbitrary client"},
        {http::TraceIngest::TrustedPeer, false, kSample, false,
         "a client that reached the process directly chose this id"},
        {http::TraceIngest::TrustedPeer, true, kSample, true, "the one row that believes"},
        {http::TraceIngest::TrustedPeer, true, "", false, "a trusted hop that sent none"},
        {http::TraceIngest::TrustedPeer, true, "00-not-a-traceparent-at-all", false,
         "a trusted hop is still not a reason to believe a malformed one"},
    }};

    for (const Row& row : kRows) {
        const http::TraceContext ctx =
            http::ingest_traceparent(row.policy, row.peer_is_trusted, row.header);
        EXPECT_EQ(ctx.present(), row.believed) << row.why;
    }
}

TEST(TraceIngestPolicy, AnEdgeProcessBelievesNothing) {
    // `peer_is_trusted_proxy` answers false when no proxy list is installed,
    // which is exactly the "this process is the edge" case. Asserted here as the
    // policy's own row rather than left to the caller, because an edge that
    // believed the header would be believing whatever the internet typed.
    EXPECT_FALSE(
        http::ingest_traceparent(http::TraceIngest::TrustedPeer, false, kSample).present());
}

// --- deferred work ----------------------------------------------------------

TEST(TraceLink, ALinkIsCarriedOnlyByASampledTrace) {
    const std::optional<http::TraceContext> sampled = http::parse_traceparent(kSample);
    ASSERT_TRUE(sampled.has_value());
    EXPECT_EQ(http::trace_link_of(*sampled), sampled->trace_id);

    // Unsampled produces no span, so a link to it would name a trace with
    // nothing in it — and an aggregator showing a link that resolves to nothing
    // is worse than one showing no link.
    http::TraceContext unsampled = *sampled;
    unsampled.flags = 0;
    EXPECT_FALSE(http::has_trace_link(http::trace_link_of(unsampled)));

    // And a value nobody minted: flags say sampled, trace-id says absent. The
    // id is what a link IS, so the absent one wins.
    http::TraceContext forged{};
    forged.flags = http::kTraceFlagSampled;
    EXPECT_FALSE(http::has_trace_link(http::trace_link_of(forged)));

    EXPECT_FALSE(http::has_trace_link(http::trace_link_of(http::TraceContext{})));
}

TEST(TraceLink, AnExecutionRootIsNotTheTraceItLinksTo) {
    // The assertion the whole shape exists for. A root that reused the enqueuing
    // trace id would make the job a CHILD of the request that queued it — and
    // then a job reclaimed after a lease expiry on Thursday would extend a span
    // that started on Monday. It is the most common mistake in traced job
    // systems and it is invisible until somebody opens the trace.
    const std::array<std::uint8_t, 16> link{0x4b, 0xf9, 0x2f, 0x35, 0x77, 0xb3,
                                            0x4d, 0xa6, 0xa3, 0xce, 0x92, 0x9d,
                                            0x0e, 0x0e, 0x47, 0x36};
    const http::TraceContext root = http::root_linked_to(link);
    ASSERT_TRUE(root.present());
    EXPECT_NE(root.trace_id, link);
    EXPECT_TRUE(root.sampled()) << "the link is only ever carried by a sampled "
                                   "enqueue, so a root that is not sampled "
                                   "produces nothing for it to point at";

    // Each execution is its own root, so a job that retries four times is four
    // traces rather than one span with four starts.
    const http::TraceContext second = http::root_linked_to(link);
    EXPECT_NE(second.trace_id, root.trace_id);
    EXPECT_NE(second.span_id, root.span_id);
}

TEST(TraceLink, NoLinkMeansNoRootAndNoCsprngCall) {
    // What "off by default" means at this seam. A job enqueued by an untraced
    // path has nothing reaching this code that says anybody wanted a trace, so
    // it runs without one — and pays no CSPRNG draw to find that out.
    EXPECT_FALSE(http::root_linked_to({}).present());
}

// --- the ambient slot -------------------------------------------------------
//
// The RAII contract on its own, with no threads in it. What a thread pool does
// with it is asserted in thread_pools_test.cc; what is asserted here is the part
// that has to hold on ANY thread that serves more than one thing in its life —
// an event loop above all, which serves every connection it owns from one stack.

[[nodiscard]] http::TraceContext filled(std::uint8_t seed) noexcept {
    http::TraceContext ctx{};
    ctx.trace_id.fill(seed);
    ctx.span_id.fill(seed);
    ctx.flags = http::kTraceFlagSampled;
    return ctx;
}

TEST(TraceScope, NestingRestoresTheOuterContext) {
    ASSERT_FALSE(http::current_trace().present());

    const http::TraceContext outer = filled(0x11);
    const http::TraceContext inner = filled(0x22);
    {
        const http::TraceScope outer_scope{outer};
        EXPECT_EQ(http::current_trace().trace_id, outer.trace_id);
        {
            const http::TraceScope inner_scope{inner};
            EXPECT_EQ(http::current_trace().trace_id, inner.trace_id);
        }
        EXPECT_EQ(http::current_trace().trace_id, outer.trace_id)
            << "an inner scope that restored to absent rather than to what it "
               "displaced would strand the rest of the outer request";
    }
    EXPECT_FALSE(http::current_trace().present());
}

TEST(TraceScope, AnExceptionThroughAScopeStillRestores) {
    // The one line of the destructor that is not exercised by a happy path, and
    // the one that matters most: the request that threw is exactly the request
    // whose id the next one on this thread would otherwise be logged under.
    ASSERT_FALSE(http::current_trace().present());
    try {
        const http::TraceScope scope{filled(0x33)};
        ASSERT_TRUE(http::current_trace().present());
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {  // NOLINT(bugprone-empty-catch)
    }
    EXPECT_FALSE(http::current_trace().present());
}

}  // namespace
}  // namespace anvil
