// The reference application's step bodies.
//
// Both are re-runnable, which is not incidental: a lease can expire against a
// process that is alive but stalled, so two runners can overlap, and resumption
// re-applies the batch that was in flight (docs/18-data-migrations.md §5). Each
// writes what the document SHOULD BE, derived from what it read, never a delta
// from it — and the seam is what makes that structural: StepContext accumulates
// fields and the RUNNER wraps them in $set, so a step has no way to spell $inc.

#include "migrations.h"

#include <string>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_array.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/stdx/string_view.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/value.hpp>

#include "anvil/db/codec.h"
#include "anvil/identity/user_fields.h"

namespace testapp {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

namespace uf = anvil::identity::fields;

// A document with no `_id` is not a document MongoDB returned. Treating it as a
// failure rather than skipping it is deliberate: the alternative is a step that
// reports success having silently declined to migrate part of the collection.
[[nodiscard]] bool has_id(const bsoncxx::document::view& doc) noexcept {
    return static_cast<bool>(doc["_id"]);
}

}  // namespace

// The display name every account should carry, derived from the account itself.
//
// Unconditional rather than "only when absent": a write that depends on what it
// found is a write whose result depends on how far the last run got, and this
// one produces the same document applied once, twice or resumed from the middle.
mig::StepOutcome backfill_display_name(
    mig::StepContext& context, std::span<const bsoncxx::document::view> batch) noexcept {
    try {
        for (const bsoncxx::document::view& doc : batch) {
            if (!has_id(doc)) { return mig::StepOutcome::Failed; }

            const bsoncxx::document::element username = doc[anvil::db::codec::key_of(
                uf::kUsernameDisplay)];
            const bsoncxx::document::element email = doc[anvil::db::codec::key_of(
                uf::kEmailDisplay)];

            bsoncxx::stdx::string_view display;
            if (username && username.type() == bsoncxx::type::k_string) {
                display = username.get_string().value;
            } else if (email && email.type() == bsoncxx::type::k_string) {
                display = email.get_string().value;
            } else {
                // Neither identity is stored as text. That is a document this
                // step cannot describe, and guessing produces a display name
                // derived from nothing.
                return mig::StepOutcome::Failed;
            }

            context.set(bsoncxx::types::bson_value::value{doc["_id"].get_value()},
                        make_document(kvp("display_name", bsoncxx::types::b_string{display})));
        }
        return mig::StepOutcome::Ok;
    } catch (...) {
        // A step is noexcept because it runs inside a loop that holds a lease:
        // an exception escaping it would leave the lease held by a process that
        // is no longer running the step.
        return mig::StepOutcome::Failed;
    }
}

// The legacy top-level `title`, which moved into the section's content document.
//
// Asks for a write only where the field is actually present, which is the other
// shape a step has: a batch that needs no change accumulates nothing, and the
// runner then issues no write at all rather than one that matches and does
// nothing.
mig::StepOutcome drop_legacy_section_title(
    mig::StepContext& context, std::span<const bsoncxx::document::view> batch) noexcept {
    try {
        for (const bsoncxx::document::view& doc : batch) {
            if (!has_id(doc)) { return mig::StepOutcome::Failed; }
            if (!doc["title"]) { continue; }

            context.unset(bsoncxx::types::bson_value::value{doc["_id"].get_value()},
                          make_document(kvp("title", bsoncxx::types::b_string{""})));
        }
        return mig::StepOutcome::Ok;
    } catch (...) {
        return mig::StepOutcome::Failed;
    }
}

}  // namespace testapp

// --- the two validators -----------------------------------------------------
//
// $jsonSchema on a collection is a net and never the validation: request data is
// checked at the edge, in input/, with typed errors and a field name the caller
// can act on (docs/06-input-validation.md). These describe the SHAPE anvil's own
// writers produce, so what they catch is the write that did not come from one.
//
// Each names only the fields that are always present. A validator listing an
// optional field as required is a validator that rejects the ordinary row, and
// it does so at the moment the collection is busiest.

namespace testapp {

bsoncxx::document::value draft_shape() {
    return make_document(kvp("$jsonSchema", [](bsoncxx::builder::basic::sub_document schema) {
        schema.append(kvp("bsonType", "object"));
        schema.append(kvp("required", [](bsoncxx::builder::basic::sub_array required) {
            required.append("_id");
            required.append("expires_at");
        }));
        schema.append(kvp("properties", [](bsoncxx::builder::basic::sub_document props) {
            props.append(kvp("expires_at", make_document(kvp("bsonType", "date"))));
        }));
    }));
}

bsoncxx::document::value audit_row_shape() {
    return make_document(kvp("$jsonSchema", [](bsoncxx::builder::basic::sub_document schema) {
        schema.append(kvp("bsonType", "object"));
        // The four fields every row carries. `actor` and `sub` are deliberately
        // absent from this list: a denial under flood has neither, and those are
        // the rows this collection holds most of.
        schema.append(kvp("required", [](bsoncxx::builder::basic::sub_array required) {
            required.append("_id");
            required.append("at");
            required.append("act");
            required.append("ok");
        }));
        schema.append(kvp("properties", [](bsoncxx::builder::basic::sub_document props) {
            props.append(kvp("at", make_document(kvp("bsonType", "date"))));
            props.append(kvp("act", make_document(kvp("bsonType", "int"))));
            props.append(kvp("ok", make_document(kvp("bsonType", "bool"))));
        }));
    }));
}

}  // namespace testapp
