#include "anvil/notifications/webpush.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>
#include <openssl/params.h>

#include "anvil/crypto/base64url.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/random.h"
#include "anvil/crypto/secret.h"

namespace anvil::notifications {
namespace {

constexpr std::string_view kPushField = "push";
constexpr std::string_view kSubscriptionField = "p256dh";
constexpr std::string_view kPayloadField = "payload";

// RFC 8291 §3.3 and §3.4. The trailing NUL is part of each label and is the most
// commonly dropped byte in an implementation that "looks right" and interoperates
// with nothing.
constexpr std::string_view kWebPushInfo = "WebPush: info\0";
constexpr std::string_view kCekInfo = "Content-Encoding: aes128gcm\0";
constexpr std::string_view kNonceInfo = "Content-Encoding: nonce\0";

// RFC 8188 §2: the last record's plaintext is terminated by 0x02, any other
// record's by 0x01. There is only ever one record here, so it is always 0x02.
constexpr std::uint8_t kLastRecordDelimiter = 0x02;

[[nodiscard]] std::span<const std::uint8_t> bytes_of(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// Each label carries its terminating NUL, which a string_view over a literal
// does not include by default.
[[nodiscard]] std::span<const std::uint8_t> label_of(std::string_view literal) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(literal.data()), literal.size() + 1};
}

// RAII for the OpenSSL handles below. One resource each, rule of zero everywhere
// else (ENGINEERING_RULES.md §3.3) — there is no normal path here with a bare free on it.
//
// Everything key-shaped goes through EVP_PKEY rather than EC_KEY: the whole
// EC_KEY family is deprecated as of OpenSSL 3.0, and building against a
// deprecated API is a build that starts failing on somebody else's schedule.
// EC_GROUP and EC_POINT are not deprecated and are still the right tools for the
// point arithmetic, so they stay.
struct EvpPkeyDeleter final {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};
struct EvpPkeyCtxDeleter final {
    void operator()(EVP_PKEY_CTX* ctx) const noexcept { EVP_PKEY_CTX_free(ctx); }
};
struct EvpMdCtxDeleter final {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};
struct EvpCipherCtxDeleter final {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept { EVP_CIPHER_CTX_free(ctx); }
};
struct EcGroupDeleter final {
    void operator()(EC_GROUP* group) const noexcept { EC_GROUP_free(group); }
};
struct EcPointDeleter final {
    void operator()(EC_POINT* point) const noexcept { EC_POINT_free(point); }
};
struct BignumDeleter final {
    void operator()(BIGNUM* value) const noexcept { BN_clear_free(value); }
};
struct EcdsaSigDeleter final {
    void operator()(ECDSA_SIG* sig) const noexcept { ECDSA_SIG_free(sig); }
};
struct OsslParamBldDeleter final {
    void operator()(OSSL_PARAM_BLD* bld) const noexcept { OSSL_PARAM_BLD_free(bld); }
};
struct OsslParamDeleter final {
    void operator()(OSSL_PARAM* params) const noexcept { OSSL_PARAM_free(params); }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter>;
using EcGroupPtr = std::unique_ptr<EC_GROUP, EcGroupDeleter>;
using EcPointPtr = std::unique_ptr<EC_POINT, EcPointDeleter>;
using BignumPtr = std::unique_ptr<BIGNUM, BignumDeleter>;
using EcdsaSigPtr = std::unique_ptr<ECDSA_SIG, EcdsaSigDeleter>;
using ParamBldPtr = std::unique_ptr<OSSL_PARAM_BLD, OsslParamBldDeleter>;
using ParamPtr = std::unique_ptr<OSSL_PARAM, OsslParamDeleter>;

constexpr const char* kCurveName = "P-256";

[[nodiscard]] EcGroupPtr p256_group() {
    return EcGroupPtr{EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1)};
}

// The uncompressed point, validated.
//
// EC_POINT_oct2point REFUSES a point that is not on the curve, which is the check
// that matters: multiplying our ephemeral scalar by an off-curve point is an
// invalid-curve attack, and it leaks the scalar a few bits at a time. It is not
// an optional validation step — it is the reason this goes through OpenSSL rather
// than through a memcpy of the coordinates.
[[nodiscard]] EcPointPtr point_from_octets(const EC_GROUP* group,
                                           std::span<const std::uint8_t> uncompressed) {
    if (uncompressed.size() != kP256PublicBytes || uncompressed[0] != 0x04) { return nullptr; }
    EcPointPtr point{EC_POINT_new(group)};
    if (!point) { return nullptr; }
    if (EC_POINT_oct2point(group, point.get(), uncompressed.data(), uncompressed.size(),
                           nullptr) != 1) {
        return nullptr;
    }
    // The point at infinity passes oct2point and is a valid encoding of nothing.
    // An ECDH against it produces a shared secret an attacker already knows.
    if (EC_POINT_is_at_infinity(group, point.get()) == 1) { return nullptr; }
    return point;
}

[[nodiscard]] bool point_to_octets(const EC_GROUP* group, const EC_POINT* point,
                                   std::array<std::uint8_t, kP256PublicBytes>& out) {
    return EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, out.data(),
                              out.size(), nullptr) == out.size();
}

// An EVP_PKEY built from explicit parameters. `private_scalar` may be null, which
// is how a public-only key is built.
[[nodiscard]] PkeyPtr key_from_parts(std::span<const std::uint8_t> public_point,
                                     const BIGNUM* private_scalar) {
    ParamBldPtr builder{OSSL_PARAM_BLD_new()};
    if (!builder) { return nullptr; }
    if (OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, kCurveName,
                                        0) != 1) {
        return nullptr;
    }
    if (OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY,
                                         public_point.data(), public_point.size()) != 1) {
        return nullptr;
    }
    if (private_scalar != nullptr &&
        OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_PRIV_KEY, private_scalar) != 1) {
        return nullptr;
    }

    ParamPtr params{OSSL_PARAM_BLD_to_param(builder.get())};
    if (!params) { return nullptr; }

    PkeyCtxPtr ctx{EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)};
    if (!ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1) { return nullptr; }

    EVP_PKEY* raw = nullptr;
    const int selection = private_scalar != nullptr ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
    if (EVP_PKEY_fromdata(ctx.get(), &raw, selection, params.get()) != 1) { return nullptr; }
    PkeyPtr key{raw};

    // fromdata imports; it does not validate. A public point that reached here
    // came through point_from_octets, but a key assembled anywhere else must not
    // be trusted on the strength of having been constructed.
    PkeyCtxPtr check{EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr)};
    if (!check || EVP_PKEY_public_check(check.get()) != 1) { return nullptr; }
    return key;
}

[[nodiscard]] PkeyPtr public_key_from_point(std::span<const std::uint8_t> uncompressed) {
    const EcGroupPtr group = p256_group();
    if (!group) { return nullptr; }
    const EcPointPtr point = point_from_octets(group.get(), uncompressed);
    if (!point) { return nullptr; }
    std::array<std::uint8_t, kP256PublicBytes> canonical{};
    if (!point_to_octets(group.get(), point.get(), canonical)) { return nullptr; }
    return key_from_parts(canonical, nullptr);
}

// The public half is DERIVED from the scalar rather than stored beside it: two
// copies of one key pair is one of them being able to disagree with the other.
[[nodiscard]] PkeyPtr private_key_from_scalar(std::span<const std::uint8_t> scalar,
                                              std::array<std::uint8_t, kP256PublicBytes>& out) {
    if (scalar.size() != kP256PrivateBytes) { return nullptr; }
    const EcGroupPtr group = p256_group();
    if (!group) { return nullptr; }

    BignumPtr value{BN_bin2bn(scalar.data(), static_cast<int>(scalar.size()), nullptr)};
    if (!value || BN_is_zero(value.get())) { return nullptr; }

    EcPointPtr point{EC_POINT_new(group.get())};
    if (!point) { return nullptr; }
    if (EC_POINT_mul(group.get(), point.get(), value.get(), nullptr, nullptr, nullptr) != 1) {
        return nullptr;
    }
    if (!point_to_octets(group.get(), point.get(), out)) { return nullptr; }
    return key_from_parts(out, value.get());
}

[[nodiscard]] bool export_public(const EVP_PKEY* key,
                                 std::array<std::uint8_t, kP256PublicBytes>& out) {
    std::size_t written = 0;
    if (EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, out.data(), out.size(),
                                        &written) != 1) {
        return false;
    }
    return written == out.size();
}

// The raw ECDH shared secret: the X coordinate, 32 bytes. NOT hashed here —
// RFC 8291 feeds it to HKDF as the IKM, and hashing it first would produce a
// value no other implementation computes.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> ecdh(const EVP_PKEY* ours,
                                                            const EVP_PKEY* theirs) {
    PkeyCtxPtr ctx{EVP_PKEY_CTX_new_from_pkey(nullptr, const_cast<EVP_PKEY*>(ours), nullptr)};
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1) { return std::nullopt; }
    if (EVP_PKEY_derive_set_peer(ctx.get(), const_cast<EVP_PKEY*>(theirs)) != 1) {
        return std::nullopt;
    }

    std::size_t length = 0;
    if (EVP_PKEY_derive(ctx.get(), nullptr, &length) != 1 || length == 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> secret(length);
    if (EVP_PKEY_derive(ctx.get(), secret.data(), &length) != 1) { return std::nullopt; }
    secret.resize(length);
    return secret;
}

// AES-128-GCM with the tag appended, no additional authenticated data.
//
// RFC 8188 puts everything that would otherwise be AAD — the salt, the record
// size, the key id — in the header, which is NOT authenticated by the tag. That
// is the RFC's choice and not ours: tampering with the header changes the key
// that is derived, so the decryption fails anyway.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> aes128gcm_seal(
    std::span<const std::uint8_t> key, std::span<const std::uint8_t> nonce,
    std::span<const std::uint8_t> plaintext) {
    CipherCtxPtr ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) { return std::nullopt; }
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1) {
        return std::nullopt;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1) {
        return std::nullopt;
    }
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> out(plaintext.size() + kGcmTagBytes);
    int written = 0;
    if (EVP_EncryptUpdate(ctx.get(), out.data(), &written, plaintext.data(),
                          static_cast<int>(plaintext.size())) != 1) {
        return std::nullopt;
    }
    int total = written;
    if (EVP_EncryptFinal_ex(ctx.get(), out.data() + total, &written) != 1) {
        return std::nullopt;
    }
    total += written;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kGcmTagBytes,
                            out.data() + total) != 1) {
        return std::nullopt;
    }
    out.resize(static_cast<std::size_t>(total) + kGcmTagBytes);
    return out;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> aes128gcm_open(
    std::span<const std::uint8_t> key, std::span<const std::uint8_t> nonce,
    std::span<const std::uint8_t> sealed) {
    if (sealed.size() < kGcmTagBytes) { return std::nullopt; }
    const std::size_t body = sealed.size() - kGcmTagBytes;

    CipherCtxPtr ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) { return std::nullopt; }
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1) {
        return std::nullopt;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1) {
        return std::nullopt;
    }
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> out(body == 0 ? 1 : body);
    int written = 0;
    if (body != 0 &&
        EVP_DecryptUpdate(ctx.get(), out.data(), &written, sealed.data(),
                          static_cast<int>(body)) != 1) {
        return std::nullopt;
    }
    int total = written;
    // The tag is const in the caller's buffer and OpenSSL's control takes a
    // non-const pointer, so it is copied rather than cast away.
    std::array<std::uint8_t, kGcmTagBytes> tag{};
    std::copy_n(sealed.data() + body, kGcmTagBytes, tag.begin());
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kGcmTagBytes, tag.data()) != 1) {
        return std::nullopt;
    }
    // The ONLY place the tag is checked. A decrypt that ignores this return is a
    // decrypt with no authentication at all.
    if (EVP_DecryptFinal_ex(ctx.get(), out.data() + total, &written) != 1) {
        return std::nullopt;
    }
    total += written;
    out.resize(static_cast<std::size_t>(total));
    return out;
}


void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_decimal(std::string& out, std::int64_t value) {
    std::array<char, 20> digits{};
    std::size_t length = 0;
    auto magnitude = static_cast<std::uint64_t>(value < 0 ? 0 : value);
    do {
        digits[length] = static_cast<char>('0' + (magnitude % 10U));
        ++length;
        magnitude /= 10U;
    } while (magnitude != 0U && length < digits.size());
    for (std::size_t i = length; i > 0; --i) { out.push_back(digits[i - 1]); }
}

// ES256 over an already-built JWS signing input.
//
// EVP_DigestSign rather than ECDSA_do_sign: the latter is deprecated in OpenSSL
// 3.0, and this form hashes and signs in one pass with no intermediate digest to
// mishandle.
//
// JWS wants r || s as fixed 32-byte big-endian integers; OpenSSL produces DER.
// The conversion is not cosmetic: BN_bn2binpad left-pads, and a DER integer whose
// leading byte happens to be below 0x80 is one byte shorter than 32. Copying the
// DER bytes through would produce a signature that verifies nowhere, roughly one
// time in 256.
[[nodiscard]] std::optional<std::array<std::uint8_t, kEcdsaSignatureBytes>> es256_sign(
    EVP_PKEY* key, std::string_view signing_input) {
    MdCtxPtr ctx{EVP_MD_CTX_new()};
    if (!ctx) { return std::nullopt; }
    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key) != 1) {
        return std::nullopt;
    }

    const auto* message = reinterpret_cast<const unsigned char*>(signing_input.data());
    std::size_t der_length = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &der_length, message, signing_input.size()) != 1) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> der(der_length);
    if (EVP_DigestSign(ctx.get(), der.data(), &der_length, message, signing_input.size()) != 1) {
        return std::nullopt;
    }
    der.resize(der_length);

    const unsigned char* cursor = der.data();
    EcdsaSigPtr signature{d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der.size()))};
    if (!signature) { return std::nullopt; }

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(signature.get(), &r, &s);
    if (r == nullptr || s == nullptr) { return std::nullopt; }

    std::array<std::uint8_t, kEcdsaSignatureBytes> out{};
    if (BN_bn2binpad(r, out.data(), kP256PrivateBytes) != kP256PrivateBytes) {
        return std::nullopt;
    }
    if (BN_bn2binpad(s, out.data() + kP256PrivateBytes, kP256PrivateBytes) !=
        kP256PrivateBytes) {
        return std::nullopt;
    }
    return out;
}

}  // namespace

std::vector<std::uint8_t> hkdf_sha256(std::span<const std::uint8_t> salt,
                                      std::span<const std::uint8_t> ikm,
                                      std::span<const std::uint8_t> info,
                                      std::size_t length) {
    // Extract. HMAC with the SALT as the key and the input keying material as the
    // message — that way round, which is the half of RFC 5869 that is easiest to
    // get backwards and produces a value that is self-consistent and interoperates
    // with nothing.
    const crypto::Digest256 prk = crypto::hmac_sha256(salt, ikm);

    // Expand. Web Push never asks for more than 32 bytes, so one block always
    // suffices — but the loop is written anyway, because a function that silently
    // truncates at 32 is one somebody reuses at 48.
    std::vector<std::uint8_t> out;
    out.reserve(length);
    std::vector<std::uint8_t> block;
    std::uint8_t counter = 1;
    while (out.size() < length) {
        std::vector<std::uint8_t> input;
        input.reserve(block.size() + info.size() + 1);
        input.insert(input.end(), block.begin(), block.end());
        input.insert(input.end(), info.begin(), info.end());
        input.push_back(counter);

        const crypto::Digest256 mac = crypto::hmac_sha256(prk, input);
        block.assign(mac.begin(), mac.end());
        const std::size_t take = std::min(block.size(), length - out.size());
        out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(take));
        ++counter;
    }
    return out;
}

std::string push_origin(std::string_view endpoint) {
    constexpr std::string_view kHttps = "https://";
    if (endpoint.size() <= kHttps.size() ||
        endpoint.compare(0, kHttps.size(), kHttps) != 0) {
        return {};
    }
    const std::string_view rest = endpoint.substr(kHttps.size());
    const std::size_t end = rest.find_first_of("/?#");
    const std::string_view authority = end == std::string_view::npos ? rest : rest.substr(0, end);
    if (authority.empty()) { return {}; }
    // The authority verbatim, PORT INCLUDED. A token signed over the host alone
    // is refused by a service on a non-default port, and it fails as an opaque
    // 401 rather than as anything that names the cause.
    return std::string{kHttps}.append(authority);
}

Result<VapidKey> generate_vapid_key() {
    PkeyPtr key{EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", kCurveName)};
    if (!key) { return fail(ErrorCode::Internal, kPushField); }

    VapidKey out{};
    if (!export_public(key.get(), out.public_key)) {
        return fail(ErrorCode::Internal, kPushField);
    }

    BIGNUM* raw = nullptr;
    if (EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_PRIV_KEY, &raw) != 1) {
        return fail(ErrorCode::Internal, kPushField);
    }
    const BignumPtr scalar{raw};
    // Left-padded to exactly 32 bytes. A scalar whose leading byte happens to be
    // zero is 31 bytes in its minimal encoding, and a key stored that way reads
    // back as a different key.
    if (BN_bn2binpad(scalar.get(), out.private_key.data(), kP256PrivateBytes) !=
        kP256PrivateBytes) {
        return fail(ErrorCode::Internal, kPushField);
    }
    return out;
}

std::string vapid_public_key_for_browser(const VapidKey& key) {
    return crypto::base64url_encode(key.public_key);
}

Result<std::string> vapid_token(const VapidKey& key, const VapidIdentity& identity,
                                std::string_view audience, db::TimeMs now) {
    if (audience.empty() || identity.subject.empty()) {
        return fail(ErrorCode::ValidationFailed, kPushField);
    }

    // Built by hand rather than through a JSON writer. The claim set is three
    // fixed fields, the audience is an origin this file computed, and the subject
    // is checked below — so there is nothing here a writer would escape, and a
    // dependency on one would put the JWT's exact bytes at the mercy of a change
    // to unrelated formatting.
    //
    // The subject reaches the JSON, so a quote or a backslash in it would forge a
    // claim. It is refused rather than escaped: `mailto:` and `https:` addresses
    // contain neither, and a subject that does is a configuration mistake.
    for (const char c : identity.subject) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20U) {
            return fail(ErrorCode::ValidationFailed, kPushField);
        }
    }
    for (const char c : audience) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20U) {
            return fail(ErrorCode::ValidationFailed, kPushField);
        }
    }

    constexpr std::string_view kHeaderJson = R"({"typ":"JWT","alg":"ES256"})";
    const std::int64_t expires_at =
        (now.time_since_epoch().count() / 1000) + kVapidLifetime.count();

    std::string claims;
    claims.reserve(64 + audience.size() + identity.subject.size());
    claims.append(R"({"aud":")").append(audience).append(R"(","exp":)");
    append_decimal(claims, expires_at);
    claims.append(R"(,"sub":")").append(identity.subject).append("\"}");

    std::string signing_input = crypto::base64url_encode(bytes_of(kHeaderJson));
    signing_input.push_back('.');
    signing_input.append(crypto::base64url_encode(bytes_of(claims)));

    std::array<std::uint8_t, kP256PublicBytes> derived{};
    const PkeyPtr signing_key = private_key_from_scalar(key.private_key, derived);
    if (!signing_key) { return fail(ErrorCode::ValidationFailed, kPushField); }

    const std::optional<std::array<std::uint8_t, kEcdsaSignatureBytes>> signature =
        es256_sign(signing_key.get(), signing_input);
    if (!signature.has_value()) { return fail(ErrorCode::Internal, kPushField); }

    signing_input.push_back('.');
    signing_input.append(crypto::base64url_encode(*signature));
    return signing_input;
}

Result<std::string> vapid_authorization(const VapidKey& key, const VapidIdentity& identity,
                                        std::string_view endpoint, db::TimeMs now) {
    const std::string audience = push_origin(endpoint);
    // An endpoint that is not an absolute https URL cannot produce an audience,
    // and a token with the wrong one fails as an opaque 401 from the push
    // service. Refusing here says which of the two it was.
    if (audience.empty()) { return fail(ErrorCode::ValidationFailed, kPushField); }

    Result<std::string> token = vapid_token(key, identity, audience, now);
    if (!token) { return token.error(); }

    std::string header = "vapid t=";
    header.append(std::move(token).value());
    header.append(", k=");
    header.append(crypto::base64url_encode(key.public_key));
    return header;
}

Result<std::vector<std::uint8_t>> encrypt_payload(const PushSubscription& subscription,
                                                  std::string_view plaintext) {
    if (plaintext.size() > kMaxPlaintextBytes) {
        return fail(ErrorCode::PayloadTooLarge, kPayloadField);
    }

    const PkeyPtr peer = public_key_from_point(subscription.p256dh);
    // Not on the curve, the point at infinity, or of the wrong order. Any of the
    // three is either corruption or an invalid-curve attack, and multiplying our
    // ephemeral scalar by it leaks the scalar a few bits at a time.
    if (!peer) { return fail(ErrorCode::ValidationFailed, kSubscriptionField); }

    // FRESH PER MESSAGE, both of them, and generated here rather than passed in.
    // AES-GCM under a repeated key and nonce leaks the XOR of the two plaintexts
    // and the authentication key with it.
    const PkeyPtr ephemeral{EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", kCurveName)};
    if (!ephemeral) { return fail(ErrorCode::Internal, kPushField); }
    std::array<std::uint8_t, kP256PublicBytes> as_public{};
    if (!export_public(ephemeral.get(), as_public)) {
        return fail(ErrorCode::Internal, kPushField);
    }
    const std::array<std::uint8_t, kSaltBytes> salt = crypto::random_array<kSaltBytes>();

    const std::optional<std::vector<std::uint8_t>> shared = ecdh(ephemeral.get(), peer.get());
    if (!shared.has_value()) { return fail(ErrorCode::Internal, kPushField); }

    // RFC 8291 §3.4. The two public keys are in a FIXED order — the user agent's
    // first — and swapping them produces a key the browser will not derive.
    std::vector<std::uint8_t> key_info;
    const std::span<const std::uint8_t> web_push_label = label_of(kWebPushInfo);
    key_info.reserve(web_push_label.size() + (2 * kP256PublicBytes));
    key_info.insert(key_info.end(), web_push_label.begin(), web_push_label.end());
    key_info.insert(key_info.end(), subscription.p256dh.begin(), subscription.p256dh.end());
    key_info.insert(key_info.end(), as_public.begin(), as_public.end());

    const std::vector<std::uint8_t> ikm =
        hkdf_sha256(subscription.auth, *shared, key_info, 32);
    const std::vector<std::uint8_t> cek =
        hkdf_sha256(salt, ikm, label_of(kCekInfo), kContentKeyBytes);
    const std::vector<std::uint8_t> nonce =
        hkdf_sha256(salt, ikm, label_of(kNonceInfo), kNonceBytes);

    std::vector<std::uint8_t> padded;
    padded.reserve(plaintext.size() + 1);
    padded.insert(padded.end(), plaintext.begin(), plaintext.end());
    // RFC 8188 §2. Without it the browser strips the last byte of the message.
    padded.push_back(kLastRecordDelimiter);

    const std::optional<std::vector<std::uint8_t>> sealed = aes128gcm_seal(cek, nonce, padded);
    if (!sealed.has_value()) { return fail(ErrorCode::Internal, kPushField); }

    std::vector<std::uint8_t> body;
    body.reserve(kHeaderBytes + sealed->size());
    body.insert(body.end(), salt.begin(), salt.end());
    append_be32(body, kRecordSize);
    body.push_back(static_cast<std::uint8_t>(kP256PublicBytes));
    body.insert(body.end(), as_public.begin(), as_public.end());
    body.insert(body.end(), sealed->begin(), sealed->end());
    return body;
}

Result<std::string> decrypt_payload(std::span<const std::uint8_t> ua_private,
                                    std::span<const std::uint8_t> auth_secret,
                                    std::span<const std::uint8_t> body) {
    if (body.size() <= kHeaderBytes || auth_secret.size() != kAuthSecretBytes) {
        return fail(ErrorCode::ValidationFailed, kPayloadField);
    }
    const std::span<const std::uint8_t> salt = body.subspan(0, kSaltBytes);
    const std::size_t id_length = body[kSaltBytes + 4];
    if (id_length != kP256PublicBytes) { return fail(ErrorCode::ValidationFailed, kPayloadField); }
    const std::span<const std::uint8_t> as_public = body.subspan(kSaltBytes + 5, id_length);
    const std::span<const std::uint8_t> sealed = body.subspan(kHeaderBytes);

    std::array<std::uint8_t, kP256PublicBytes> ua_public{};
    const PkeyPtr ours = private_key_from_scalar(ua_private, ua_public);
    const PkeyPtr theirs = public_key_from_point(as_public);
    if (!ours || !theirs) { return fail(ErrorCode::ValidationFailed, kPayloadField); }

    const std::optional<std::vector<std::uint8_t>> shared = ecdh(ours.get(), theirs.get());
    if (!shared.has_value()) { return fail(ErrorCode::Internal, kPushField); }

    std::vector<std::uint8_t> key_info;
    const std::span<const std::uint8_t> web_push_label = label_of(kWebPushInfo);
    key_info.reserve(web_push_label.size() + (2 * kP256PublicBytes));
    key_info.insert(key_info.end(), web_push_label.begin(), web_push_label.end());
    key_info.insert(key_info.end(), ua_public.begin(), ua_public.end());
    key_info.insert(key_info.end(), as_public.begin(), as_public.end());

    const std::vector<std::uint8_t> ikm = hkdf_sha256(auth_secret, *shared, key_info, 32);
    const std::vector<std::uint8_t> cek =
        hkdf_sha256(salt, ikm, label_of(kCekInfo), kContentKeyBytes);
    const std::vector<std::uint8_t> nonce =
        hkdf_sha256(salt, ikm, label_of(kNonceInfo), kNonceBytes);

    std::optional<std::vector<std::uint8_t>> opened = aes128gcm_open(cek, nonce, sealed);
    // A failed tag check. Not distinguished from a malformed body on purpose:
    // telling the two apart is an oracle, and neither is recoverable.
    if (!opened.has_value()) { return fail(ErrorCode::ValidationFailed, kPayloadField); }

    // Strip the record delimiter and any padding after it.
    std::size_t end = opened->size();
    while (end > 0 && (*opened)[end - 1] == 0) { --end; }
    if (end == 0 || (*opened)[end - 1] != kLastRecordDelimiter) {
        return fail(ErrorCode::ValidationFailed, kPayloadField);
    }
    --end;
    return std::string{reinterpret_cast<const char*>(opened->data()), end};
}

}  // namespace anvil::notifications
