#pragma once

// The writer that IS the declaration.
//
// --- the second copy this exists to refuse ----------------------------------
//
// Every other table a generated client is built from is read by the server as
// well: the route table is what the filter resolves, the permission catalogue is
// what a mask is named from, the rate-limit rules are what the limiter counts
// against. A client generated from them is generated from the thing that runs.
//
// Response SHAPES were the one exception. anvil writes responses by hand, key by
// key, into a `std::string`, so nothing described a body and a generated client
// declared its own types — hand-written, unverified, and the one part of a
// generated client the descriptor did not underwrite.
//
// The tempting fix is a `ResponseSpec` table emitted into the descriptor while
// handlers keep appending by hand. That is worse than no table: it is a SECOND
// copy of the response shape, in the place least able to check itself, and it
// would be believed. A handler that stopped writing `locale` would leave the
// declared schema saying it still does, and the failure would land on whoever
// generated the client.
//
// So the spec IS the writer. There is no way to produce a described body except
// by walking the declaration in order, because the walk is the API:
//
//     http::write_object<kMeResponse>(body)
//         .uuid<"id">(ctx.user_id)
//         .text<"locale">(tag)
//         .boolean<"superadmin">(is_super)
//         .done();
//
// Each call returns the writer for the NEXT declared field. A call naming a key
// that is not next, a call of the wrong type for the field that is, and a
// `done()` before the last field are all `static_assert`s — so the emitted
// schema is a description of the bytes rather than a claim about them.
//
// The chain is why it works, and also exactly where it stops working: the
// position is a template parameter, so every call site has to know statically
// which field it is writing.
//
// That is a narrower restriction than it first sounds. A writer is a value
// holding one reference, so a runtime BRANCH is fine — both arms start from the
// same statically-known position and each is checked there, which is how a
// nullable field is written:
//
//     auto at_perms = write_object<kMeResponse>(body)
//         .uuid<"id">(ctx.user_id)
//         .text<"locale">(tag);
//     if (is_superadmin(ctx.user_type)) { at_perms.null_field<"permissions">().done(); }
//     else                              { at_perms.strings<"permissions">(held).done(); }
//
// What genuinely cannot be expressed is a LOOP over the fields — choosing a key
// by a runtime index — and that is the shape this refuses on purpose. The
// alternative is a runtime cursor, which turns a build error into a 500.
//
// --- what it covers, and what stays hand-written ----------------------------
//
// FLAT objects, and arrays of one declared shape. That is the whole scope, and
// it is scoped deliberately rather than approximated: a nested-object grammar
// here would be a schema language, and a schema language that describes 80% of
// the bodies is one a client still cannot trust.
//
// A route with no spec emits `"response":null`, and a generated client falls
// back to its own declaration — which is what every route does today. So
// adoption is per route rather than a flag day, and a body too rich for this
// stays hand-written and honestly undescribed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "anvil/core/types.h"
#include "anvil/http/json_writer.h"
#include "anvil/http/response_spec.h"

namespace anvil::http {

// A key, as a template argument.
//
// C++20 class-type non-type template parameters are what make the position
// check possible at all: without a name in the type system, a writer could only
// compare strings at run time, and a runtime check on a response shape is a 500
// where a build error belongs.
template <std::size_t N>
struct FieldName final {
    std::array<char, N> text{};

    consteval FieldName(const char (&literal)[N]) noexcept {  // NOLINT: the point is implicit use
        for (std::size_t i = 0; i < N; ++i) { text[i] = literal[i]; }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view{text.data(), N - 1};  // without the terminator
    }
};

namespace detail {

// `Fields[I]` is only evaluated when the bound holds, because `&&` short
// circuits during constant evaluation. Spelled as one predicate rather than two
// asserts so that an out-of-range position reports as "too many fields" instead
// of as a hard error inside the subscript.
template <const auto& Fields>
[[nodiscard]] constexpr bool position_is(std::size_t i, std::string_view name,
                                         FieldKind kind) noexcept {
    return i < Fields.size() && Fields[i].name == name && Fields[i].kind == kind;
}

template <const auto& Fields>
[[nodiscard]] constexpr bool position_is_nullable(std::size_t i, std::string_view name) noexcept {
    return i < Fields.size() && Fields[i].name == name && Fields[i].nullable;
}

}  // namespace detail

// The writer for one object, positioned at declared field `I`.
//
// It holds a reference to the caller's buffer and nothing else — no state, no
// allocation, and no separate "have I started" flag, because the position is in
// the type. Every method returns the next writer by value, so the chain is a
// sequence of empty objects the optimiser folds into straight-line appends.
template <const auto& Fields, std::size_t I>
class ObjectWriter final {
public:
    explicit constexpr ObjectWriter(std::string& out) noexcept : out_{out} {}

    template <FieldName Name>
    [[nodiscard]] ObjectWriter<Fields, I + 1> text(std::string_view value) const {
        static_assert(detail::position_is<Fields>(I, Name.view(), FieldKind::String),
                      "this is not the next declared field, or it is not a string");
        key<Name>();
        append_json_string(out_, value);
        return ObjectWriter<Fields, I + 1>{out_};
    }

    template <FieldName Name>
    [[nodiscard]] ObjectWriter<Fields, I + 1> number(std::int64_t value) const {
        static_assert(detail::position_is<Fields>(I, Name.view(), FieldKind::Int),
                      "this is not the next declared field, or it is not an int");
        key<Name>();
        append_json_int(out_, value);
        return ObjectWriter<Fields, I + 1>{out_};
    }

    template <FieldName Name>
    [[nodiscard]] ObjectWriter<Fields, I + 1> boolean(bool value) const {
        static_assert(detail::position_is<Fields>(I, Name.view(), FieldKind::Bool),
                      "this is not the next declared field, or it is not a bool");
        key<Name>();
        out_ += value ? "true" : "false";
        return ObjectWriter<Fields, I + 1>{out_};
    }

    template <FieldName Name>
    [[nodiscard]] ObjectWriter<Fields, I + 1> uuid(const Uuid& id) const {
        static_assert(detail::position_is<Fields>(I, Name.view(), FieldKind::Uuid),
                      "this is not the next declared field, or it is not a uuid");
        key<Name>();
        append_json_uuid(out_, id);
        return ObjectWriter<Fields, I + 1>{out_};
    }

    template <FieldName Name>
    [[nodiscard]] ObjectWriter<Fields, I + 1> time_ms(std::int64_t epoch_ms) const {
        static_assert(detail::position_is<Fields>(I, Name.view(), FieldKind::Time),
                      "this is not the next declared field, or it is not a time");
        key<Name>();
        append_json_time(out_, epoch_ms);
        return ObjectWriter<Fields, I + 1>{out_};
    }

    template <FieldName Name>
    [[nodiscard]] ObjectWriter<Fields, I + 1> strings(
        std::span<const std::string_view> values) const {
        static_assert(detail::position_is<Fields>(I, Name.view(), FieldKind::Strings),
                      "this is not the next declared field, or it is not a string array");
        key<Name>();
        out_ += '[';
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) { out_ += ','; }
            append_json_string(out_, values[i]);
        }
        out_ += ']';
        return ObjectWriter<Fields, I + 1>{out_};
    }

    // JSON `null` for the field at this position, whatever its kind.
    //
    // Refused unless the declaration says the field is nullable, which is the
    // half of the contract a client cannot defend itself against: a field
    // declared always-present and sent as null is a crash in the consumer, on
    // the field it was told it could trust.
    template <FieldName Name>
    [[nodiscard]] ObjectWriter<Fields, I + 1> null_field() const {
        static_assert(detail::position_is_nullable<Fields>(I, Name.view()),
                      "this is not the next declared field, or it is not declared nullable");
        key<Name>();
        out_ += "null";
        return ObjectWriter<Fields, I + 1>{out_};
    }

    // Closes the object. Refuses to close one that is missing a declared field,
    // because a schema promising a key the server never wrote is the same defect
    // as one omitting a key it does — read from the other side.
    void done() const {
        static_assert(I == Fields.size(),
                      "the object is short of its declared fields; every declared key is "
                      "written or the schema describes bytes that are not sent");
        out_ += '}';
    }

private:
    template <FieldName Name>
    void key() const {
        if constexpr (I != 0) { out_ += ','; }
        // The DECLARED name, not the template argument, although the assertion
        // above proves them equal. The one that reaches the wire is then always
        // the one the schema emits, even if that assertion is ever relaxed.
        append_json_key(out_, Fields[I].name);
    }

    std::string& out_;
};

// Begins a described object in `out`.
//
// `Fields` is a reference to a namespace-scope `inline constexpr
// std::array<ResponseField, N>` — the same shape every other seam in this
// library takes, and it has to be a reference rather than a value because a
// `std::span` is not a structural type and cannot be a template argument.
template <const auto& Fields>
[[nodiscard]] ObjectWriter<Fields, 0> write_object(std::string& out) {
    static_assert(response_shape_is_well_formed(std::span<const ResponseField>{Fields}),
                  "a response shape with an empty, non-UTF-8 or duplicated key");
    out += '{';
    return ObjectWriter<Fields, 0>{out};
}

// An array of one declared shape.
//
// Not a method on the object writer, and not nestable: an array of arrays or an
// array of differing shapes is exactly the schema language this header refuses
// to become. Its elements are written through the same `ObjectWriter`, so an
// element is subject to the identical position checks.
template <const auto& Fields>
class ArrayWriter final {
public:
    explicit ArrayWriter(std::string& out) : out_{out} {
        static_assert(response_shape_is_well_formed(std::span<const ResponseField>{Fields}),
                      "a response shape with an empty, non-UTF-8 or duplicated key");
        out_ += '[';
    }

    // The separator is the one piece of state a compile-time position cannot
    // carry, because the element COUNT is a run-time fact. It is also the only
    // one: everything inside an element is checked exactly as an object is.
    [[nodiscard]] ObjectWriter<Fields, 0> item() {
        if (!first_) { out_ += ','; }
        first_ = false;
        out_ += '{';
        return ObjectWriter<Fields, 0>{out_};
    }

    void done() { out_ += ']'; }

    ArrayWriter(const ArrayWriter&) = delete;
    ArrayWriter& operator=(const ArrayWriter&) = delete;

private:
    std::string& out_;
    bool         first_{true};
};

}  // namespace anvil::http
