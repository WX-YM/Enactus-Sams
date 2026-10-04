#include "anvil/auth/prehash.h"

#include <openssl/crypto.h>

#include <algorithm>
#include <charconv>
#include <stdexcept>

#include "anvil/auth/password.h"
#include "anvil/crypto/base64url.h"
#include "anvil/crypto/constant_time.h"
#include "anvil/crypto/digest.h"
#include "anvil/crypto/errors.h"
#include "anvil/crypto/random.h"
#include "anvil/i18n/normalize.h"

namespace anvil::auth {
namespace {

// Everything before the client-stage memory cost. The record names its own
// version, algorithm and Argon2 version up front so that a v2 record is refused
// by a v1 reader rather than half-parsed by it.
constexpr std::string_view kRecordPrefix = "$anvil-prehash$v=1$argon2id$v=19$m=";
constexpr std::string_view kKeyedStage = "$hmac-sha256$k=";
constexpr std::string_view kArgon2Stage = "$argon2id$v=19$m=";
constexpr std::string_view kLegacyPrefix = "$argon2id$v=19$m=";

// Domain separation. The same pepper or salt key used for anything else can
// never produce a value that collides with one of these.
constexpr std::string_view kStageContext = "anvil.prehash.stage.v1";
constexpr std::string_view kSaltContext = "anvil.prehash.salt.v1";

// Unpadded standard base64 of 16 and 32 bytes.
constexpr std::size_t kSaltChars = 22;
constexpr std::size_t kDigestChars = 43;

// The floor plain mode enforces (auth/password.cc). The client stage is where a
// guess's cost now lives, so it is held to the same line.
constexpr std::uint32_t kMinClientMemoryKib = 8192;
constexpr std::uint32_t kMinClientIterations = 2;

// --- standard base64, unpadded ----------------------------------------------
//
// Inside the record only. PHC strings — including the nested one libargon2
// writes for the Argon2 stage — use the standard alphabet, and one record
// mixing two alphabets is one more thing for a migration script to get wrong.
// The wire uses base64url (crypto/base64url.h), because it travels in JSON
// and form bodies.

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void append_b64(std::string& out, std::span<const std::uint8_t> bytes) {
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t group = (std::uint32_t{bytes[i]} << 16) |
                                    (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        out.push_back(kAlphabet[(group >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 6) & 0x3FU]);
        out.push_back(kAlphabet[group & 0x3FU]);
    }
    const std::size_t tail = bytes.size() - i;
    if (tail == 1) {
        const std::uint32_t group = std::uint32_t{bytes[i]} << 16;
        out.push_back(kAlphabet[(group >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12) & 0x3FU]);
    } else if (tail == 2) {
        const std::uint32_t group = (std::uint32_t{bytes[i]} << 16) |
                                    (std::uint32_t{bytes[i + 1]} << 8);
        out.push_back(kAlphabet[(group >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 12) & 0x3FU]);
        out.push_back(kAlphabet[(group >> 6) & 0x3FU]);
    }
}

[[nodiscard]] constexpr std::uint8_t b64_digit(char c) noexcept {
    if (c >= 'A' && c <= 'Z') { return static_cast<std::uint8_t>(c - 'A'); }
    if (c >= 'a' && c <= 'z') { return static_cast<std::uint8_t>(c - 'a' + 26); }
    if (c >= '0' && c <= '9') { return static_cast<std::uint8_t>(c - '0' + 52); }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return 0xFFU;
}

// Decodes exactly out.size() bytes from exactly the unpadded length that
// encodes them. Non-canonical trailing bits are refused: two spellings of one
// value in a stored record means a byte-level comparison of records — which a
// conditional write-back performs — can disagree with a value-level one.
[[nodiscard]] bool decode_b64_exact(std::string_view text, std::span<std::uint8_t> out) noexcept {
    const std::size_t expected = (out.size() * 4 + 2) / 3;
    if (text.size() != expected) { return false; }

    std::uint32_t accumulator = 0;
    int bits = 0;
    std::size_t written = 0;
    for (const char c : text) {
        const std::uint8_t digit = b64_digit(c);
        if (digit == 0xFFU) { return false; }
        accumulator = (accumulator << 6) | digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[written++] = static_cast<std::uint8_t>((accumulator >> bits) & 0xFFU);
        }
    }
    return written == out.size() && (accumulator & ((1U << bits) - 1U)) == 0;
}

// --- a linear cursor over a record ---------------------------------------------
//
// Hand-written, never std::regex (CLAUDE.md §5). Every step either consumes
// exactly what it expects or fails the whole parse.

struct Cursor final {
    std::string_view rest;

    [[nodiscard]] bool literal(std::string_view expected) noexcept {
        if (rest.substr(0, expected.size()) != expected) { return false; }
        rest.remove_prefix(expected.size());
        return true;
    }

    // Up to ten digits, no sign, no leading zero, and never zero: every cost
    // parameter here has a floor of one, and a leading zero is a second
    // spelling of the same number.
    [[nodiscard]] std::optional<std::uint32_t> number() noexcept {
        std::size_t end = 0;
        while (end < rest.size() && end < 11 && rest[end] >= '0' && rest[end] <= '9') { ++end; }
        if (end == 0 || end > 10 || rest[0] == '0') { return std::nullopt; }
        std::uint32_t value = 0;
        const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + end, value);
        if (ec != std::errc{} || ptr != rest.data() + end) { return std::nullopt; }
        rest.remove_prefix(end);
        return value;
    }

    // "<m>,t=<t>,p=<p>" — the caller has already consumed "m=".
    [[nodiscard]] std::optional<crypto::Argon2Params> params() noexcept {
        const std::optional<std::uint32_t> memory = number();
        if (!memory.has_value() || !literal(",t=")) { return std::nullopt; }
        const std::optional<std::uint32_t> iterations = number();
        if (!iterations.has_value() || !literal(",p=")) { return std::nullopt; }
        const std::optional<std::uint32_t> parallelism = number();
        if (!parallelism.has_value()) { return std::nullopt; }
        return crypto::Argon2Params{
            .memory_kib = *memory, .iterations = *iterations, .parallelism = *parallelism};
    }

    [[nodiscard]] std::string_view take(std::size_t count) noexcept {
        if (rest.size() < count) { return {}; }
        const std::string_view taken = rest.substr(0, count);
        rest.remove_prefix(count);
        return taken;
    }
};

[[nodiscard]] constexpr bool is_key_id_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

[[nodiscard]] bool valid_key_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > kPrehashMaxKeyIdChars) { return false; }
    for (const char c : id) {
        if (!is_key_id_char(c)) { return false; }
    }
    return true;
}

enum class StageKind : std::uint8_t { KeyedDigest, Argon2 };

struct ParsedRecord final {
    crypto::Argon2Params client;
    PrehashSalt          salt;
    StageKind            stage;
    // KeyedDigest only.
    std::string_view     key_id;
    crypto::Digest256    tag;
    // Argon2 only: libargon2's own encoded string, leading `$` included.
    std::string_view     nested;
};

[[nodiscard]] std::optional<ParsedRecord> parse_record(std::string_view record) noexcept {
    Cursor cursor{record};
    if (!cursor.literal(kRecordPrefix)) { return std::nullopt; }

    ParsedRecord parsed{};
    const std::optional<crypto::Argon2Params> client = cursor.params();
    if (!client.has_value() || !cursor.literal("$")) { return std::nullopt; }
    parsed.client = *client;

    if (!decode_b64_exact(cursor.take(kSaltChars), parsed.salt)) { return std::nullopt; }

    if (cursor.literal(kKeyedStage)) {
        const std::size_t end = cursor.rest.find('$');
        if (end == std::string_view::npos) { return std::nullopt; }
        parsed.key_id = cursor.take(end);
        if (!valid_key_id(parsed.key_id) || !cursor.literal("$")) { return std::nullopt; }
        if (!decode_b64_exact(cursor.take(kDigestChars), parsed.tag)) { return std::nullopt; }
        if (!cursor.rest.empty()) { return std::nullopt; }
        parsed.stage = StageKind::KeyedDigest;
        return parsed;
    }

    // The Argon2 stage is libargon2's own string and libargon2 verifies it; the
    // prefix and the parameter parse are checked here so that a record which
    // could never verify is Malformed at parse time rather than at verify time.
    if (cursor.rest.substr(0, kArgon2Stage.size()) == kArgon2Stage &&
        crypto::argon2id_parse_params(cursor.rest).has_value()) {
        parsed.nested = cursor.rest;
        parsed.stage = StageKind::Argon2;
        return parsed;
    }
    return std::nullopt;
}

[[nodiscard]] std::span<const std::uint8_t> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

void append_number(std::string& out, std::uint32_t value) {
    // to_chars: no locale, no allocation. A locale that groups digits would
    // write a record no parser here reads back.
    std::array<char, 10> digits{};
    const auto [ptr, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    out.append(digits.data(), ptr);
}

void append_params(std::string& out, const crypto::Argon2Params& params) {
    append_number(out, params.memory_kib);
    out.append(",t=");
    append_number(out, params.iterations);
    out.append(",p=");
    append_number(out, params.parallelism);
}

[[nodiscard]] crypto::Digest256 keyed_tag(const crypto::Key256& pepper, const PrehashKey& k) {
    std::array<std::uint8_t, kStageContext.size() + 1 + kPrehashKeyBytes> message{};
    std::copy(kStageContext.begin(), kStageContext.end(), message.begin());
    message[kStageContext.size()] = 0x00;
    std::copy(k.span().begin(), k.span().end(), message.begin() + kStageContext.size() + 1);

    const crypto::Digest256 tag = crypto::hmac_sha256(pepper.span(), message);
    // The message holds `k`, which is a working credential.
    OPENSSL_cleanse(message.data(), message.size());
    return tag;
}

[[nodiscard]] bool params_at_least(const crypto::Argon2Params& have,
                                   const crypto::Argon2Params& want) noexcept {
    return have.memory_kib >= want.memory_kib && have.iterations >= want.iterations &&
           have.parallelism >= want.parallelism;
}

void require_argon2_valid(const crypto::Argon2Params& params, const char* what) {
    // libargon2's own floor: at least eight blocks per lane.
    if (params.iterations < 1 || params.parallelism < 1 ||
        params.memory_kib / 8 < params.parallelism) {
        throw std::invalid_argument{std::string{what} + " Argon2 parameters are not valid"};
    }
}

}  // namespace

// --- construction ------------------------------------------------------------

PrehashHasher::PrehashHasher(PrehashPolicy policy)
    : policy_{std::move(policy)}, dummy_key_{crypto::random_secret<kPrehashKeyBytes>()} {
    const crypto::Argon2Params& client = policy_.client;
    require_argon2_valid(client, "client-stage");
    if (client.memory_kib < kMinClientMemoryKib || client.iterations < kMinClientIterations) {
        throw std::invalid_argument{"client-stage Argon2 parameters below policy"};
    }

    std::vector<std::string_view> ids;
    ids.reserve(policy_.retired_peppers.size() + 1);
    if (const auto* keyed = std::get_if<PrehashKeyedDigestStage>(&policy_.server)) {
        ids.push_back(keyed->key_id);
    } else {
        require_argon2_valid(std::get<PrehashArgon2Stage>(policy_.server).params, "server-stage");
    }
    for (const PrehashRetiredPepper& retired : policy_.retired_peppers) {
        ids.push_back(retired.key_id);
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (!valid_key_id(ids[i])) {
            throw std::invalid_argument{"a pepper id must be 1-16 characters of [a-z0-9]"};
        }
        for (std::size_t j = 0; j < i; ++j) {
            // Two peppers under one id would make which one verifies a record
            // depend on the order of a configuration list.
            if (ids[i] == ids[j]) { throw std::invalid_argument{"pepper ids must be distinct"}; }
        }
    }

    // Built once, single-threaded, so the equaliser costs nothing extra on the
    // login path and no lazy initialisation races between concurrent logins.
    dummy_record_ = enroll(dummy_key_, PrehashSaltAnswer{crypto::random_array<kPrehashSaltBytes>(),
                                                         policy_.client});
}

// --- salts ---------------------------------------------------------------------

PrehashSalt PrehashHasher::derive_salt(std::uint8_t kind,
                                       std::string_view canonical_identifier) const {
    std::string message;
    message.reserve(kSaltContext.size() + 2 + canonical_identifier.size());
    message.append(kSaltContext);
    message.push_back('\0');
    message.push_back(static_cast<char>(kind));
    message.append(canonical_identifier);

    const crypto::Digest256 full = crypto::hmac_sha256(policy_.salt_key.span(), message);
    PrehashSalt salt{};
    std::copy_n(full.begin(), salt.size(), salt.begin());
    return salt;
}

PrehashSaltAnswer PrehashHasher::answer_for(std::optional<std::string_view> stored,
                                            std::uint8_t kind,
                                            std::string_view canonical_identifier) const {
    if (stored.has_value()) {
        if (const std::optional<ParsedRecord> parsed = parse_record(*stored)) {
            return PrehashSaltAnswer{parsed->salt, parsed->client};
        }
    }
    return PrehashSaltAnswer{derive_salt(kind, canonical_identifier), policy_.client};
}

// --- enrolment -----------------------------------------------------------------

std::string PrehashHasher::enroll(const PrehashKey& k, const PrehashSaltAnswer& client) const {
    std::string record;
    record.reserve(160);
    record.append(kRecordPrefix);
    append_params(record, client.params);
    record.push_back('$');
    append_b64(record, client.salt);

    if (const auto* keyed = std::get_if<PrehashKeyedDigestStage>(&policy_.server)) {
        record.append(kKeyedStage);
        record.append(keyed->key_id);
        record.push_back('$');
        append_b64(record, keyed_tag(keyed->key, k));
        return record;
    }

    const PrehashArgon2Stage& stage = std::get<PrehashArgon2Stage>(policy_.server);
    const std::array<std::uint8_t, kPrehashSaltBytes> salt2 =
        crypto::random_array<kPrehashSaltBytes>();
    record.append(crypto::argon2id_hash_encoded(stage.params, k.span(), salt2, kPrehashKeyBytes));
    return record;
}

std::string PrehashHasher::enroll_plaintext(std::string_view password,
                                            const PrehashSalt& salt) const {
    if (password.size() > kMaxPasswordBytes) {
        throw std::invalid_argument{"password exceeds the hard byte cap"};
    }

    // NFC and nothing else — byte-for-byte what PasswordHasher does and what
    // the client does, or the same passphrase typed on two keyboards derives
    // two keys and the person cannot sign in (docs/03-i18n-utf8.md §6).
    std::optional<std::string> normalized = i18n::normalize(password, i18n::NormalizeMode::Nfc);
    if (!normalized.has_value()) {
        throw crypto::CryptoError{"password normalisation failed"};
    }

    PrehashKey k;
    try {
        crypto::argon2_hash_raw(crypto::Argon2Type::Argon2id, crypto::kArgon2Version13,
                                policy_.client, as_bytes(*normalized), salt, {}, {},
                                k.mutable_span());
    } catch (...) {
        OPENSSL_cleanse(normalized->data(), normalized->size());
        throw;
    }
    OPENSSL_cleanse(normalized->data(), normalized->size());
    return enroll(k, PrehashSaltAnswer{salt, policy_.client});
}

// --- verification --------------------------------------------------------------

PrehashVerification PrehashHasher::verify(std::string_view stored, const PrehashKey& k) const {
    const std::optional<ParsedRecord> parsed = parse_record(stored);
    if (!parsed.has_value()) { return {{}, crypto::VerifyOutcome::Malformed}; }

    crypto::VerifyOutcome outcome = crypto::VerifyOutcome::Malformed;
    if (parsed->stage == StageKind::KeyedDigest) {
        const crypto::Key256* pepper = nullptr;
        const auto* current = std::get_if<PrehashKeyedDigestStage>(&policy_.server);
        if (current != nullptr && current->key_id == parsed->key_id) {
            pepper = &current->key;
        }
        for (const PrehashRetiredPepper& retired : policy_.retired_peppers) {
            if (pepper == nullptr && retired.key_id == parsed->key_id) { pepper = &retired.key; }
        }
        // A pepper this process does not hold is a configuration fault — a
        // rotation that dropped a key still in use — and is Malformed so that it
        // is alerted on rather than reading as a wave of wrong passwords.
        if (pepper == nullptr) { return {{}, crypto::VerifyOutcome::Malformed}; }

        const crypto::Digest256 computed = keyed_tag(*pepper, k);
        outcome = crypto::secure_equal(computed, parsed->tag) ? crypto::VerifyOutcome::Match
                                                              : crypto::VerifyOutcome::Mismatch;
    } else {
        outcome = crypto::argon2id_verify_encoded(parsed->nested, k.span());
    }

    std::string upgraded;
    if (outcome == crypto::VerifyOutcome::Match && needs_rehash(stored)) {
        // Only the stage is rebuilt. The client salt and parameters stay, because
        // they are what the client will hash with next time and the server cannot
        // change them without the password.
        upgraded = enroll(k, PrehashSaltAnswer{parsed->salt, parsed->client});
    }
    return {std::move(upgraded), outcome};
}

void PrehashHasher::consume_dummy_time() const noexcept {
    // A real parse and a real stage over a record of the current policy, so the
    // elapsed time and memory traffic match a verify against a real account.
    //
    // The record is known-good and never needs a rehash, so nothing here enrols;
    // what can still throw is an allocation, and an exception escaping a
    // noexcept function on a pool thread is std::terminate for the process.
    crypto::VerifyOutcome outcome = crypto::VerifyOutcome::Mismatch;
    try {
        outcome = verify(dummy_record_, dummy_key_).outcome;
    } catch (...) {
        outcome = crypto::VerifyOutcome::Malformed;
    }
    static volatile crypto::VerifyOutcome sink = crypto::VerifyOutcome::Mismatch;
    sink = outcome;
    (void)sink;
}

bool PrehashHasher::needs_rehash(std::string_view stored) const noexcept {
    const std::optional<ParsedRecord> parsed = parse_record(stored);
    if (!parsed.has_value()) { return true; }

    if (const auto* keyed = std::get_if<PrehashKeyedDigestStage>(&policy_.server)) {
        return parsed->stage != StageKind::KeyedDigest || parsed->key_id != keyed->key_id;
    }
    if (parsed->stage != StageKind::Argon2) { return true; }
    const std::optional<crypto::Argon2Params> have = crypto::argon2id_parse_params(parsed->nested);
    return !have.has_value() ||
           !params_at_least(*have, std::get<PrehashArgon2Stage>(policy_.server).params);
}

// --- migration -----------------------------------------------------------------

Result<std::string> PrehashHasher::wrap_legacy(std::string_view legacy) const {
    Cursor cursor{legacy};
    if (!cursor.literal(kLegacyPrefix)) { return fail(ErrorCode::ValidationFailed); }
    const std::optional<crypto::Argon2Params> params = cursor.params();
    if (!params.has_value() || !cursor.literal("$")) { return fail(ErrorCode::ValidationFailed); }

    PrehashSalt salt{};
    if (!decode_b64_exact(cursor.take(kSaltChars), salt) || !cursor.literal("$")) {
        return fail(ErrorCode::ValidationFailed);
    }
    PrehashKey k;
    if (!decode_b64_exact(cursor.take(kDigestChars), k.mutable_span()) || !cursor.rest.empty()) {
        return fail(ErrorCode::ValidationFailed);
    }
    return enroll(k, PrehashSaltAnswer{salt, *params});
}

// --- the wire --------------------------------------------------------------------

std::optional<PrehashKey> decode_prehash_credential(std::string_view wire) noexcept {
    if (wire.size() != kPrehashCredentialChars) { return std::nullopt; }
    PrehashKey k;
    const std::optional<std::size_t> written = crypto::base64url_decode_into(wire, k.mutable_span());
    if (!written.has_value() || *written != kPrehashKeyBytes) { return std::nullopt; }
    return k;
}

void append_prehash_salt_answer(std::string& body, const PrehashSaltAnswer& answer) {
    body.append(R"({"algorithm":"argon2id","version":19,"salt":")");
    body.append(crypto::base64url_encode(answer.salt));
    body.append(R"(","memory_kib":)");
    append_number(body, answer.params.memory_kib);
    body.append(R"(,"iterations":)");
    append_number(body, answer.params.iterations);
    body.append(R"(,"parallelism":)");
    append_number(body, answer.params.parallelism);
    body.append(R"(,"hash_bytes":32})");
}

}  // namespace anvil::auth
