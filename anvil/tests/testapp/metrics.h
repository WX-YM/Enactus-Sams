#pragma once

// The reference application's metric table.
//
// anvil ships the registry, the cells, the snapshot and the OpenMetrics writer;
// WHAT gets counted, beyond anvil's own mechanisms, is this list
// (docs/01-seams.md §11). Compiled by every build of the test suite, so the
// worked example in the seam doc is a file that must keep compiling rather than
// a snippet that can rot.
//
// Nothing here is persisted, so — alone among the tables in this directory —
// there is no renumbering hazard and a metric may be removed. A dashboard is a
// consumer too, though, and it breaks silently when a series stops being
// emitted.

#include <array>
#include <cstdint>

#include "anvil/analytics/metric_spec.h"

namespace testapp {

namespace m = anvil::analytics;

// A label's VALUE SET is part of the declaration, not a runtime concern: it is
// what lets well_formed() multiply the value spaces and refuse a table above the
// cell ceiling, so the registry's memory cost is a compile-time constant
// (docs/17-analytics.md §6).
inline constexpr std::array<std::string_view, 3> kPlanValues{"free", "pro", "team"};
inline constexpr std::array<std::string_view, 2> kOutcomeValues{"succeeded", "failed"};

inline constexpr std::array<m::LabelSpec, 2> kCheckoutLabels{
    {{"plan", kPlanValues}, {"outcome", kOutcomeValues}}};
inline constexpr std::array<m::LabelSpec, 1> kPlanLabel{{{"plan", kPlanValues}}};

// Microseconds, as integers in the declared unit. A floating-point boundary
// makes two processes disagree about which bucket a value landed in, and a
// bucket count that differs by one between instances is indistinguishable from a
// real signal.
inline constexpr std::array<std::int64_t, 5> kLatencyBucketsUs{1000, 10000, 50000, 250000,
                                                               1000000};

// The writer appends `_total` to a counter, so no name here carries it.
inline constexpr std::array<m::MetricSpec, 3> kMetrics{{
    {"checkout_completed", "Checkouts that reached a terminal state", kCheckoutLabels, {},
     m::MetricKind::Counter, m::MetricUnit::None},

    {"carts_open", "Carts with at least one item, sampled at scrape", kPlanLabel, {},
     m::MetricKind::Gauge, m::MetricUnit::None},

    {"checkout_latency_microseconds", "Wall time from cart to receipt", {},
     kLatencyBucketsUs, m::MetricKind::Histogram, m::MetricUnit::Microseconds},
}};

// Half of this seam is a BUILD rather than a test — including the series count,
// which is the assertion that matters most: a table whose label space exceeds
// the ceiling fails to compile, which is the only place a cardinality bug is
// cheap.
static_assert(m::metric_table_is_well_formed(kMetrics),
              "an empty, duplicate or ungrammatical name; an empty help; a counter named "
              "_total; a name that disagrees with its unit; non-increasing buckets; a "
              "duplicate label; an anvil_ prefix; or a cell count past the ceiling");

// checkout_completed is 6 series of 1 cell, carts_open is 3 of 1, and
// checkout_latency is 1 series of 8 — five boundaries plus an overflow, a _sum
// and a _count. Stated here because "a histogram series is buckets + 3 cells" is
// the arithmetic a ceiling that counted series would miss.
static_assert(m::table_cell_count(kMetrics) == 6 + 3 + 8);

// Index into kMetrics. anvil's own metrics occupy the low indices of the
// concatenated table, so a call site resolves through application_metric()
// rather than adding an offset it would then have to maintain.
enum class Metric : std::size_t {
    CheckoutCompleted = 0,
    CartsOpen = 1,
    CheckoutLatency = 2,
};

// The label value spaces, as enums. This is the second of the three layers in
// docs/17 §6: observe() takes INDICES, and an enumerator is how a call site
// names one without a cast.
enum class Plan : std::uint8_t { Free = 0, Pro = 1, Team = 2 };
enum class Outcome : std::uint8_t { Succeeded = 0, Failed = 1 };

}  // namespace testapp
