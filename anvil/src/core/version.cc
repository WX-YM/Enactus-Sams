#include "anvil/core/version.h"

#ifndef ANVIL_VERSION_MAJOR
#error "ANVIL_VERSION_* must be defined by the build (see src/CMakeLists.txt)"
#endif

namespace anvil {

Version version() noexcept {
    return Version{ANVIL_VERSION_MAJOR, ANVIL_VERSION_MINOR, ANVIL_VERSION_PATCH};
}

std::string_view version_string() noexcept {
    return ANVIL_VERSION_STRING;
}

}  // namespace anvil
