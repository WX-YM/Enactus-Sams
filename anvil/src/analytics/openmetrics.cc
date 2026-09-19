#include "anvil/analytics/openmetrics.h"

#include <array>
#include <cstdint>

#include "anvil/analytics/metric_spec.h"

namespace anvil::analytics {
namespace {

// std::to_string is banned under src/analytics/ by tools/check-source-bans.sh —
// it is the shape of every cardinality bug that has ever shipped, because a
// label value built from a number is one change away from a label value built
// from a request. The ban is absolute rather than contextual, so the scrape
// writer formats its own digits.
void append_uint(std::string& out, std::uint64_t value) {
    std::array<char, 20> digits{};
    std::size_t next = digits.size();
    do {
        digits[--next] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    out.append(digits.data() + next, digits.size() - next);
}

void append_int(std::string& out, std::int64_t value) {
    if (value < 0) {
        out.push_back('-');
        // Negated through the unsigned domain: -INT64_MIN is undefined, and a
        // bucket boundary is application-supplied.
        append_uint(out, ~static_cast<std::uint64_t>(value) + 1U);
        return;
    }
    append_uint(out, static_cast<std::uint64_t>(value));
}

[[nodiscard]] std::string_view type_word(MetricKind kind) noexcept {
    switch (kind) {
        case MetricKind::Counter:   return "counter";
        case MetricKind::Gauge:     return "gauge";
        case MetricKind::Histogram: return "histogram";
    }
    return "unknown";
}

// The label value indices behind a series number. The inverse of the dot product
// counters.h computes on the increment path, and it exists only here — on the
// scrape path, once per series — which is the whole reason the increment path
// can be a multiply-add.
void decode_series(const MetricSpec& spec, std::size_t series,
                   std::array<std::size_t, kMaxLabels>& out) noexcept {
    std::size_t remaining = series;
    for (std::size_t i = 0; i < spec.labels.size(); ++i) {
        std::size_t stride = 1;
        for (std::size_t j = i + 1; j < spec.labels.size(); ++j) {
            stride *= spec.labels[j].values.size();
        }
        if (stride == 0) {
            out[i] = 0;
            continue;
        }
        out[i] = remaining / stride;
        remaining %= stride;
    }
}

// The declared labels, WITHOUT the closing brace and without `{` when there are
// none — so a caller adding `le` can open, extend and close it, and a caller
// with nothing to add closes it itself. Two functions writing the same brace
// would be two places for a comma to go missing.
void append_declared_labels(std::string& out, const MetricSpec& spec,
                            const std::array<std::size_t, kMaxLabels>& indices) {
    for (std::size_t i = 0; i < spec.labels.size(); ++i) {
        out.push_back(i == 0 ? '{' : ',');
        out.append(spec.labels[i].name);
        out.append("=\"");
        const std::span<const std::string_view> values = spec.labels[i].values;
        append_openmetrics_escaped(
            out, indices[i] < values.size() ? values[indices[i]] : std::string_view{});
        out.push_back('"');
    }
}

void append_plain_sample(std::string& out, std::string_view name, const MetricSpec& spec,
                         const std::array<std::size_t, kMaxLabels>& indices,
                         std::uint64_t value) {
    out.append(name);
    append_declared_labels(out, spec, indices);
    if (!spec.labels.empty()) { out.push_back('}'); }
    out.push_back(' ');
    append_uint(out, value);
    out.push_back('\n');
}

}  // namespace

void append_openmetrics_escaped(std::string& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '"':  out.append("\\\""); break;
            default:   out.push_back(c); break;
        }
    }
}

std::size_t estimate_openmetrics_bytes(const Snapshot& snapshot) noexcept {
    // Twenty digits is the widest a uint64 gets, and the rest is the family
    // header plus a generous allowance for the label set. Over-estimating costs
    // bytes that are freed at the end of the request; under-estimating costs a
    // reallocation and a copy of the whole body.
    std::size_t bytes = sizeof("# EOF\n");
    for (std::size_t i = 0; i < snapshot.metric_count(); ++i) {
        const MetricSpec& spec = snapshot.spec_at(i);
        bytes += (3 * (spec.name.size() + 16)) + (spec.help.size() * 2);
        std::size_t label_bytes = 0;
        for (const LabelSpec& label : spec.labels) {
            std::size_t widest = 0;
            for (const std::string_view value : label.values) {
                // Every byte of a value can double under escaping.
                widest = value.size() * 2 > widest ? value.size() * 2 : widest;
            }
            label_bytes += label.name.size() + widest + 4;
        }
        const std::size_t samples = series_count(spec) * cells_per_series(spec);
        bytes += samples * (spec.name.size() + 8 + label_bytes + 32);
    }
    return bytes;
}

void append_openmetrics(std::string& out, const Snapshot& snapshot) {
    // Built once per METRIC, not once per sample.
    // `anvil_mongo_pool_wait_microseconds_bucket` is well past the small-string
    // buffer, so assembling it inside the series loop would be one allocation
    // per series on a path whose whole claim is that it makes none.
    std::string suffixed;
    std::size_t widest_name = 0;
    for (std::size_t i = 0; i < snapshot.metric_count(); ++i) {
        const std::size_t name = snapshot.spec_at(i).name.size();
        widest_name = name > widest_name ? name : widest_name;
    }
    suffixed.reserve(widest_name + sizeof("_bucket"));

    std::size_t next = 0;
    for (std::size_t metric = 0; metric < snapshot.metric_count(); ++metric) {
        const MetricSpec& spec = snapshot.spec_at(metric);

        // The METRIC FAMILY name, which for a counter is the name WITHOUT
        // `_total`: the family is `foo` and the sample is `foo_total`. Emitting
        // the family as `foo_total` produces a `foo_total_total` sample, which
        // every collector reads as a different series from the one the dashboard
        // names.
        out.append("# TYPE ");
        out.append(spec.name);
        out.push_back(' ');
        out.append(type_word(spec.kind));
        out.push_back('\n');

        if (spec.unit != MetricUnit::None) {
            out.append("# UNIT ");
            out.append(spec.name);
            out.push_back(' ');
            out.append(unit_suffix(spec.unit));
            out.push_back('\n');
        }

        out.append("# HELP ");
        out.append(spec.name);
        out.push_back(' ');
        append_openmetrics_escaped(out, spec.help);
        out.push_back('\n');

        const std::size_t series = series_count(spec);
        const std::size_t slots = cells_per_series(spec);
        const std::span<const std::uint64_t> values = snapshot.values();

        for (std::size_t s = 0; s < series; ++s) {
            std::array<std::size_t, kMaxLabels> indices{};
            decode_series(spec, s, indices);

            switch (spec.kind) {
                case MetricKind::Histogram: {
                    // CUMULATIVE, computed here by a running total over cells
                    // that are already being read. The increment path adds to
                    // ONE bucket, which is what keeps an observation to three
                    // adds rather than twelve (docs/17-analytics.md §5).
                    std::uint64_t running = 0;
                    suffixed.assign(spec.name);
                    suffixed.append("_bucket");
                    for (std::size_t b = 0; b <= spec.buckets.size(); ++b) {
                        running += values[next + b];
                        out.append(suffixed);
                        append_declared_labels(out, spec, indices);
                        out.push_back(spec.labels.empty() ? '{' : ',');
                        out.append("le=\"");
                        if (b == spec.buckets.size()) {
                            out.append("+Inf");
                        } else {
                            append_int(out, spec.buckets[b]);
                        }
                        out.append("\"} ");
                        append_uint(out, running);
                        out.push_back('\n');
                    }

                    suffixed.assign(spec.name);
                    suffixed.append("_sum");
                    append_plain_sample(out, suffixed, spec, indices,
                                        values[next + spec.buckets.size() + 1]);

                    suffixed.assign(spec.name);
                    suffixed.append("_count");
                    append_plain_sample(out, suffixed, spec, indices,
                                        values[next + spec.buckets.size() + 2]);
                    break;
                }
                case MetricKind::Counter: {
                    suffixed.assign(spec.name);
                    suffixed.append("_total");
                    append_plain_sample(out, suffixed, spec, indices, values[next]);
                    break;
                }
                case MetricKind::Gauge:
                    append_plain_sample(out, spec.name, spec, indices, values[next]);
                    break;
            }
            next += slots;
        }
    }
    out.append("# EOF\n");
}

}  // namespace anvil::analytics
