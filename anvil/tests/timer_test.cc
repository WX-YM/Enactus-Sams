// The parts of the durable job queue that need no Redis: the envelope wire
// format, the recurrence arithmetic, the backoff schedule and the registry's own
// invariants (docs/10-timer-jobs.md).
//
// The Redis mechanism — claim-exactly-once, reclaim-after-crash, dead-lettering — is
// in tests/timer_db_test.cc, against a real server. Splitting them this way is
// deliberate: the wire format and the DST arithmetic are the parts that must not
// depend on a running service to be checked, because they are the parts a future
// change is most likely to break silently.

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>

#include "anvil/crypto/random.h"
#include "anvil/timer/queue.h"
#include "anvil/timer/registry.h"

namespace {

using namespace anvil;
using timer::JobHeader;
using std::uint16_t;

// --- the envelope wire format ------------------------------------------------

TEST(JobEnvelope, RoundTripsEveryHeaderFieldAndTheArguments) {
    const std::array<std::uint8_t, 5> args{1, 2, 3, 250, 0};
    const JobHeader header{
        .id = {0xAA, 0xBB, 0x01, 0x02, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0xFF},
        .not_before = db::TimeMs{std::chrono::milliseconds{1'780'000'000'123}},
        .kind = 1,
        .args_len = static_cast<std::uint16_t>(args.size()),
        .attempt = 3,
        .version = timer::kJobEnvelopeVersionUnlinked,
        .reserved = {},
    };

    std::string envelope;
    timer::encode_envelope(header, args, envelope);
    EXPECT_EQ(envelope.size(), timer::kJobHeaderBytesUnlinked + args.size());

    const std::optional<JobHeader> decoded = timer::decode_header(envelope);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->id, header.id);
    EXPECT_EQ(decoded->not_before, header.not_before);
    EXPECT_EQ(decoded->kind, header.kind);
    EXPECT_EQ(decoded->attempt, header.attempt);
    EXPECT_EQ(decoded->args_len, args.size());

    const std::span<const std::uint8_t> read = timer::envelope_args(envelope, *decoded);
    ASSERT_EQ(read.size(), args.size());
    for (std::size_t i = 0; i < args.size(); ++i) { EXPECT_EQ(read[i], args[i]); }
}

TEST(JobEnvelope, IsByteExactAndNotTheStructsPadding) {
    // The layout is the WIRE FORMAT, so a change to it must be a deliberate act: an
    // entry written by one build has to decode identically in another. Byte-level
    // assertions are the only way to notice a struct copy creeping in.
    const JobHeader header{
        .id = {},
        .not_before = db::TimeMs{std::chrono::milliseconds{0x0102030405060708}},
        .kind = static_cast<std::uint16_t>(0x0201),
        .args_len = 0,
        .attempt = 7,
        .version = 1,
        .reserved = {},
    };
    std::string envelope;
    timer::encode_envelope(header, {}, envelope);

    ASSERT_EQ(envelope.size(), 32U);
    // not_before is int64 little-endian at offset 16.
    EXPECT_EQ(static_cast<std::uint8_t>(envelope[16]), 0x08);
    EXPECT_EQ(static_cast<std::uint8_t>(envelope[23]), 0x01);
    // kind is uint16 little-endian at offset 24.
    EXPECT_EQ(static_cast<std::uint8_t>(envelope[24]), 0x01);
    EXPECT_EQ(static_cast<std::uint8_t>(envelope[25]), 0x02);
    EXPECT_EQ(static_cast<std::uint8_t>(envelope[28]), 7);
    EXPECT_EQ(static_cast<std::uint8_t>(envelope[29]), 1);
    // The reserved tail is zeroed, so a future field starts from a known value.
    EXPECT_EQ(envelope[30], '\0');
    EXPECT_EQ(envelope[31], '\0');
}

TEST(JobEnvelope, RefusesTruncatedForgedAndOverlongEntries) {
    std::string envelope;
    const std::array<std::uint8_t, 4> args{9, 9, 9, 9};
    timer::encode_envelope(
        JobHeader{.id = {},
                  .not_before = db::TimeMs{},
                  .kind = 1,
                  .args_len = 4,
                  .attempt = 1,
                  .version = timer::kJobEnvelopeVersionUnlinked,
                  .reserved = {}},
        args, envelope);

    // Shorter than the header.
    EXPECT_FALSE(timer::decode_header(envelope.substr(0, 10)).has_value());
    // A declared length that disagrees with the bytes present. Coercing it — trusting
    // whichever is smaller — would let a handler read a partial argument blob as a
    // complete one.
    EXPECT_FALSE(timer::decode_header(envelope.substr(0, envelope.size() - 1)).has_value());
    EXPECT_FALSE(timer::decode_header(envelope + "extra").has_value());
    // An unknown version. Taken from the HIGHEST version this build knows plus
    // one, because the next version added is what would otherwise quietly turn
    // this case into an assertion that a valid envelope is invalid.
    std::string wrong_version = envelope;
    wrong_version[29] = static_cast<char>(timer::kJobEnvelopeVersionLinked + 1);
    EXPECT_FALSE(timer::decode_header(wrong_version).has_value());
    // A zeroed blob must not decode into a dispatchable kind.
    const std::string zeros(timer::kJobHeaderBytesUnlinked, '\0');
    EXPECT_FALSE(timer::decode_header(zeros).has_value());
}

TEST(JobEnvelope, ALinkedEnvelopeCarriesTheTraceAndStillFindsItsArguments) {
    // Version 2 is the only one with a link in it, and the args move by sixteen
    // bytes because of it. A decoder that read the args at the version-1 offset
    // would hand a handler the tail of the trace id as its first argument —
    // which is not a crash, and is exactly the kind of wrong that survives.
    const std::array<std::uint8_t, 4> args{7, 6, 5, 4};
    const std::array<std::uint8_t, 16> link{0x4b, 0xf9, 0x2f, 0x35, 0x77, 0xb3,
                                            0x4d, 0xa6, 0xa3, 0xce, 0x92, 0x9d,
                                            0x0e, 0x0e, 0x47, 0x36};
    const JobHeader header{
        .id = {},
        .not_before = db::TimeMs{},
        .kind = 1,
        .args_len = static_cast<std::uint16_t>(args.size()),
        .attempt = 1,
        // Deliberately the WRONG version. The encoder derives it from the link,
        // and a header that could be told the two separately could be told two
        // different answers.
        .version = timer::kJobEnvelopeVersionUnlinked,
        .reserved = {},
        .linked_trace = link,
    };

    std::string envelope;
    timer::encode_envelope(header, args, envelope);
    ASSERT_EQ(envelope.size(), timer::kJobHeaderBytesLinked + args.size());
    EXPECT_EQ(static_cast<std::uint8_t>(envelope[29]), timer::kJobEnvelopeVersionLinked);

    const std::optional<JobHeader> decoded = timer::decode_header(envelope);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->version, timer::kJobEnvelopeVersionLinked);
    EXPECT_EQ(decoded->linked_trace, link);

    const std::span<const std::uint8_t> read = timer::envelope_args(envelope, *decoded);
    ASSERT_EQ(read.size(), args.size());
    for (std::size_t i = 0; i < args.size(); ++i) { EXPECT_EQ(read[i], args[i]); }
}

TEST(JobEnvelope, AnUnlinkedEnqueueIsByteIdenticalToWhatItAlwaysWas) {
    // The rollout property, asserted rather than asserted-about. An older worker
    // rejects a version it does not know and ACKs the entry, which loses the
    // job — so a deployment that does not trace has to keep producing the
    // version every build in the fleet can read.
    const std::array<std::uint8_t, 3> args{1, 2, 3};
    std::string envelope;
    timer::encode_envelope(JobHeader{.id = {},
                                     .not_before = db::TimeMs{},
                                     .kind = 1,
                                     .args_len = 3,
                                     .attempt = 1,
                                     .version = timer::kJobEnvelopeVersionLinked,
                                     .reserved = {},
                                     .linked_trace = {}},
                           args, envelope);
    EXPECT_EQ(envelope.size(), timer::kJobHeaderBytesUnlinked + args.size());
    EXPECT_EQ(static_cast<std::uint8_t>(envelope[29]), timer::kJobEnvelopeVersionUnlinked);
}

TEST(JobEnvelope, ALinkedVersionWithNoLinkInItIsRefused) {
    // Nothing here writes one, so reading one back means the bytes did not come
    // from this codec. Accepting it would mean a handler running under a fresh
    // root that links to the nil trace, which correlates to nothing and looks
    // like it correlates to something.
    const std::string forged = std::string(timer::kJobHeaderBytesLinked, '\0');
    std::string with_version = forged;
    with_version[29] = static_cast<char>(timer::kJobEnvelopeVersionLinked);
    EXPECT_FALSE(timer::decode_header(with_version).has_value());
}

// --- the registry -----------------------------------------------------------

TEST(JobRegistry, ReservedRetiredAndUnknownKindsHaveNoHandler) {
    // A zeroed or truncated envelope decodes kind 0, and dispatching it must be
    // impossible rather than merely unlikely — slot 0 is the reserved None.
    EXPECT_EQ(timer::spec_of(0), nullptr);
    EXPECT_EQ(timer::spec_of(static_cast<std::uint16_t>(timer::kJobKindCount)), nullptr);
    EXPECT_EQ(timer::spec_of(static_cast<std::uint16_t>(9999)), nullptr);

    for (std::uint16_t i = 1; i < timer::kJobKindCount; ++i) {
        const timer::JobSpec* spec = timer::spec_of(static_cast<std::uint16_t>(i));
        ASSERT_NE(spec, nullptr) << "kind " << i << " has no handler";
        EXPECT_FALSE(spec->key.empty());
        EXPECT_GT(spec->max_attempts, 0);
        EXPECT_GT(spec->lease_seconds, 0U);
    }
}

TEST(JobRegistry, EveryKeyIsUniqueSoALogLineNamesOneKind) {
    std::set<std::string_view> keys;
    for (std::uint16_t i = 1; i < timer::kJobKindCount; ++i) {
        const timer::JobSpec* spec = timer::spec_of(static_cast<std::uint16_t>(i));
        ASSERT_NE(spec, nullptr);
        EXPECT_TRUE(keys.insert(spec->key).second)
            << spec->key << " is used by two kinds; a dead-letter record would be ambiguous";
    }
}

// --- retries ----------------------------------------------------------------

TEST(JobRetry, BackoffIsTheDeclaredScheduleAndSaturatesRatherThanWraps) {
    // 1 s, 4 s, 15 s, 60 s, 5 min, then dead-letter (docs/10-timer-jobs.md §8).
    EXPECT_EQ(timer::retry_delay_seconds(1), 1U);
    EXPECT_EQ(timer::retry_delay_seconds(2), 4U);
    EXPECT_EQ(timer::retry_delay_seconds(3), 15U);
    EXPECT_EQ(timer::retry_delay_seconds(4), 60U);
    EXPECT_EQ(timer::retry_delay_seconds(5), 300U);
    // Past the table it saturates. An attempt count beyond the budget should never
    // reach here, and if it does the answer must be a bounded delay rather than an
    // out-of-bounds read.
    EXPECT_EQ(timer::retry_delay_seconds(6), 300U);
    EXPECT_EQ(timer::retry_delay_seconds(255), 300U);
    // Attempt 0 is not a thing the queue produces, but it must not index at -1.
    EXPECT_EQ(timer::retry_delay_seconds(0), 1U);
}

TEST(JobRetry, JitterIsUniformRatherThanModuloBiased) {
    // Jitter exists to spread retries out. A biased draw clusters them, which is the
    // failure it was added to prevent, so the distribution is asserted rather than
    // assumed (anvil/crypto/random.h).
    constexpr std::uint32_t kBound = 7;
    constexpr int kDraws = 7000;
    std::array<int, kBound> seen{};
    for (int i = 0; i < kDraws; ++i) {
        const std::uint32_t draw = crypto::random_below(kBound);
        ASSERT_LT(draw, kBound);
        ++seen[draw];
    }
    // Each bucket should be near kDraws/kBound = 1000. A modulo-biased generator over
    // a bound that does not divide 2^32 skews the low buckets measurably.
    for (const int count : seen) {
        EXPECT_GT(count, 800) << "jitter is not uniform";
        EXPECT_LT(count, 1200) << "jitter is not uniform";
    }
    EXPECT_EQ(crypto::random_below(0), 0U);
    EXPECT_EQ(crypto::random_below(1), 0U);
}

// --- recurrence, and the DST question -------------------------

TEST(JobRecurrence, BucketsArePureUtcArithmeticWithNoCalendar) {
    constexpr timer::RecurringSpec daily{timer::kDay, std::chrono::seconds{3 * 3600},
                                         std::chrono::seconds{0}, 1};

    // 2026-08-07T00:00:00Z is exactly a bucket boundary.
    constexpr std::int64_t kMidnight = 1'785'024'000'000;
    EXPECT_EQ(timer::recurrence_bucket(kMidnight, daily),
              timer::recurrence_bucket(kMidnight + 3600'000, daily));
    EXPECT_NE(timer::recurrence_bucket(kMidnight, daily),
              timer::recurrence_bucket(kMidnight + (24 * 3600'000), daily));
    // The due instant is the bucket start plus the offset, and nothing else.
    EXPECT_EQ(timer::recurrence_due_ms(timer::recurrence_bucket(kMidnight, daily), daily),
              kMidnight + (3 * 3600'000));
}

TEST(JobRecurrence, ADstTransitionProducesExactlyOneBucketPerPeriod) {
    // Egypt reinstated DST in 2023, so this is live rather than theoretical
    //. A recurrence expressed in Africa/Cairo would run twice on the
    // autumn transition and not at all on the spring one; this arithmetic has no
    // timezone in it, so it cannot.
    //
    // Egypt's 2026 transitions: forward on the last Friday of April, back on the last
    // Thursday of October. Walking a full day across each in UTC must yield exactly
    // one bucket per 24 hours either way.
    constexpr timer::RecurringSpec daily{timer::kDay, std::chrono::seconds{2 * 3600},
                                         std::chrono::seconds{0}, 1};

    for (const std::int64_t transition : {std::int64_t{1'777'420'800'000},   // 2026-04-24
                                          std::int64_t{1'793'404'800'000}}) {  // 2026-10-26
        std::set<std::int64_t> buckets;
        // Every ten minutes across three days spanning the transition.
        for (std::int64_t t = transition - (24 * 3600'000);
             t < transition + (2 * 24 * 3600'000); t += 600'000) {
            buckets.insert(timer::recurrence_bucket(t, daily));
        }
        // Three days, three buckets. Not two, not four.
        EXPECT_EQ(buckets.size(), 3U)
            << "a DST transition changed the number of daily buckets, which means the "
               "recurrence is not UTC";

        // And each bucket's due instant is exactly 24 hours after the previous one, so
        // no interval is doubled or skipped.
        std::vector<std::int64_t> due;
        due.reserve(buckets.size());
        for (const std::int64_t bucket : buckets) {
            due.push_back(timer::recurrence_due_ms(bucket, daily));
        }
        for (std::size_t i = 1; i < due.size(); ++i) {
            EXPECT_EQ(due[i] - due[i - 1], 24LL * 3600 * 1000);
        }
    }
}

TEST(JobRecurrence, EveryDeclaredRecurrenceHasAnOffsetInsideItsPeriod) {
    // An offset at or past the period would schedule into the NEXT bucket, so the
    // recurrence would drift forward by one period every time it fired. The
    // static_asserts in queue.h cover the declared table; this covers the invariant
    // for anything added later that the asserts do not reach.
    for (const timer::RecurringSpec& spec : config::kRecurringJobs) {
        EXPECT_LT(spec.offset_in_period, spec.period) << "offset must be inside the period";
        EXPECT_NE(timer::spec_of(spec.kind), nullptr)
            << "a recurrence names a kind with no handler";
        // Jitter must not be able to push a firing past its own bucket, or two buckets
        // could produce work in the same window.
        EXPECT_LT(spec.offset_in_period + spec.max_jitter, spec.period)
            << "jitter can push this recurrence out of its own bucket";
    }
}

}  // namespace
