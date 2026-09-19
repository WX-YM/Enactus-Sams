#include "anvil/analytics/sessions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

#include "anvil/crypto/digest.h"
#include "anvil/crypto/fast_hash.h"

namespace anvil::analytics {
namespace {

// Held behind a shared_ptr so a reader never sees a half-installed key, and read
// on the ingest path with no lock at all (ENGINEERING_RULES.md §4).
std::atomic<std::shared_ptr<const crypto::Key256>> g_pepper;
std::mutex                                         g_pepper_mutex;

// Mapped IPv4 lives in the last four bytes of the sixteen, behind the
// ::ffff:0:0/96 prefix. The same test http/client_address.cc uses, so the two
// cannot come to disagree about what a v4 address is.
[[nodiscard]] bool is_mapped_v4(const http::PackedAddress& address) noexcept {
    static constexpr std::array<std::uint8_t, 12> kPrefix{0, 0, 0, 0,    0,    0,
                                                          0, 0, 0, 0, 0xFF, 0xFF};
    return std::equal(kPrefix.begin(), kPrefix.end(), address.begin());
}

}  // namespace

DayNumber day_of(db::TimeMs at) noexcept {
    // Floor division, so a pre-epoch instant does not land a day early. The
    // arithmetic matters because the day is part of the DIGEST: an off-by-one
    // splits one visitor across two ids and reports the split as growth.
    const std::int64_t ms = at.time_since_epoch().count();
    constexpr std::int64_t kMsPerDay = 86'400'000;
    const std::int64_t days = ms >= 0 ? ms / kMsPerDay : ((ms - kMsPerDay + 1) / kMsPerDay);
    return static_cast<DayNumber>(days);
}

void install_visitor_pepper(crypto::Key256 pepper) {
    const std::lock_guard<std::mutex> held{g_pepper_mutex};
    g_pepper.store(std::make_shared<const crypto::Key256>(std::move(pepper)),
                   std::memory_order_release);
}

bool visitor_pepper_installed() noexcept {
    return static_cast<bool>(g_pepper.load(std::memory_order_acquire));
}

void clear_visitor_pepper() noexcept {
    const std::lock_guard<std::mutex> held{g_pepper_mutex};
    // Dropping the last shared_ptr runs SecretBuffer's destructor, which is what
    // calls OPENSSL_cleanse. A reader holding its own copy keeps the key alive
    // until it returns, which is exactly what the shared_ptr is for.
    g_pepper.store(std::shared_ptr<const crypto::Key256>{}, std::memory_order_release);
}

http::PackedAddress coarsen_for_visitor(const http::PackedAddress& address) noexcept {
    // v4 is NOT coarsened. Coarsening it merges everyone behind one NAT into a
    // single visitor, which deflates the number this exists to produce — the
    // opposite error from the one /64 fixes for v6, and the larger of the two.
    if (is_mapped_v4(address)) { return address; }

    http::PackedAddress coarse = address;
    // /64: keep the routing prefix, zero the interface identifier. Privacy
    // addressing rotates that identifier several times a day, and hashing it
    // mints a new visitor for the same person each time.
    for (std::size_t i = 8; i < coarse.size(); ++i) { coarse[i] = 0; }
    return coarse;
}

bool session_is_sampled_in(const VisitorId& session, std::uint32_t denominator) noexcept {
    // 1 keeps everything, which is the correct default: sampling is a pressure
    // valve, not a policy.
    if (denominator <= 1) { return true; }
    // xxh3 rather than a keyed digest, deliberately: the only requirement is
    // that different sessions land differently, and there is nothing here an
    // attacker gains by predicting — a visitor who could force themselves into
    // the kept fraction has arranged to be measured.
    const crypto::FastDigest digest = crypto::xxh3_64(std::span<const std::uint8_t>{session});
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < digest.size(); ++i) {
        value |= static_cast<std::uint64_t>(digest[i]) << (i * 8);
    }
    // Deterministic PER SESSION, so a session is kept whole or dropped whole.
    return (value % denominator) == 0;
}

VisitorId visitor_id(const http::PackedAddress& address, DayNumber day) noexcept {
    const std::shared_ptr<const crypto::Key256> pepper =
        g_pepper.load(std::memory_order_acquire);
    // NO FALLBACK. An unkeyed digest of a 32-bit input space is reversible from
    // a database dump, so a deployment without a pepper records nothing rather
    // than recording something that looks fine and is not.
    if (!pepper) { return VisitorId{}; }

    const http::PackedAddress coarse = coarsen_for_visitor(address);

    // address || day, little-endian, in one stack buffer. The day is part of the
    // INPUT rather than a separate field, so two days are two unrelated ids and
    // no join across them is possible even for whoever holds the collection.
    std::array<std::uint8_t, 20> input{};
    std::copy(coarse.begin(), coarse.end(), input.begin());
    const auto unsigned_day = static_cast<std::uint32_t>(day);
    for (std::size_t i = 0; i < 4; ++i) {
        input[16 + i] = static_cast<std::uint8_t>((unsigned_day >> (i * 8)) & 0xFFU);
    }

    const crypto::Digest256 digest = crypto::hmac_sha256(pepper->span(), input);

    VisitorId id{};
    std::copy(digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(id.size()),
              id.begin());
    // 128 bits of a 256-bit digest. A collision needs 2^64 visitors in one day,
    // which is eight bytes of storage saved against a bound nothing reaches.
    return id;
}

}  // namespace anvil::analytics
