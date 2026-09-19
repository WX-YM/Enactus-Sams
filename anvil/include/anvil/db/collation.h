#pragma once

// The one collation string, handed to both sides.
//
// MongoDB will not use an index built with a different collation than the query
// carries. The failure is a COLLSCAN — slowness, not an error — which means it is
// found under load rather than in review, and by then it is on every query
// against that collection.
//
// So there is exactly one place a collation comes from, and both the index
// definition and the query take it from here. Two independently written collation
// documents that happen to agree today are two that can stop agreeing.

#include <optional>
#include <string_view>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/stdx/string_view.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/core/locale.h"

namespace anvil::db {

// A locale whose LocaleSpec names no collation sorts by byte value, which is
// correct and faster wherever linguistic ordering is not needed. Asking for a
// collation document in that case would build one naming an empty locale, which
// the server rejects.
[[nodiscard]] inline bool needs_collation(Locale locale) noexcept {
    return !locale.collation().empty();
}

// Strength 2 — case- and diacritic-insensitive at the primary and secondary
// levels, which is what a human means by "sort these names". Strength 3 would
// order "a" before "A" and split what a reader sees as one group.
//
// Precondition: needs_collation(locale). Calling this for a binary-collated
// locale is a programming error, not a runtime condition.
[[nodiscard]] inline bsoncxx::document::value collation_for(Locale locale) {
    return bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("locale", bsoncxx::types::b_string{locale.collation()}),
        bsoncxx::builder::basic::kvp("strength", bsoncxx::types::b_int32{2}));
}

// The same document, for a caller holding the collation LOCALE rather than a
// Locale — an IndexSpec's `collation` is a view into the application's own
// LocaleSpec table, and the index catalogue has no Locale to hand.
//
// It exists so that there is still exactly ONE place a collation document is
// built. Two spellings of `{locale, strength: 2}` that agree today are two that
// can stop agreeing, and the failure is a COLLSCAN rather than an error.
//
// Precondition: `locale` is non-empty. The server rejects a collation naming an
// empty locale.
[[nodiscard]] inline bsoncxx::document::value collation_for(std::string_view locale) {
    return bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp(
            "locale", bsoncxx::types::b_string{
                          bsoncxx::stdx::string_view{locale.data(), locale.size()}}),
        bsoncxx::builder::basic::kvp("strength", bsoncxx::types::b_int32{2}));
}

// For a caller that may or may not need one, so the decision is made once rather
// than at each of the index definition and the query.
[[nodiscard]] inline std::optional<bsoncxx::document::value> optional_collation_for(
    Locale locale) {
    if (!needs_collation(locale)) { return std::nullopt; }
    return collation_for(locale);
}

}  // namespace anvil::db
