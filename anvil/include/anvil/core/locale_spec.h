#pragma once

// One locale, as an application declares it.
//
// This header deliberately has NO dependency on the application's configuration
// header, and that is its entire job: <anvil_app_config.h> includes THIS file to
// spell its table, and anvil/core/locale.h includes the application's header to
// read it. Without a type both sides can name independently, those two includes
// are a cycle.
//
// Nothing else belongs here. Every convenience built on top of a locale lives in
// anvil/core/locale.h, which is the side of the cycle that may see the table.

#include <cstddef>
#include <string_view>

namespace anvil {

struct LocaleSpec final {
    // The BSON subdocument key, the `?lang=` value, and the value on the wire —
    // one string for all three, because three spellings of one locale is three
    // places for them to disagree.
    std::string_view tag;          // 16

    // The ICU collation locale used when text in this locale is SORTED. Empty
    // means binary comparison, which is correct and fastest for a locale that
    // needs no linguistic ordering.
    //
    // The same string reaches the query and the index that must serve it
    // (anvil/db/collation.h). A sort carrying a collation its index was not
    // built with does not use that index, and the symptom is a COLLSCAN rather
    // than an error — so the two must come from one place, and this is it.
    std::string_view collation;    // 16

    // Right-to-left. Read by the renderers that emit a `dir` attribute; it is
    // not used to reorder anything server-side, because bidi reordering is the
    // client's job and doing it twice is worse than not doing it at all.
    bool             rtl;          //  1
};

// Members are ordered largest-alignment-first so there is no interior padding.
static_assert(sizeof(LocaleSpec) == 2 * sizeof(std::string_view) + sizeof(std::size_t),
              "LocaleSpec must not grow padding");

}  // namespace anvil
