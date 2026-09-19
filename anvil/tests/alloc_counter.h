#pragma once

// The allocation counter anvil_alloc_tests is built around.
//
// "Zero allocations on this path" is a budget stated in a doc, and it is exactly
// the kind of claim that quietly stops being true. Counting is the only way to
// know: this binary replaces global operator new so a test can assert that a
// path did not allocate, rather than asserting that it was written not to.
//
// The counter lives HERE, as one inline object, because more than one test
// translation unit needs it and a per-TU copy would count only its own file's
// allocations — which is a test that passes while measuring nothing. The
// replacement operators themselves stay in exactly one translation unit
// (validation_fuzz_test.cc), because a replacement operator defined twice is an
// ODR violation the linker is not required to diagnose.

#include <atomic>
#include <cstddef>

namespace anvil::testing {

inline std::atomic<std::size_t> g_allocations{0};

class AllocationCounter final {
public:
    AllocationCounter() noexcept : start_{g_allocations.load(std::memory_order_relaxed)} {}

    [[nodiscard]] std::size_t count() const noexcept {
        return g_allocations.load(std::memory_order_relaxed) - start_;
    }

private:
    std::size_t start_;
};

}  // namespace anvil::testing
