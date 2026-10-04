#pragma once

// The event seam: what an application needs to declare the moments of its own
// product that are worth recording, with no dependency on its configuration
// header.
//
// anvil ships the buffer, the shedding policy, sessionisation, the rollup
// arithmetic and the collections' shape. WHAT is worth recording is a list of
// the application's own (docs/01-seams.md §12).
//
// --- what is STORED here, and can therefore never be renumbered -------------
//
// `EventCode` and a dimension's VALUE INDEX. They join the locale index, the
// permission bit, the namespace index, the field-type code, the audit action
// value and the notification template id on the list of integers this library
// can never renumber: a rollup computed months ago carries them, and a
// renumbering silently reinterprets every row that already exists.
//
// --- why `cls` is a column and not a label ---------------------------------
//
// EventClass plays exactly the role AuditClass plays in docs/01-seams.md §9, and
// for exactly the same reason: it decides WHAT MAY BE LOST. anvil cannot infer
// it — only the application knows which of its moments is the one the whole
// funnel is about — and getting it backwards means a flood of page views evicts
// the one signup it was hiding.
//
// --- why a dimension is a closed set ---------------------------------------
//
// The same reasoning as a metric label (anvil/analytics/metric_spec.h §6 of
// docs/17): an event carrying a free-text dimension is a collection whose index
// cardinality is chosen by a visitor. A dimension's values are declared beside
// the event, and what the row stores is the INDEX.
//
// --- the one dimension that is NOT a closed set -----------------------------
//
// An `Entity` dimension names an id the application mints elsewhere — an entry,
// a catalogue row — rather than a value this table can enumerate: the whole
// point is that a new project must not need a deploy to be counted separately
// (docs/17-analytics.md §19). It keeps every other property of a dimension
// (declared beside the event, checked at the door) except the closed value
// list, which is why it is a KIND of dimension rather than a different thing
// bolted on beside one. Its cardinality is bounded a different way — the
// application's admission check at ingest (anvil/analytics/ingest.h) — and its
// stored form is the 16-byte id itself, never an index into anything, because
// there is no table to index into.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "anvil/core/ct_text.h"

namespace anvil::analytics {

// STORED as int32 on every event row and every rollup. APPEND ONLY.
using EventCode = std::int32_t;

// At most four dimensions. Not a storage limit — four indices are four bytes —
// but a cardinality one: the dimensions multiply in the rollup, and a fifth is
// almost always the point where somebody reaches for a request field.
inline constexpr std::size_t kMaxDimensions = 4;

// A dimension's value index is ONE BYTE on the wire and in memory, so a closed
// set may hold at most 255 values. 0xFF is the absent slot, which is what an
// event carries for a dimension its spec does not declare.
inline constexpr std::size_t kMaxDimensionValues = 255;
inline constexpr std::uint8_t kNoDimensionValue = 0xFF;

// The load-bearing column. A Conversion is never dropped while a Behaviour row
// is buffered.
enum class EventClass : std::uint8_t {
    // Compressible. A refresh storm is one row with a repeat count, and a count
    // IS the rate.
    Behaviour = 0,
    // The moment the funnel is about. Never dropped while anything compressible
    // is still held, and refused only when the buffer holds nothing else — which
    // is a state worth alerting on rather than a load signal.
    Conversion = 1,
};

// The name the class goes on the wire under, for a client generated from the
// descriptor. The stored column is the CODE, so a name here is paid once per
// generated client rather than once per row.
[[nodiscard]] constexpr std::string_view event_class_name(EventClass cls) noexcept {
    switch (cls) {
        case EventClass::Behaviour:  return "behaviour";
        case EventClass::Conversion: return "conversion";
    }
    return "behaviour";
}

// `Enum` is the closed set every dimension used to be; `Entity` is the one
// described above. Kept a two-value enum rather than a bool so a third kind, if
// one is ever needed, is an addition rather than an inversion of this one.
enum class DimensionKind : std::uint8_t {
    Enum = 0,
    Entity = 1,
};

struct DimensionSpec final {
    std::string_view                  name;    // 16
    // ENUM ONLY: the entire closed value set. Empty for an Entity dimension —
    // `values` and `kind` disagreeing would be two ways to ask the same
    // question with two different answers, so `well_formed()` refuses it.
    std::span<const std::string_view> values;  // 16
    DimensionKind                     kind = DimensionKind::Enum;  // 1 (+7 pad)
};

static_assert(sizeof(DimensionSpec) == 40, "DimensionSpec must not grow padding");

// Ordered largest-alignment-first so the table packs.
struct EventSpec final {
    // For queries and for an operator reading a rollup. Never translated and
    // never parsed from a request.
    std::string_view               name;              // 16
    std::span<const DimensionSpec> dimensions;        // 16
    EventCode                      code;              //  4
    EventClass                     cls;               //  1
    // Refused at the door, not filtered later. An event declaring this is never
    // buffered and never written without consent: "recorded and then excluded
    // from queries" is a policy one forgotten $match away from being no policy
    // at all.
    bool                           requires_consent;  //  1
};

static_assert(sizeof(EventSpec) == 40, "EventSpec must not grow padding");

// --- table conformance ------------------------------------------------------

[[nodiscard]] constexpr bool event_table_is_well_formed(
    std::span<const EventSpec> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        const EventSpec& spec = table[i];
        if (!ct::is_non_empty_utf8(spec.name)) { return false; }
        // Negative codes are refused so a stored value can never collide with a
        // sentinel, and so a decoder reading a rollup can bound what it sees.
        if (spec.code < 0) { return false; }
        if (spec.dimensions.size() > kMaxDimensions) { return false; }
        std::size_t entity_dimensions = 0;
        for (std::size_t d = 0; d < spec.dimensions.size(); ++d) {
            const DimensionSpec& dimension = spec.dimensions[d];
            if (!ct::is_non_empty_utf8(dimension.name)) { return false; }
            if (dimension.kind == DimensionKind::Entity) {
                // No closed set to check — an Entity dimension's `values` must
                // be empty, or the table is stating the space two ways and
                // disagreeing with itself.
                if (!dimension.values.empty()) { return false; }
                ++entity_dimensions;
            } else {
                // A dimension with no values contributes nothing and makes
                // every offer() against it silently drop the dimension.
                if (dimension.values.empty()) { return false; }
                if (dimension.values.size() > kMaxDimensionValues) { return false; }
                for (std::size_t v = 0; v < dimension.values.size(); ++v) {
                    if (!ct::is_non_empty_utf8(dimension.values[v])) { return false; }
                    for (std::size_t w = 0; w < v; ++w) {
                        if (dimension.values[w] == dimension.values[v]) { return false; }
                    }
                }
            }
            for (std::size_t e = 0; e < d; ++e) {
                if (spec.dimensions[e].name == dimension.name) { return false; }
            }
        }
        // Event carries exactly one entity slot (anvil/analytics/event.h) — not
        // an array like the enum dimensions — so a second Entity dimension on
        // the same event would have nowhere of its own to be stored.
        if (entity_dimensions > 1) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            // A duplicate code makes the second entry unreachable and makes
            // every row already written under it ambiguous.
            if (table[j].code == spec.code) { return false; }
            if (table[j].name == spec.name) { return false; }
        }
    }
    return true;
}

// Codes are exactly 0..N-1 in table order, so a lookup is one bounds check and
// one index rather than a scan per event — and an event is offered on a request
// path.
[[nodiscard]] constexpr bool events_are_dense_from_zero(
    std::span<const EventSpec> table) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].code != static_cast<EventCode>(i)) { return false; }
    }
    return true;
}

// --- lookup -----------------------------------------------------------------

// nullptr for a code this build does not declare — a real state during a rolling
// deploy, when a row written by a newer process names an event an older one has
// never heard of. Refusing to interpret it is the correct direction to fail.
[[nodiscard]] constexpr const EventSpec* event_spec(std::span<const EventSpec> table,
                                                    EventCode code) noexcept {
    if (code >= 0) {
        const auto index = static_cast<std::size_t>(code);
        if (index < table.size() && table[index].code == code) { return &table[index]; }
    }
    for (const EventSpec& spec : table) {
        if (spec.code == code) { return &spec; }
    }
    return nullptr;
}

[[nodiscard]] constexpr const EventSpec* event_by_name(std::span<const EventSpec> table,
                                                       std::string_view name) noexcept {
    for (const EventSpec& spec : table) {
        if (spec.name == name) { return &spec; }
    }
    return nullptr;
}

// nullptr when this event declares no Entity-kind dimension — the ordinary
// case, since most events never will. `well_formed()` guarantees there is at
// most one, so the first found is the only one there is.
[[nodiscard]] constexpr const DimensionSpec* entity_dimension_of(const EventSpec& spec) noexcept {
    for (const DimensionSpec& dimension : spec.dimensions) {
        if (dimension.kind == DimensionKind::Entity) { return &dimension; }
    }
    return nullptr;
}

[[nodiscard]] constexpr EventClass event_class_of(std::span<const EventSpec> table,
                                                  EventCode code) noexcept {
    const EventSpec* spec = event_spec(table, code);
    // An UNKNOWN code is Behaviour, which is the droppable class. A row a build
    // cannot interpret must not be able to evict one it can.
    return spec == nullptr ? EventClass::Behaviour : spec->cls;
}

[[nodiscard]] constexpr bool event_requires_consent(std::span<const EventSpec> table,
                                                    EventCode code) noexcept {
    const EventSpec* spec = event_spec(table, code);
    // An unknown code is treated as REQUIRING consent. The two unknown-code
    // defaults point in opposite directions deliberately: both fail towards
    // recording less.
    return spec == nullptr || spec->requires_consent;
}

}  // namespace anvil::analytics
