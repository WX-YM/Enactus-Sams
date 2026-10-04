#pragma once

// Per-request scratch: one bump-pointer region on the stack, one bulk release,
// zero individual frees.
//
// A JSON document of any interesting shape is hundreds of small allocations —
// a node here, a vector growth there — and each one is a lock, a potential
// cache miss, and a free() later. A monotonic_buffer_resource over a stack
// array turns all of them into pointer arithmetic, and turns cleanup into
// letting the object go out of scope (CLAUDE.md §2.1).
//
// The inline buffer is sized for the p99 body. Past it the resource spills to
// the upstream heap resource rather than failing, so a legitimate large-but-
// bounded document still parses; the size cap, not the arena, is what rejects
// hostile input.

#include <array>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>

namespace anvil::input {

template <std::size_t InlineBytes>
class RequestArena final {
public:
    RequestArena() noexcept
        : buffer_{}, resource_{buffer_.data(), buffer_.size(), std::pmr::new_delete_resource()} {}

    RequestArena(const RequestArena&) = delete;
    RequestArena& operator=(const RequestArena&) = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept { return &resource_; }

    // Uninitialised storage for `count` objects. Trivially destructible only:
    // nothing in this arena is ever destroyed individually, so a type with a
    // destructor would leak whatever it owns.
    template <typename T>
    [[nodiscard]] std::span<T> allocate(std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "arena objects are never destroyed individually");
        if (count == 0) { return {}; }
        void* memory = resource_.allocate(count * sizeof(T), alignof(T));
        return std::span<T>{static_cast<T*>(memory), count};
    }

    [[nodiscard]] std::span<char> allocate_chars(std::size_t count) {
        return allocate<char>(count);
    }

private:
    // Declaration order is load-bearing: the resource is constructed with a
    // pointer into buffer_, so buffer_ must be alive first and must outlive it.
    std::array<std::byte, InlineBytes>  buffer_;
    std::pmr::monotonic_buffer_resource resource_;
};

// The default for a request body. Sized from the worst realistic shape rather
// than from the body size: a 6 KB document of many small objects needs roughly
// 40 KiB of nodes, because the cost is per NODE, not per byte. 64 KiB of stack
// is nothing against the 8 MB a pool thread has, and the alternative — spilling
// to the heap on ordinary input — is the allocation this whole design exists to
// avoid. tests/validation_fuzz_test.cc asserts a realistic body stays inline.
using BodyArena = RequestArena<65536>;

}  // namespace anvil::input
