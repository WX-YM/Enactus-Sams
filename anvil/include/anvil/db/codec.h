#pragma once

// BSON encoding and decoding for the value types in lib/core.
//
// Every rule here is a storage rule from docs/09-mongodb.md §2, enforced in
// both directions:
//
//   Uuid        BinData subtype 4, exactly 16 bytes. Never a 36-character
//               string: that is 2.25x the payload before BSON framing, it
//               inflates every index that references it, and it turns a 16-byte
//               memcmp into a string compare.
//   PermSet     BinData subtype 0, exactly 16 bytes, little-endian words.
//   Digest256   BinData subtype 0, exactly 32 bytes.
//   Timestamps  BSON date (int64 milliseconds), always UTC, never a string.
//   Enums       int32, range-checked on the way in. Never a display string.
//   Localized   { en, ar } subdocument. Both required, both non-empty.
//
// DECODING NEVER COERCES. A field carrying the wrong BSON type is an error, not
// a conversion. A stored 36-character id silently parsed into a Uuid, or the
// string "1" read as an enum, laundered corruption into a valid-looking value
// and would let a document written by an older or hostile writer pass every
// downstream check.
//
// Failures come back as ErrorCode::Internal, not ValidationFailed: this data was
// written by this system, so a type mismatch is an integrity fault rather than
// bad user input. The Failure carries the field NAME and never the value —
// echoing a stored value into a log line or a response is how a document's
// contents reach somebody who was never authorised to read them.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/sub_document.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/stdx/string_view.hpp>
#include <bsoncxx/types.hpp>

#include "anvil/core/perm_set.h"
#include "anvil/core/result.h"
#include "anvil/core/locale.h"
#include "anvil/core/types.h"
#include "anvil/crypto/secret.h"

namespace anvil::db {

// Millisecond resolution, because that is exactly what BSON date stores. A
// system_clock::time_point holds nanoseconds on this platform, so round-tripping
// one through BSON silently truncates; making the truncation part of the type
// means it happens once, at the boundary, instead of surprising a comparison.
using TimeMs = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

[[nodiscard]] TimeMs now_ms() noexcept;

namespace codec {

// --- Encoding -------------------------------------------------------------
//
// The b_* views below borrow their bytes: bsoncxx::types::b_binary holds a
// pointer, not a copy. They are safe inside a single full expression —
//     make_document(kvp("_id", codec::uuid_bin(id)))
// serialises before the temporary dies — and they are a dangling read anywhere
// else. Never store one, never return one from a function whose argument is a
// temporary.

// bsoncxx's key type is its own string_view polyfill, and std::string_view is
// not implicitly convertible to it. One conversion here keeps every append site
// reading as an ordinary C++ call.
[[nodiscard]] inline bsoncxx::stdx::string_view key_of(std::string_view key) noexcept {
    return bsoncxx::stdx::string_view{key.data(), key.size()};
}

[[nodiscard]] inline bsoncxx::types::b_binary uuid_bin(const Uuid& id) noexcept {
    return bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_uuid,
                                    static_cast<std::uint32_t>(id.size()), id.data()};
}

[[nodiscard]] inline bsoncxx::types::b_binary digest_bin(const crypto::Digest256& d) noexcept {
    return bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                    static_cast<std::uint32_t>(d.size()), d.data()};
}

template <std::size_t N>
[[nodiscard]] bsoncxx::types::b_binary bytes_bin(const std::array<std::uint8_t, N>& b) noexcept {
    return bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                    static_cast<std::uint32_t>(N), b.data()};
}

[[nodiscard]] inline bsoncxx::types::b_date time_date(TimeMs at) noexcept {
    return bsoncxx::types::b_date{at.time_since_epoch()};
}

// Builder appends. These serialise immediately, so the borrowed-bytes hazard
// above does not apply to them — the array lives across the append call.
template <typename Builder>
void append_uuid(Builder& doc, std::string_view key, const Uuid& id) {
    doc.append(bsoncxx::builder::basic::kvp(key_of(key), uuid_bin(id)));
}

template <typename Builder>
void append_optional_uuid(Builder& doc, std::string_view key, const std::optional<Uuid>& id) {
    if (id.has_value()) {
        doc.append(bsoncxx::builder::basic::kvp(key_of(key), uuid_bin(*id)));
    } else {
        // Written explicitly rather than omitted: a partial index that filters
        // on this field has to see it on every document, and a partial filter
        // over a field that is sometimes absent selects a different set than
        // the one the index was declared for.
        doc.append(bsoncxx::builder::basic::kvp(key_of(key), bsoncxx::types::b_null{}));
    }
}

template <typename Builder>
void append_perm_set(Builder& doc, std::string_view key, const PermSet& perms) {
    const std::array<std::uint8_t, PermSet::kBytes> bytes = perms.to_bytes();
    doc.append(bsoncxx::builder::basic::kvp(key_of(key), bytes_bin(bytes)));
}

template <typename Builder>
void append_digest(Builder& doc, std::string_view key, const crypto::Digest256& digest) {
    doc.append(bsoncxx::builder::basic::kvp(key_of(key), digest_bin(digest)));
}

template <typename Builder>
void append_time(Builder& doc, std::string_view key, TimeMs at) {
    doc.append(bsoncxx::builder::basic::kvp(key_of(key), time_date(at)));
}

template <typename Builder>
void append_optional_time(Builder& doc, std::string_view key, const std::optional<TimeMs>& at) {
    if (at.has_value()) {
        doc.append(bsoncxx::builder::basic::kvp(key_of(key), time_date(*at)));
    } else {
        doc.append(bsoncxx::builder::basic::kvp(key_of(key), bsoncxx::types::b_null{}));
    }
}

// Enums are stored as int32. The underlying type is usually uint8_t, so the
// cast is widening and cannot lose a value.
template <typename Builder, typename E>
void append_enum(Builder& doc, std::string_view key, E value) {
    static_assert(std::is_enum_v<E>, "append_enum is for enumerations only");
    doc.append(bsoncxx::builder::basic::kvp(
        key_of(key), bsoncxx::types::b_int32{static_cast<std::int32_t>(value)}));
}

// Version fields are always int64: $inc preserves the stored width, so a
// version written as int32 stays int32 forever and read_int64 would then reject
// it. Fixing the width at the one place versions are created removes the class.
template <typename Builder>
void append_int64(Builder& doc, std::string_view key, std::int64_t value) {
    doc.append(bsoncxx::builder::basic::kvp(key_of(key), bsoncxx::types::b_int64{value}));
}

// Both languages are required. A missing Arabic value is a validation error, not
// an English fallback (docs/03-i18n-utf8.md §1), and the check belongs here so
// that no writer can bypass it by building the subdocument by hand.
template <typename Builder>
[[nodiscard]] Status append_localized(Builder& doc, std::string_view key, LocalizedView text);

// --- Decoding -------------------------------------------------------------
//
// Each reader asserts the exact BSON type and, for binary, the exact subtype and
// length. `field` is a compile-time field name used only for the log line.

[[nodiscard]] Result<Uuid> read_uuid(const bsoncxx::document::view& doc, std::string_view field);
[[nodiscard]] Result<std::optional<Uuid>> read_optional_uuid(const bsoncxx::document::view& doc,
                                                             std::string_view field);

[[nodiscard]] Result<PermSet> read_perm_set(const bsoncxx::document::view& doc,
                                            std::string_view field);
[[nodiscard]] Result<crypto::Digest256> read_digest(const bsoncxx::document::view& doc,
                                                    std::string_view field);

[[nodiscard]] Result<TimeMs> read_time(const bsoncxx::document::view& doc,
                                       std::string_view field);
[[nodiscard]] Result<std::optional<TimeMs>> read_optional_time(const bsoncxx::document::view& doc,
                                                               std::string_view field);

[[nodiscard]] Result<std::int32_t> read_int32(const bsoncxx::document::view& doc,
                                              std::string_view field);
[[nodiscard]] Result<std::int64_t> read_int64(const bsoncxx::document::view& doc,
                                              std::string_view field);
[[nodiscard]] Result<bool> read_bool(const bsoncxx::document::view& doc, std::string_view field);

// The returned view points into `doc`'s underlying buffer. It is valid exactly
// as long as that buffer is — copy it before it crosses a thread-pool boundary
// (ENGINEERING_RULES.md §2.2). The bytes are validated against the full UTF-8 policy:
// stored text that is malformed is rejected, never repaired.
[[nodiscard]] Result<std::string_view> read_text(const bsoncxx::document::view& doc,
                                                 std::string_view field);
[[nodiscard]] Result<LocalizedView> read_localized(const bsoncxx::document::view& doc,
                                                   std::string_view field);

// Fixed-length binary of subtype 0, copied into the caller's array. Used for the
// packed IP (16 B) and the truncated user-agent hash (8 B) in user_sessions.
[[nodiscard]] Status read_bytes(const bsoncxx::document::view& doc, std::string_view field,
                                std::span<std::uint8_t> out);

// int32 plus a range check. `max_valid` is the highest defined enumerator: an
// out-of-range value is corruption or an older writer, and either way it must
// not become a valid-looking enum.
template <typename E>
[[nodiscard]] Result<E> read_enum(const bsoncxx::document::view& doc, std::string_view field,
                                  E max_valid) {
    static_assert(std::is_enum_v<E>, "read_enum is for enumerations only");
    const Result<std::int32_t> raw = read_int32(doc, field);
    if (!raw) { return raw.error(); }
    const std::int32_t value = raw.value();
    if (value < 0 || value > static_cast<std::int32_t>(max_valid)) {
        return fail(ErrorCode::Internal, field);
    }
    return static_cast<E>(value);
}

// --- Implementation of the templates that need a decoder -------------------

[[nodiscard]] Status check_localized(LocalizedView text) noexcept;

template <typename Builder>
[[nodiscard]] Status append_localized(Builder& doc, std::string_view key, LocalizedView text) {
    const Status valid = check_localized(text);
    if (!valid) { return valid; }
    // Keyed by the locale TAG, in the application's declared order. The order is
    // not load-bearing for a BSON subdocument, but writing it consistently means
    // two instances that received the same content produce byte-identical
    // documents — which is what makes a hash of one a usable ETag.
    doc.append(bsoncxx::builder::basic::kvp(
        key_of(key), [&text](bsoncxx::builder::basic::sub_document sub) {
            for (std::size_t i = 0; i < kLocaleCount; ++i) {
                sub.append(bsoncxx::builder::basic::kvp(
                    key_of(config::kLocales[i].tag),
                    bsoncxx::types::b_string{key_of(text.values[i])}));
            }
        }));
    return ok();
}

}  // namespace codec
}  // namespace anvil::db
