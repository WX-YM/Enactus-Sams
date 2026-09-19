#pragma once

// Crypto failures are a distinct exception type so they can never be caught by
// a handler that meant to catch a validation error and turned a broken CSPRNG
// into a 400.

#include <stdexcept>
#include <string>

namespace anvil::crypto {

class CryptoError final : public std::runtime_error {
public:
    explicit CryptoError(const std::string& what) : std::runtime_error{what} {}
};

}  // namespace anvil::crypto
