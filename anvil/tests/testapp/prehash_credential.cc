// The client stage of docs/05 §12, as a command, for a harness driving the
// reference server with curl.
//
//   printf '%s' "$password" | testapp_prehash_credential <salt> <m> <t> <p>
//
// prints the credential a browser would send: base64url(Argon2id(UTF-8(NFC(
// password)), salt, m, t, p, T = 32)). The salt is the base64url the salt route
// answered with.
//
// It exists because nothing else on a build machine can do this from a shell.
// The reference `argon2` command takes its salt as a command-line STRING, and a
// derived salt is sixteen arbitrary bytes — a NUL among them is routine, and no
// shell argument can carry one.
//
// The password arrives on stdin and never in argv: an argument is readable by
// every user on the machine through `ps` for as long as the process runs.
//
// A test binary, like the server it drives. A browser computes this itself;
// nothing in a deployment should ever call a program to do it.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <openssl/crypto.h>

#include "anvil/auth/password.h"
#include "anvil/auth/prehash.h"
#include "anvil/crypto/argon2.h"
#include "anvil/crypto/base64url.h"
#include "anvil/i18n/normalize.h"

namespace {

[[nodiscard]] std::optional<std::uint32_t> positive(const char* text) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > 0xFFFFFFFFUL) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: printf '%%s' \"$password\" | %s <salt-b64url> <memory_kib> "
                     "<iterations> <parallelism>\n",
                     argv[0]);
        return 2;
    }

    anvil::auth::PrehashSalt salt{};
    const std::optional<std::size_t> written =
        anvil::crypto::base64url_decode_into(argv[1], salt);
    const std::optional<std::uint32_t> memory = positive(argv[2]);
    const std::optional<std::uint32_t> iterations = positive(argv[3]);
    const std::optional<std::uint32_t> parallelism = positive(argv[4]);
    if (!written.has_value() || *written != salt.size() || !memory || !iterations ||
        !parallelism) {
        std::fprintf(stderr, "a 16-byte base64url salt and three positive numbers\n");
        return 2;
    }

    std::string password{std::istreambuf_iterator<char>{std::cin},
                         std::istreambuf_iterator<char>{}};
    if (password.empty() || password.size() > anvil::auth::kMaxPasswordBytes) {
        std::fprintf(stderr, "a password of 1..%zu bytes on stdin\n",
                     anvil::auth::kMaxPasswordBytes);
        return 2;
    }

    std::optional<std::string> nfc =
        anvil::i18n::normalize(password, anvil::i18n::NormalizeMode::Nfc);
    OPENSSL_cleanse(password.data(), password.size());
    if (!nfc.has_value()) {
        std::fprintf(stderr, "the password is not valid UTF-8\n");
        return 2;
    }

    anvil::auth::PrehashKey k;
    try {
        anvil::crypto::argon2_hash_raw(
            anvil::crypto::Argon2Type::Argon2id, anvil::crypto::kArgon2Version13,
            {.memory_kib = *memory, .iterations = *iterations, .parallelism = *parallelism},
            {reinterpret_cast<const std::uint8_t*>(nfc->data()), nfc->size()}, salt, {}, {},
            k.mutable_span());
    } catch (const std::exception& failure) {
        OPENSSL_cleanse(nfc->data(), nfc->size());
        std::fprintf(stderr, "%s\n", failure.what());
        return 1;
    }
    OPENSSL_cleanse(nfc->data(), nfc->size());

    std::printf("%s\n", anvil::crypto::base64url_encode(k.span()).c_str());
    return 0;
}
