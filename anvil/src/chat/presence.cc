// versioned-write-exempt: last seen is a $max of a timestamp onto one account's
// row, so two writers racing cannot lose anything either would keep.

#include "anvil/chat/presence.h"

#include <array>
#include <charconv>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/update.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/core/uuid.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/db/repository.h"
#include "anvil/input/fields.h"

namespace anvil::chat {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

// The key outlives its account's last socket by an hour, so "last seen a minute
// ago" is answered from Redis and only an older answer from the row.
constexpr long long kKeyTtlSeconds = 3600;

// The most last-seen rows one pass writes, so a process holding a hundred
// thousand accounts spreads their writes over several passes rather than
// holding a database connection for one long one.
constexpr std::size_t kMaxWritesPerPass = 1000;

constexpr std::string_view kSeenField = "seen";

[[nodiscard]] std::string key_of(const Uuid& user) {
    std::string key;
    key.reserve(kPresenceKeyPrefix.size() + 32);
    key.append(kPresenceKeyPrefix);
    constexpr std::string_view kHex = "0123456789abcdef";
    for (const std::uint8_t byte : user) {
        key.push_back(kHex[byte >> 4U]);
        key.push_back(kHex[byte & 0x0FU]);
    }
    return key;
}

[[nodiscard]] std::int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               db::now_ms().time_since_epoch())
        .count();
}

// A value this tracker wrote, or nullopt for anything else: a Redis key is not
// a trusted input, and a value that does not parse is no answer at all.
[[nodiscard]] std::optional<std::int64_t> parse_value(std::string_view text) noexcept {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

PresenceTracker::PresenceTracker(PresenceConfig config, sw::redis::Redis& redis)
    : config_{std::move(config)},
      redis_{redis},
      database_{config_.enabled
                    ? std::string{config_.databases.for_collection(config_.collection)}
                    : std::string{}} {
    if (!config_.enabled) { return; }
    if (!db::collection_is_declared(config_.collection)) {
        throw std::invalid_argument{
            "presence is enabled but its collection is not in the collection table"};
    }
    if (config_.refresh.count() <= 0 || config_.online_window <= config_.refresh) {
        throw std::invalid_argument{
            "presence needs a refresh interval shorter than its online window"};
    }
    thread_ = std::thread{[this]() { run(); }};
}

PresenceTracker::~PresenceTracker() { stop(); }

void PresenceTracker::stop() noexcept {
    {
        const std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) { thread_.join(); }
}

void PresenceTracker::came_online(const Uuid& user) {
    if (!config_.enabled) { return; }
    {
        const std::lock_guard lock{mutex_};
        pending_[user] = true;
    }
    wake_.notify_one();
}

void PresenceTracker::went_offline(const Uuid& user) {
    if (!config_.enabled) { return; }
    {
        const std::lock_guard lock{mutex_};
        pending_[user] = false;
    }
    wake_.notify_one();
}

void PresenceTracker::run() noexcept {
    auto next_refresh = std::chrono::steady_clock::now() + config_.refresh;
    while (true) {
        {
            std::unique_lock lock{mutex_};
            wake_.wait_until(lock, next_refresh,
                             [this]() { return stopping_ || !pending_.empty(); });
            if (stopping_) { return; }
        }
        const bool due = std::chrono::steady_clock::now() >= next_refresh;
        if (due) { next_refresh = std::chrono::steady_clock::now() + config_.refresh; }
        pass(due);
    }
}

void PresenceTracker::pass(bool refresh_all) noexcept {
    std::map<Uuid, bool> changes;
    {
        const std::lock_guard lock{mutex_};
        changes.swap(pending_);
    }
    const std::int64_t now = now_ms();
    std::vector<Uuid> wrote_offline;
    try {
        // Every Redis write of the pass in one round trip. The changes first,
        // so a socket that opened is online within one pass rather than one
        // refresh.
        auto pipe = redis_.pipeline(false);
        std::size_t queued = 0;
        for (const auto& [user, online] : changes) {
            if (online) {
                online_.insert(user);
            } else {
                online_.erase(user);
                wrote_offline.push_back(user);
            }
            pipe.set(key_of(user), std::to_string(online ? now : -now),
                     std::chrono::seconds{kKeyTtlSeconds});
            ++queued;
        }
        if (refresh_all) {
            for (const Uuid& user : online_) {
                if (changes.contains(user)) { continue; }
                pipe.set(key_of(user), std::to_string(now), std::chrono::seconds{kKeyTtlSeconds});
                ++queued;
            }
        }
        if (queued != 0) { (void)pipe.exec(); }
    } catch (const std::exception& e) {
        // The keys go stale and their accounts read as offline: presence is a
        // hint, and the next pass writes them again.
        LOG_WARN << "chat presence refresh failed: " << e.what();
    }

    // Last seen, where it is due: everybody still here on a full refresh, and
    // everybody who just left.
    std::vector<Uuid> due;
    const std::int64_t interval_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(config_.last_seen_write).count();
    const auto is_due = [&](const Uuid& user) {
        const auto found = written_ms_.find(user);
        return found == written_ms_.end() || now - found->second >= interval_ms;
    };
    if (refresh_all) {
        for (const Uuid& user : online_) {
            if (due.size() >= kMaxWritesPerPass) { break; }
            if (is_due(user)) { due.push_back(user); }
        }
        // Forget accounts that are gone and whose interval has passed: their
        // next write is due anyway, and the map stays the size of the accounts
        // active in the last interval.
        std::erase_if(written_ms_, [&](const auto& entry) {
            return !online_.contains(entry.first) && now - entry.second >= interval_ms;
        });
    }
    for (const Uuid& user : wrote_offline) {
        if (due.size() >= kMaxWritesPerPass) { break; }
        if (is_due(user)) { due.push_back(user); }
    }
    if (due.empty()) { return; }

    try {
        auto entry = db::MongoPool::instance().acquire();
        mongocxx::client& client = *entry;
        auto rows = client[database_][std::string{config_.collection}];
        mongocxx::options::update upsert{};
        upsert.upsert(true);
        const Uuid nobody{};
        for (const Uuid& user : due) {
            written_ms_[user] = now;
            // Only a value somebody may be shown is stored. Asked with no viewer:
            // "may ANYBODY see this account's presence".
            if (!config_.may_see || !config_.may_see(client, nobody, user)) { continue; }
            rows.update_one(
                make_document(kvp("_id", db::codec::uuid_bin(user))),
                make_document(kvp("$max", make_document(kvp(
                                              db::codec::key_of(kSeenField),
                                              db::codec::time_date(db::TimeMs{
                                                  std::chrono::milliseconds{now}}))))),
                upsert);
            last_seen_writes_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (const std::exception& e) {
        LOG_WARN << "chat last-seen write failed: " << e.what();
    }
}

Result<PresenceView> PresenceTracker::view(mongocxx::client& client, const Uuid& viewer,
                                           const Uuid& subject) const {
    if (!config_.enabled) { return fail(ErrorCode::NotFound); }
    // Withheld and unknown are one answer, so a viewer cannot tell "hidden
    // from me" from "never online".
    if (viewer != subject && (!config_.may_see || !config_.may_see(client, viewer, subject))) {
        return PresenceView{std::nullopt, false};
    }
    const std::int64_t now = now_ms();
    try {
        const sw::redis::OptionalString value = redis_.get(key_of(subject));
        if (value) {
            if (const std::optional<std::int64_t> parsed = parse_value(*value)) {
                const std::int64_t window_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(config_.online_window)
                        .count();
                const bool online = *parsed > 0 && now - *parsed < window_ms;
                const std::int64_t at = *parsed > 0 ? *parsed : -*parsed;
                return PresenceView{db::TimeMs{std::chrono::milliseconds{at}}, online};
            }
        }
    } catch (const std::exception& e) {
        // Fall through to the row: without Redis nobody reads as online, and
        // last seen is the stored one.
        LOG_WARN << "chat presence read failed: " << e.what();
    }
    return repo::guarded([&]() -> Result<PresenceView> {
        mongocxx::options::find options{};
        options.projection(make_document(kvp(db::codec::key_of(kSeenField), 1)));
        const auto row = client[database_][std::string{config_.collection}].find_one(
            make_document(kvp("_id", db::codec::uuid_bin(subject))), options);
        if (!row) { return PresenceView{std::nullopt, false}; }
        const Result<db::TimeMs> seen = db::codec::read_time(row->view(), kSeenField);
        if (!seen) { return PresenceView{std::nullopt, false}; }
        return PresenceView{seen.value(), false};
    });
}

Result<std::vector<PresenceView>> PresenceTracker::view_many(mongocxx::client& client,
                                                             const Uuid& viewer,
                                                             std::span<const Uuid> subjects) const {
    if (!config_.enabled) { return fail(ErrorCode::NotFound); }
    if (subjects.size() > kMaxPresenceBatch) {
        return Failure{ErrorCode::ValidationFailed, "users",
                       static_cast<std::uint16_t>(input::Reason::TooLong)};
    }
    std::vector<PresenceView> out(subjects.size(), PresenceView{std::nullopt, false});
    // Only the subjects the viewer may see are read at all, so a withheld one
    // costs the same as one nobody has heard of: the hook, and nothing else.
    std::vector<std::size_t> visible;
    visible.reserve(subjects.size());
    for (std::size_t i = 0; i < subjects.size(); ++i) {
        if (viewer == subjects[i] ||
            (config_.may_see && config_.may_see(client, viewer, subjects[i]))) {
            visible.push_back(i);
        }
    }
    if (visible.empty()) { return out; }

    std::vector<std::size_t> unheard;
    unheard.reserve(visible.size());
    try {
        std::vector<std::string> keys;
        keys.reserve(visible.size());
        for (const std::size_t i : visible) { keys.push_back(key_of(subjects[i])); }
        std::vector<sw::redis::OptionalString> values;
        values.reserve(keys.size());
        redis_.mget(keys.begin(), keys.end(), std::back_inserter(values));
        const std::int64_t now = now_ms();
        const std::int64_t window_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(config_.online_window).count();
        for (std::size_t k = 0; k < visible.size(); ++k) {
            const std::optional<std::int64_t> parsed =
                k < values.size() && values[k] ? parse_value(*values[k]) : std::nullopt;
            if (!parsed.has_value()) {
                unheard.push_back(visible[k]);
                continue;
            }
            const std::int64_t at = *parsed > 0 ? *parsed : -*parsed;
            out[visible[k]] = PresenceView{db::TimeMs{std::chrono::milliseconds{at}},
                                           *parsed > 0 && now - *parsed < window_ms};
        }
    } catch (const std::exception& e) {
        // As view(): without Redis nobody is online, and last seen is the row.
        LOG_WARN << "chat presence read failed: " << e.what();
        unheard = visible;
    }
    if (unheard.empty()) { return out; }
    const Status read = repo::guarded([&]() -> Status {
        bsoncxx::builder::basic::array in;
        for (const std::size_t i : unheard) { in.append(db::codec::uuid_bin(subjects[i])); }
        mongocxx::options::find options{};
        options.projection(make_document(kvp(db::codec::key_of(kSeenField), 1)));
        options.limit(static_cast<std::int64_t>(unheard.size()));
        for (const bsoncxx::document::view row : client[database_][std::string{config_.collection}].find(
                 make_document(kvp("_id", make_document(kvp("$in", in.extract())))), options)) {
            const Result<Uuid> who = db::codec::read_uuid(row, "_id");
            const Result<db::TimeMs> seen = db::codec::read_time(row, kSeenField);
            if (!who || !seen) { continue; }
            for (const std::size_t i : unheard) {
                if (subjects[i] == who.value()) { out[i] = PresenceView{seen.value(), false}; }
            }
        }
        return ok();
    });
    if (!read) { return read.error(); }
    return out;
}

}  // namespace anvil::chat
