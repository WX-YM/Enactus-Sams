#pragma once

// Entries on the wire: the members a request carries beside the content, and
// the JSON an editor and a listing read.
//
// Content itself is bound by the section binder — sections::bind_data and
// sections::bind_images against `kind.shape` — so a field is validated by one
// piece of code wherever it appears. What is here is only what an entry has and
// a section does not.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/core/types.h"
#include "anvil/entries/document.h"
#include "anvil/entries/registry.h"
#include "anvil/input/json.h"
#include "anvil/sections/payload.h"

namespace anvil::entries {

using BindError = sections::BindError;

// A JSON string that is a well-formed slug. Absent is not an error here — the
// service decides whether the kind needs one.
[[nodiscard]] std::optional<BindError> bind_slug(const input::JsonValue* value, std::string& out);

// `{"pinned": true, "hidden": false}` into bits to set and bits to clear. A
// name the kind does not declare is refused with an EMPTY field name, for the
// reason docs/12-sections-cms.md §1 gives: a client must not choose what
// appears in a response or a log line.
[[nodiscard]] std::optional<BindError> bind_flags(const KindSpec& kind,
                                                  const input::JsonValue* value, FlagSet& set,
                                                  FlagSet& clear);

// A JSON array of entry ids, as reorder() takes them. At most `max` ids, so a
// request cannot make the server parse an unbounded list into a vector.
[[nodiscard]] std::optional<BindError> bind_order(const input::JsonValue* value, std::size_t max,
                                                  std::vector<Uuid>& out);

// How an image is linked in editor JSON. `src` is `base/id` followed by
// `suffix`, so an application whose media URLs carry a variant name (`/thumb`)
// says so here rather than rewriting the bytes afterwards.
struct ImageLinks final {
    std::string_view base;
    std::string_view suffix;
};

// The kind table for an editor: each kind's configuration, its flags with their
// labels, and its shape exactly as the section registry endpoint writes one
// (sections::append_shape_json), so one field control serves both editors.
[[nodiscard]] std::string serialize_kinds(std::span<const KindSpec> kinds);

// One entry as an editor reads it: placement, version, and each copy present
// with EVERY locale of every field side by side. A payload per locale would be
// two reads that could come from two versions.
void append_entry_json(std::string& out, const EntryDocument& entry, const ImageLinks& links);
[[nodiscard]] std::string serialize_entry(const EntryDocument& entry, const ImageLinks& links);

// `{"entries": [...], "next": "<id>" | null}`.
[[nodiscard]] std::string serialize_page(const EntryPage& page, const ImageLinks& links);

}  // namespace anvil::entries
