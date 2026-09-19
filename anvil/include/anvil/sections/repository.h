#pragma once

// The `sections` collection.
//
// TWO documents per key — `{k, s: 0}` published and `{k, s: 1}` draft — never
// one document holding both. A public read then cannot leak an unpublished
// string through a forgotten projection, because the unpublished string is not
// in the document it read.
//
// The compound `_id` gives the uniqueness constraint and the primary-key index
// for free: the collection carries no secondary index at all, and no extra
// 16-byte `_id` per document.
//
// --- what this layer decides, and what it does not --------------------------
//
// It encodes and decodes. It does not know what a valid section is: which keys
// exist, which are localised, and what the bounds are all come from the
// compile-time registry, and the service applies them. What this file DOES
// enforce is that a stored field with no registry entry is dropped on the way
// out — a field removed by a deploy must stop being served, and it must not
// reappear in a merged write.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "anvil/crypto/fast_hash.h"
#include "anvil/db/codec.h"
#include "anvil/db/repository.h"
#include "anvil/sections/content.h"
#include "anvil/sections/registry.h"

namespace anvil::sections {

// The field names are PUBLISHED for the same reason the user schema's are: the
// index catalogue is the application's, and it cannot name a column it has to
// guess (anvil/identity/user_fields.h).
namespace section_fields {
inline constexpr std::string_view kId = "_id";
// Sub-keys of the compound `_id`. Field ORDER inside an embedded-document `_id`
// is significant to MongoDB — `{k, s}` and `{s, k}` are different keys — so the
// identity is built in exactly one place.
inline constexpr std::string_view kIdKey = "k";
inline constexpr std::string_view kIdState = "s";
inline constexpr std::string_view kEtag = "etag";
inline constexpr std::string_view kUpdatedAt = "updated_at";
inline constexpr std::string_view kUpdatedBy = "updated_by";
}  // namespace section_fields

// The values plus the bookkeeping around them. The values themselves are
// SectionContent, shared with every layer above, so no layer converts between
// two near-identical shapes.
//
// A stored BSON type that disagrees with the registry is an integrity fault and
// is reported as one — decoding never coerces (anvil/db/codec.h).
struct SectionDocument final {
    SectionContent     content;
    crypto::FastDigest etag;
    db::TimeMs         updated_at;
    Uuid               updated_by;
    std::int64_t       version;
};

class SectionRepository final : public repo::RepositoryBase {
public:
    SectionRepository(std::string database, std::string_view collection) noexcept
        : RepositoryBase{std::move(database), collection} {}

    // nullopt when the key has never been written in that state. `spec` drives
    // decoding, so a field the registry no longer declares is skipped rather
    // than returned.
    [[nodiscard]] Result<std::optional<SectionDocument>> find(mongocxx::client& client,
                                                              const SectionSpec& spec,
                                                              SectionState state) const;

    // How many DECLARED image slots have nothing usable bound to them in
    // storage, across `registry`.
    //
    // The question BootstrapReport::images_missing cannot answer. That one
    // counts THIS BOOT'S resolver failures, and bootstrap binds an image only
    // into a section it CREATES — so a database seeded before the defaults tree
    // existed reports zero failures on every later boot while every slot stays
    // empty. This is taken from the stored documents instead, so it is the same
    // answer whatever route the deployment took to reach the state it is in.
    //
    // Counted per SLOT rather than per section: a section declaring three slots
    // with one bound is two gaps on the page, and a count of sections whose
    // `media` map is entirely empty calls that zero — which is the same shape of
    // wrong answer this exists to replace.
    //
    // One `$in` over the `_id` index with a `media`-only projection: N sections
    // is one round trip and N index seeks rather than N queries, and nothing
    // comes back but the map being counted. A section declaring no slot is never
    // asked about, and a section whose document is ABSENT counts all of its
    // slots — absent and empty are the same gap.
    //
    // BLOCKING. Boot only, beside bootstrap_sections.
    [[nodiscard]] Result<std::size_t> count_unbound_image_slots(
        mongocxx::client& client, std::span<const SectionSpec> registry,
        SectionState state) const;

    // Bootstrap and first-draft creation: insert-if-absent, tolerating the
    // duplicate-key error rather than reading first.
    //
    // A read-then-write races N booting instances, and a boot path that
    // OVERWRITES turns every deploy into a content wipe. Returns true when THIS
    // call created the document, which is what lets a caller distinguish "I
    // created the first draft" from "somebody else got there first" without a
    // second query.
    [[nodiscard]] Result<bool> insert_if_absent(mongocxx::client& client,
                                                const SectionSpec& spec, SectionState state,
                                                const SectionDocument& content,
                                                const Uuid& actor) const;

    // The normal staff write. Filters on the caller's expected version and
    // $incs it, so two staff saving the same section produce one winner and one
    // VersionMismatch rather than a silent lost update.
    //
    // `session` is the caller's transaction, and it is not optional: the section
    // update and the media reference counts it changes must commit or abort
    // together. A count that commits while the section write aborts leaves live
    // content pointing at files the collector will delete.
    [[nodiscard]] Result<std::int64_t> update(mongocxx::client& client,
                                              mongocxx::client_session& session,
                                              const SectionSpec& spec, SectionState state,
                                              std::int64_t expected_version,
                                              const SectionDocument& content,
                                              const Uuid& actor) const;
};

}  // namespace anvil::sections
