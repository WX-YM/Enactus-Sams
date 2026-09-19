#include "anvil/core/descriptor_budget.h"

#include <sys/resource.h>

#include <cstdint>

namespace anvil {

std::size_t descriptor_ceiling(DescriptorShare share) noexcept {
    if (share.denominator == 0) { return 0; }

    rlimit limit{};
    // A getrlimit that fails leaves nothing to derive from, and guessing a
    // ceiling would defeat the point of deriving one. Zero refuses every
    // connection at the door, which is a visible, recoverable configuration
    // failure rather than an invisible descriptor exhaustion later.
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) { return 0; }

    // RLIM_INFINITY is a real value on some systems and is not a number of
    // descriptors. Treat it as the largest budget worth deriving from rather than
    // overflowing the arithmetic below.
    const auto budget = (limit.rlim_cur == RLIM_INFINITY)
                            ? static_cast<std::uint64_t>(1) << 20U
                            : static_cast<std::uint64_t>(limit.rlim_cur);
    if (budget <= kDescriptorReserve) { return 0; }

    // std::uint64_t throughout: rlim_t is 64 bits and the infinity case above
    // substitutes a value larger than any real descriptor limit, so the
    // multiplication has to be done in a width that holds it.
    const std::uint64_t spendable = budget - kDescriptorReserve;
    return (spendable * share.numerator) / share.denominator;
}

}  // namespace anvil
