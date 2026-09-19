// versioned-write-exempt: `insert_if_absent` creates a document that has no
// previous version at all, and uses insert_one rather than an update. Every
// ordinary staff write goes through `update`, which delegates to
// repo::update_versioned (anvil/db/versioned.h).

#include "anvil/sections/repository.h"

#include <cstddef>
#include <utility>

#include <bsoncxx/array/value.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/concatenate.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/options/find.hpp>

#include "anvil/db/versioned.h"
#include "anvil/sections/codec.h"

namespace anvil::sections {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

namespace fields = section_fields;

// MongoDB rejects a duplicate `_id` with error code 11000. Bootstrap depends on
// distinguishing "another instance got there first" — which is success — from a
// real failure, and the driver reports both as an operation_exception.
constexpr int kDuplicateKeyCode = 11000;

// The `_id` VALUE — `{k, s}` — built in exactly one place because field order
// inside an embedded document is significant to the server: `{k, s}` and
// `{s, k}` are different keys, and a second spelling of this would be a lookup
// that silently matches nothing.
//
// Separate from identity_of because a `$in` over the primary key wants the value
// without the field wrapped around it.
[[nodiscard]] bsoncxx::document::value id_value_of(std::string_view key, SectionState state) {
    return make_document(kvp(db::codec::key_of(fields::kIdKey),
                             bsoncxx::types::b_string{db::codec::key_of(key)}),
                         kvp(db::codec::key_of(fields::kIdState),
                             bsoncxx::types::b_int32{static_cast<std::int32_t>(state)}));
}

// The compound primary key as a FILTER.
[[nodiscard]] bsoncxx::document::value identity_of(std::string_view key, SectionState state) {
    return make_document(
        kvp(db::codec::key_of(fields::kId), id_value_of(key, state).view()));
}

// The `k` half of a returned `_id`, resolved against the registry. A document
// whose key is not in the table is DISCARDED rather than reported: the registry
// is the allow-list on the way out as well as on the way in, and a section
// removed by a deploy is one whose slots nothing renders any more.
[[nodiscard]] const SectionSpec* spec_of(const bsoncxx::document::view& doc,
                                         std::span<const SectionSpec> registry) noexcept {
    const bsoncxx::document::element id = doc[db::codec::key_of(fields::kId)];
    if (!id || id.type() != bsoncxx::type::k_document) { return nullptr; }
    const Result<std::string_view> key =
        db::codec::read_text(id.get_document().value, fields::kIdKey);
    if (!key) { return nullptr; }
    return find_section(registry, key.value());
}

[[nodiscard]] bsoncxx::document::value content_document(const SectionSpec& spec,
                                                        const SectionDocument& content,
                                                        const Uuid& actor) {
    bsoncxx::builder::basic::document doc;
    codec::append_content(doc, spec, content.content);
    doc.append(kvp(db::codec::key_of(fields::kEtag), db::codec::bytes_bin(content.etag)));
    db::codec::append_uuid(doc, fields::kUpdatedBy, actor);
    return doc.extract();
}

[[nodiscard]] Result<SectionDocument> decode(const bsoncxx::document::view& doc,
                                             const SectionSpec& spec) {
    Result<SectionContent> content = codec::decode_content(doc, spec);
    if (!content) { return content.error(); }

    SectionDocument out{};
    out.content = std::move(content).value();

    const Status etag = db::codec::read_bytes(doc, fields::kEtag, out.etag);
    if (!etag) { return etag.error(); }
    const Result<db::TimeMs> updated_at = db::codec::read_time(doc, fields::kUpdatedAt);
    if (!updated_at) { return updated_at.error(); }
    const Result<Uuid> updated_by = db::codec::read_uuid(doc, fields::kUpdatedBy);
    if (!updated_by) { return updated_by.error(); }
    const Result<std::int64_t> version = repo::document_version(doc);
    if (!version) { return version.error(); }

    out.updated_at = updated_at.value();
    out.updated_by = updated_by.value();
    out.version = version.value();
    return out;
}

}  // namespace

Result<std::optional<SectionDocument>> SectionRepository::find(mongocxx::client& client,
                                                               const SectionSpec& spec,
                                                               SectionState state) const {
    return repo::guarded([&]() -> Result<std::optional<SectionDocument>> {
        mongocxx::collection collection = bind(client);
        const auto found = collection.find_one(identity_of(spec.key, state).view());
        if (!found) { return std::optional<SectionDocument>{}; }
        Result<SectionDocument> decoded = decode(found->view(), spec);
        if (!decoded) { return decoded.error(); }
        return std::optional<SectionDocument>{std::move(decoded).value()};
    });
}

Result<std::size_t> SectionRepository::count_unbound_image_slots(
    mongocxx::client& client, std::span<const SectionSpec> registry, SectionState state) const {
    // Every declared slot starts out counted, and a slot found bound is
    // SUBTRACTED. That is what makes a section whose document never arrived — one
    // an insert lost, or one added to the registry since the last boot — count
    // all of its slots without a second pass to find out which keys came back.
    std::size_t unbound = 0;
    std::size_t sections_asked = 0;
    for (const SectionSpec& spec : registry) {
        if (spec.images.empty()) { continue; }
        unbound += spec.images.size();
        ++sections_asked;
    }
    // A registry that declares no image anywhere is already answered, and a
    // `$in` over nothing is a round trip to learn nothing.
    if (sections_asked == 0) { return std::size_t{0}; }

    return repo::guarded([&]() -> Result<std::size_t> {
        bsoncxx::builder::basic::array wanted;
        for (const SectionSpec& spec : registry) {
            if (spec.images.empty()) { continue; }
            wanted.append(id_value_of(spec.key, state).view());
        }
        const bsoncxx::array::value identities = wanted.extract();

        mongocxx::options::find options{};
        // `media` only. The values are a few hundred bytes each and none of them
        // is being read — returning whole documents to count map keys is network
        // and BSON decode spent on nothing (ENGINEERING_RULES.md §7). `_id` arrives whatever
        // the projection says, which is what maps each document back to its spec.
        options.projection(make_document(kvp(db::codec::key_of(codec::kMediaField), 1)));
        // Bounded by what was asked for. The `_id` is unique, so this cannot be
        // exceeded — which is the reason to state it rather than the reason to
        // leave it out.
        options.limit(static_cast<std::int64_t>(sections_asked));

        const bsoncxx::document::value filter = make_document(
            kvp(db::codec::key_of(fields::kId),
                make_document(kvp("$in", bsoncxx::types::b_array{identities.view()}))));

        std::size_t bound = 0;
        mongocxx::collection collection = bind(client);
        for (const bsoncxx::document::view doc : collection.find(filter.view(), options)) {
            const SectionSpec* spec = spec_of(doc, registry);
            if (spec == nullptr) { continue; }
            const bsoncxx::document::element media =
                doc[db::codec::key_of(codec::kMediaField)];
            if (!media || media.type() != bsoncxx::type::k_document) { continue; }
            const bsoncxx::document::view media_view = media.get_document().value;
            for (const ImageSpec& image : spec->images) {
                // BINARY, not merely present. A slot holding something a renderer
                // cannot resolve to a media id is the same gap on the page as a
                // slot holding nothing, and counting presence would report it as
                // bound — which is the class of answer this whole function
                // exists to stop giving.
                const bsoncxx::document::element slot =
                    media_view[db::codec::key_of(image.slot)];
                if (slot && slot.type() == bsoncxx::type::k_binary) { ++bound; }
            }
        }
        // Defensive rather than expected: `bound` is accumulated one declared
        // slot at a time out of the same registry `unbound` was summed from, so
        // it cannot exceed it. Saturating rather than wrapping, because an
        // unsigned wrap here would report a gigantic number of gaps and send an
        // operator looking for content that is fine (ENGINEERING_RULES.md §5).
        return bound >= unbound ? std::size_t{0} : unbound - bound;
    });
}

Result<bool> SectionRepository::insert_if_absent(mongocxx::client& client,
                                                 const SectionSpec& spec, SectionState state,
                                                 const SectionDocument& content,
                                                 const Uuid& actor) const {
    try {
        mongocxx::collection collection = bind(client);
        bsoncxx::builder::basic::document doc;
        doc.append(bsoncxx::builder::concatenate(identity_of(spec.key, state).view()));
        doc.append(bsoncxx::builder::concatenate(content_document(spec, content, actor).view()));
        db::codec::append_time(doc, fields::kUpdatedAt, db::now_ms());
        repo::append_initial_version(doc);

        collection.insert_one(doc.view());
        return true;
    } catch (const mongocxx::operation_exception& e) {
        // Another instance inserted it first, or it was already there. Both are
        // the intended outcome of an idempotent bootstrap, and neither is an
        // error the caller should have to tell apart from a real failure.
        if (e.code().value() == kDuplicateKeyCode) { return false; }
        return repo::translate(e);
    } catch (const mongocxx::exception& e) {
        return repo::translate(e);
    } catch (...) {
        return fail(ErrorCode::Internal);
    }
}

Result<std::int64_t> SectionRepository::update(mongocxx::client& client,
                                               mongocxx::client_session& session,
                                               const SectionSpec& spec, SectionState state,
                                               std::int64_t expected_version,
                                               const SectionDocument& content,
                                               const Uuid& actor) const {
    mongocxx::collection collection = bind(client);
    const bsoncxx::document::value identity = identity_of(spec.key, state);
    const bsoncxx::document::value fields_to_set = content_document(spec, content, actor);
    return repo::update_versioned(collection, session, identity.view(), expected_version,
                                  fields_to_set.view());
}

}  // namespace anvil::sections
