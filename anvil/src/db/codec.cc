#include "anvil/db/codec.h"

#include <cstring>
#include <tuple>

#include <bsoncxx/document/element.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/i18n/utf8.h"

namespace anvil::db {

TimeMs now_ms() noexcept {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

namespace codec {
namespace {

// One place where "the field is absent" and "the field is the wrong type" are
// separated, so a reader never has to remember to check both.
enum class Lookup : std::uint8_t { Ok, Missing, WrongType };

struct Found final {
    bsoncxx::document::element element;
    Lookup                     status;
};

[[nodiscard]] Found lookup(const bsoncxx::document::view& doc, std::string_view field,
                           bsoncxx::type expected) noexcept {
    const bsoncxx::document::element element =
        doc[bsoncxx::stdx::string_view{field.data(), field.size()}];
    if (!element) { return Found{element, Lookup::Missing}; }
    if (element.type() != expected) { return Found{element, Lookup::WrongType}; }
    return Found{element, Lookup::Ok};
}

[[nodiscard]] bool is_null(const bsoncxx::document::view& doc, std::string_view field) noexcept {
    const bsoncxx::document::element element =
        doc[bsoncxx::stdx::string_view{field.data(), field.size()}];
    return !element || element.type() == bsoncxx::type::k_null;
}

// Binary is the type where "the right BSON type" is not enough: subtype 4 with
// 16 bytes is a Uuid, subtype 0 with 16 bytes is a PermSet, and reading one as
// the other is silent corruption. Both are checked before the bytes are touched.
[[nodiscard]] Result<std::span<const std::uint8_t>> read_binary(
    const bsoncxx::document::view& doc, std::string_view field,
    bsoncxx::binary_sub_type sub_type, std::size_t exact_size) {
    const Found found = lookup(doc, field, bsoncxx::type::k_binary);
    if (found.status != Lookup::Ok) { return fail(ErrorCode::Internal, field); }

    const bsoncxx::types::b_binary binary = found.element.get_binary();
    if (binary.sub_type != sub_type || binary.size != exact_size) {
        return fail(ErrorCode::Internal, field);
    }
    return std::span<const std::uint8_t>{binary.bytes, exact_size};
}

}  // namespace

// --- Identifiers ----------------------------------------------------------

Result<Uuid> read_uuid(const bsoncxx::document::view& doc, std::string_view field) {
    // Subtype 4 only. Subtype 3 is the legacy driver-specific UUID with a
    // byte order that differs per driver, and a stored 36-character string is
    // k_string, which never reaches this branch.
    const Result<std::span<const std::uint8_t>> bytes =
        read_binary(doc, field, bsoncxx::binary_sub_type::k_uuid, sizeof(Uuid));
    if (!bytes) { return bytes.error(); }

    Uuid id{};
    std::memcpy(id.data(), bytes.value().data(), id.size());
    return id;
}

Result<std::optional<Uuid>> read_optional_uuid(const bsoncxx::document::view& doc,
                                               std::string_view field) {
    if (is_null(doc, field)) { return std::optional<Uuid>{}; }
    const Result<Uuid> id = read_uuid(doc, field);
    if (!id) { return id.error(); }
    return std::optional<Uuid>{id.value()};
}

// --- Fixed-width binary ---------------------------------------------------

Result<PermSet> read_perm_set(const bsoncxx::document::view& doc, std::string_view field) {
    const Result<std::span<const std::uint8_t>> bytes =
        read_binary(doc, field, bsoncxx::binary_sub_type::k_binary, PermSet::kBytes);
    if (!bytes) { return bytes.error(); }

    std::array<std::uint8_t, PermSet::kBytes> raw{};
    std::memcpy(raw.data(), bytes.value().data(), raw.size());
    return PermSet::from_bytes(raw);
}

Result<crypto::Digest256> read_digest(const bsoncxx::document::view& doc,
                                      std::string_view field) {
    const Result<std::span<const std::uint8_t>> bytes =
        read_binary(doc, field, bsoncxx::binary_sub_type::k_binary,
                    std::tuple_size_v<crypto::Digest256>);
    if (!bytes) { return bytes.error(); }

    crypto::Digest256 digest{};
    std::memcpy(digest.data(), bytes.value().data(), digest.size());
    return digest;
}

Status read_bytes(const bsoncxx::document::view& doc, std::string_view field,
                  std::span<std::uint8_t> out) {
    const Result<std::span<const std::uint8_t>> bytes =
        read_binary(doc, field, bsoncxx::binary_sub_type::k_binary, out.size());
    if (!bytes) { return bytes.error(); }

    std::memcpy(out.data(), bytes.value().data(), out.size());
    return ok();
}

// --- Scalars --------------------------------------------------------------

Result<TimeMs> read_time(const bsoncxx::document::view& doc, std::string_view field) {
    // BSON date is int64 milliseconds since the Unix epoch and carries no zone,
    // which is exactly why it is the only accepted representation: a string
    // timestamp would carry a zone, or worse, imply a local one (docs/09-mongodb.md §2).
    const Found found = lookup(doc, field, bsoncxx::type::k_date);
    if (found.status != Lookup::Ok) { return fail(ErrorCode::Internal, field); }
    return TimeMs{found.element.get_date().value};
}

Result<std::optional<TimeMs>> read_optional_time(const bsoncxx::document::view& doc,
                                                 std::string_view field) {
    if (is_null(doc, field)) { return std::optional<TimeMs>{}; }
    const Result<TimeMs> at = read_time(doc, field);
    if (!at) { return at.error(); }
    return std::optional<TimeMs>{at.value()};
}

Result<std::int32_t> read_int32(const bsoncxx::document::view& doc, std::string_view field) {
    const Found found = lookup(doc, field, bsoncxx::type::k_int32);
    if (found.status != Lookup::Ok) { return fail(ErrorCode::Internal, field); }
    return found.element.get_int32().value;
}

Result<std::int64_t> read_int64(const bsoncxx::document::view& doc, std::string_view field) {
    // Deliberately not accepting int32 and widening. $inc preserves the stored
    // width, so a document whose version was created as int32 would drift into a
    // different type than one created by this build, and a filter on {v: <int64>}
    // matches an int32 4 — but a covered index comparison would not. Writers use
    // append_int64 so the width is fixed at creation.
    const Found found = lookup(doc, field, bsoncxx::type::k_int64);
    if (found.status != Lookup::Ok) { return fail(ErrorCode::Internal, field); }
    return found.element.get_int64().value;
}

Result<bool> read_bool(const bsoncxx::document::view& doc, std::string_view field) {
    const Found found = lookup(doc, field, bsoncxx::type::k_bool);
    if (found.status != Lookup::Ok) { return fail(ErrorCode::Internal, field); }
    return found.element.get_bool().value;
}

// --- Text -----------------------------------------------------------------

Result<std::string_view> read_text(const bsoncxx::document::view& doc, std::string_view field) {
    const Found found = lookup(doc, field, bsoncxx::type::k_string);
    if (found.status != Lookup::Ok) { return fail(ErrorCode::Internal, field); }

    const auto stored = found.element.get_string().value;
    const std::string_view text{stored.data(), stored.size()};
    if (i18n::validate(text) != i18n::Utf8Error::Ok) { return fail(ErrorCode::Internal, field); }
    return text;
}

Status check_localized(LocalizedView text) noexcept {
    if (!text.complete()) { return fail(ErrorCode::ValidationFailed); }
    for (const std::string_view value : text.values) {
        if (i18n::validate(value) != i18n::Utf8Error::Ok) {
            return fail(ErrorCode::ValidationFailed);
        }
    }
    return ok();
}

Result<LocalizedView> read_localized(const bsoncxx::document::view& doc,
                                     std::string_view field) {
    const Found found = lookup(doc, field, bsoncxx::type::k_document);
    if (found.status != Lookup::Ok) { return fail(ErrorCode::Internal, field); }

    const bsoncxx::document::view sub = found.element.get_document().value;

    LocalizedView text{};
    for (std::size_t i = 0; i < kLocaleCount; ++i) {
        const Result<std::string_view> value = read_text(sub, config::kLocales[i].tag);
        if (!value) { return fail(ErrorCode::Internal, field); }
        text.values[i] = value.value();
    }

    // Present but empty is the same defect as missing: it renders as a blank
    // heading in that locale, silently. A stored document that has one is a
    // document written by something that bypassed append_localized.
    if (!text.complete()) { return fail(ErrorCode::Internal, field); }
    return text;
}

}  // namespace codec
}  // namespace anvil::db
