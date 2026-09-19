#include "anvil/sections/service.h"

#include <exception>
#include <string>
#include <utility>

#include <mongocxx/client_session.hpp>
#include <mongocxx/exception/exception.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/crypto/fast_hash.h"
#include "anvil/db/versioned.h"
#include "anvil/redis/redis_client.h"
#include "anvil/sections/codec.h"

namespace anvil::sections {
namespace {

// The cached value is the serialised JSON with an 8-byte little-endian version
// prefix. Storing the version alongside rather than re-parsing it out of the
// JSON keeps the read path free of a parse it would otherwise pay on every
// Redis hit.
constexpr std::size_t kVersionPrefixBytes = 8;

// Aspect-ratio tolerance, in tenths of a percent. Cropping and resampling do not
// land on an exact ratio for every source size, and rejecting a 1920x1081 hero
// would be a defect dressed as a control. Compared with integer cross
// multiplication — never with a double, whose rounding is exactly the class of
// bug this avoids.
constexpr std::int64_t kAspectTolerancePermille = 10;

[[nodiscard]] std::string frame(const SerializedSection& payload) {
    std::string out;
    out.reserve(kVersionPrefixBytes + payload.json.size());
    for (std::size_t i = 0; i < kVersionPrefixBytes; ++i) {
        out.push_back(
            static_cast<char>((static_cast<std::uint64_t>(payload.version) >> (i * 8)) & 0xFFU));
    }
    out.append(payload.json);
    return out;
}

[[nodiscard]] std::shared_ptr<const SerializedSection> unframe(std::string value) {
    if (value.size() <= kVersionPrefixBytes) { return nullptr; }
    std::uint64_t version = 0;
    for (std::size_t i = 0; i < kVersionPrefixBytes; ++i) {
        version |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(value[i])) << (i * 8);
    }
    std::string json = value.substr(kVersionPrefixBytes);
    const crypto::FastDigest etag = crypto::xxh3_64(json);
    return std::make_shared<const SerializedSection>(SerializedSection{
        std::move(json), etag, crypto::etag_of(etag), static_cast<std::int64_t>(version)});
}

// Thrown to abort a transaction from inside the driver's callback. The driver
// retries a TransientTransactionError for us; a BUSINESS failure must not be
// retried, so it unwinds instead of returning and leaving the callback to commit
// what it just decided against.
struct AbortTransaction final : std::exception {
    explicit AbortTransaction(Failure f) noexcept : failure{f} {}
    [[nodiscard]] const char* what() const noexcept override { return "section write aborted"; }
    Failure failure;
};

}  // namespace

SectionService::SectionService(std::string database, std::string_view collection,
                               std::span<const SectionSpec> registry, media::MediaService& media,
                               SectionServiceConfig config)
    : database_{std::move(database)},
      registry_{registry},
      media_{media},
      config_{config},
      channel_{std::string{config.cache_prefix} + ":inval"},
      sections_{database_, collection},
      cache_{std::make_unique<Slot[]>(registry.size() * kLocaleCount)},
      listening_{false},
      listener_{} {}

SectionService::~SectionService() { stop_invalidation_listener(); }

// --- tier 1 -----------------------------------------------------------------

std::string SectionService::cache_key(std::string_view section_key, Locale locale) const {
    std::string key;
    key.reserve(config_.cache_prefix.size() + section_key.size() + locale.tag().size() + 2);
    key.append(config_.cache_prefix);
    key.push_back(':');
    key.append(section_key);
    key.push_back(':');
    key.append(locale.tag());
    return key;
}

std::shared_ptr<const SerializedSection> SectionService::peek(const SectionSpec& spec,
                                                              Locale locale) const noexcept {
    const std::size_t index = section_index(registry_, &spec);
    if (index >= registry_.size()) { return nullptr; }
    return cache_[(index * kLocaleCount) + locale.index()].load(std::memory_order_acquire);
}

void SectionService::store_local(std::size_t index, Locale locale,
                                 std::shared_ptr<const SerializedSection> payload) noexcept {
    if (index >= registry_.size()) { return; }
    cache_[(index * kLocaleCount) + locale.index()].store(std::move(payload),
                                                          std::memory_order_release);
}

void SectionService::invalidate_local(std::string_view key) noexcept {
    const SectionSpec* spec = find_section(registry_, key);
    if (spec == nullptr) { return; }
    const std::size_t index = section_index(registry_, spec);
    for (const Locale locale : kAllLocales) { store_local(index, locale, nullptr); }

    // AFTER the drop, never before. A consumer whose own cache sits above this
    // one re-reads on the callback, and a hook called first would hand it back
    // the stale value it was being told about.
    //
    // The REGISTRY's spelling of the key is what goes out, not the caller's.
    // `key` on the subscriber's path is a view into a message buffer that dies
    // with the callback; `spec->key` is a literal in .rodata that outlives the
    // process's last request (ENGINEERING_RULES.md §2.2).
    if (!config_.on_invalidated) { return; }
    try {
        config_.on_invalidated(spec->key);
    } catch (const std::exception& e) {
        // This runs on the subscriber thread on the path that matters most. An
        // exception let out of here is noexcept violated and std::terminate
        // called, so one consumer's bug in a callback would take the process
        // down and stop every OTHER instance's invalidations with it
        // (ENGINEERING_RULES.md §4).
        //
        // The key is copied for the log line rather than viewed: a registry key
        // is not NUL-terminated and this path is already the exceptional one.
        LOG_ERROR << "section invalidation hook threw for key " << std::string{spec->key}
                  << ": " << e.what();
    } catch (...) {
        LOG_ERROR << "section invalidation hook threw for key " << std::string{spec->key};
    }
}

// --- tiers 2 and 3 ----------------------------------------------------------

Result<std::shared_ptr<const SerializedSection>> SectionService::load(mongocxx::client& client,
                                                                      const SectionSpec& spec,
                                                                      Locale locale) {
    const std::size_t index = section_index(registry_, &spec);
    const std::string key = cache_key(spec.key, locale);

    try {
        const auto cached = redis::RedisClient::instance().get(key);
        if (cached) {
            std::shared_ptr<const SerializedSection> payload = unframe(*cached);
            if (payload) {
                store_local(index, locale, payload);
                return payload;
            }
        }
    } catch (const std::exception& e) {
        // Degraded, never fatal: the section is still readable from MongoDB.
        // Logged rather than swallowed, because a Redis outage that nobody
        // notices is one that stays.
        LOG_WARN << "section cache read degraded: " << e.what();
    }

    const Result<std::optional<SectionDocument>> stored =
        sections_.find(client, spec, SectionState::Published);
    if (!stored) { return stored.error(); }
    if (!stored.value().has_value()) {
        return std::shared_ptr<const SerializedSection>{};
    }

    const SectionDocument& document = *stored.value();
    auto payload = std::make_shared<const SerializedSection>(
        serialize(spec, document.content, document.version, locale, config_.image_url_base));

    try {
        redis::RedisClient::instance().set(
            key, frame(*payload),
            std::chrono::duration_cast<std::chrono::milliseconds>(config_.redis_ttl));
    } catch (const std::exception& e) {
        LOG_WARN << "section cache write degraded: " << e.what();
    }

    store_local(index, locale, payload);
    return payload;
}

Result<std::size_t> SectionService::preload(mongocxx::client& client) {
    std::size_t loaded = 0;
    for (const SectionSpec& spec : registry_) {
        for (const Locale locale : kAllLocales) {
            const Result<std::shared_ptr<const SerializedSection>> payload =
                load(client, spec, locale);
            if (!payload) { return payload.error(); }
            if (payload.value()) { ++loaded; }
        }
    }
    return loaded;
}

Result<std::optional<SectionDocument>> SectionService::read_document(mongocxx::client& client,
                                                                     const SectionSpec& spec,
                                                                     SectionState state) const {
    return sections_.find(client, spec, state);
}

// --- images -----------------------------------------------------------------

Status SectionService::verify_images(mongocxx::client& client, const SectionSpec& spec,
                                     const SectionContent& content) const {
    for (const SectionImage& image : content.images) {
        const ImageSpec* slot = find_image(spec, image.slot);
        if (slot == nullptr) { return fail(ErrorCode::ValidationFailed); }

        // The namespace is part of the lookup, so a media id uploaded through
        // another API cannot be attached to a section slot.
        const Result<std::optional<media::MediaRecord>> found =
            media_.find(client, config_.image_namespace, image.media_id);
        if (!found) { return found.error(); }
        if (!found.value().has_value()) {
            // Indistinguishable from "wrong namespace" and from "belongs to
            // someone else". The caller turns this into a stealth 404 rather
            // than confirming which media ids exist.
            return fail(ErrorCode::NotFound, slot->slot);
        }

        const media::MediaRecord& row = *found.value();
        if (row.width < slot->min_width || row.height < slot->min_height) {
            return fail(ErrorCode::ValidationFailed, slot->slot);
        }

        // Zero in either half means the slot constrains no ratio, and the cross
        // multiplication below would then compare against zero and reject
        // everything. Skipped explicitly rather than relying on the arithmetic.
        if (slot->aspect_num == 0 || slot->aspect_den == 0) { continue; }

        const auto width = static_cast<std::int64_t>(row.width);
        const auto height = static_cast<std::int64_t>(row.height);
        const auto num = static_cast<std::int64_t>(slot->aspect_num);
        const auto den = static_cast<std::int64_t>(slot->aspect_den);
        const std::int64_t cross = (width * den) - (height * num);
        const std::int64_t magnitude = cross < 0 ? -cross : cross;
        if (magnitude * 1000 > width * den * kAspectTolerancePermille) {
            return fail(ErrorCode::ValidationFailed, slot->slot);
        }
    }
    return ok();
}

// --- write ------------------------------------------------------------------

// The first draft of a section, created from the published content plus the
// patch. Separate from `write` because it shares none of its shape: there is no
// prior draft to compare against, no version to filter on, and no image
// reference count to move — a draft references media the published document
// already references, and taking a second count on the same id would leak a
// reference the moment the draft is discarded.
Result<SectionWriteOutcome> SectionService::create_first_draft(mongocxx::client& client,
                                                               const SectionSpec& spec,
                                                               const SectionContent& patch,
                                                               const Uuid& actor) {
    const Result<std::optional<SectionDocument>> live =
        sections_.find(client, spec, SectionState::Published);
    if (!live) { return live.error(); }
    // No published document means the key was never bootstrapped, which is the
    // fault a NotFound here is actually reporting.
    if (!live.value().has_value()) { return fail(ErrorCode::NotFound); }

    const SectionContent merged = canonicalise(spec, merge(live.value()->content, patch));
    if (const std::optional<BindError> missing = check_required(spec, merged)) {
        return fail(ErrorCode::ValidationFailed, missing->field);
    }
    if (const Status images = verify_images(client, spec, merged); !images) {
        return images.error();
    }

    SectionDocument document{};
    document.content = merged;
    document.etag = content_etag(spec, merged);
    document.updated_by = actor;
    document.version = repo::kInitialVersion;

    // Insert-if-absent rather than insert: two staff pressing "save as draft" in
    // the same second are two creates of the same document, and the loser must
    // be told its version is stale rather than handed a duplicate-key error or,
    // worse, overwriting the winner.
    const Result<bool> created =
        sections_.insert_if_absent(client, spec, SectionState::Draft, document, actor);
    if (!created) { return created.error(); }
    if (!created.value()) { return fail(ErrorCode::VersionMismatch); }

    // No cache and no invalidation: tier 1 holds PUBLISHED sections only, and a
    // draft has no public reader to go stale.
    return SectionWriteOutcome{crypto::etag_of(document.etag), repo::kInitialVersion};
}

Result<SectionWriteOutcome> SectionService::write(mongocxx::client& client,
                                                  const SectionSpec& spec, SectionState state,
                                                  std::int64_t expected_version,
                                                  const SectionContent& patch,
                                                  const Uuid& actor) {
    const Result<std::optional<SectionDocument>> stored = sections_.find(client, spec, state);
    if (!stored) { return stored.error(); }
    if (!stored.value().has_value()) {
        // A DRAFT is allowed not to exist yet, and this is the branch that
        // creates the first one.
        //
        // Bootstrap seeds the Published document of every registry section and
        // no Draft of any of them, so without this branch the first "save as
        // draft" of every section answers NotFound — the state a fresh database
        // is in is the state where the feature does not work, which is every
        // deployment. READING a draft that has never been written still answers
        // NotFound, deliberately: "I saved a draft" and "I never saved one" must
        // not render identically.
        //
        // A draft is a draft OF the live page, so the base is the published
        // content and the patch lands on top of it. Anything the patch does not
        // mention therefore reads as the site reads today rather than as empty.
        if (state != SectionState::Draft) { return fail(ErrorCode::NotFound); }
        // Only from zero. A caller that believes a draft already exists at some
        // version is working from a stale read, and creating one underneath it
        // would silently discard whatever it thought it was editing.
        if (expected_version != 0) { return fail(ErrorCode::VersionMismatch); }
        return create_first_draft(client, spec, patch, actor);
    }

    const SectionDocument& before = *stored.value();
    const SectionContent merged = canonicalise(spec, merge(before.content, patch));

    if (const std::optional<BindError> missing = check_required(spec, merged)) {
        return fail(ErrorCode::ValidationFailed, missing->field);
    }
    if (const Status images = verify_images(client, spec, merged); !images) {
        return images.error();
    }

    SectionDocument next{};
    next.content = merged;
    next.etag = content_etag(spec, merged);
    next.updated_by = actor;
    next.version = before.version;

    SectionWriteOutcome outcome{};
    try {
        auto session = client.start_session();
        repo::in_transaction(session, [&](mongocxx::client_session* txn) {
            const Result<std::int64_t> version =
                sections_.update(client, *txn, spec, state, expected_version, next, actor);
            if (!version) { throw AbortTransaction{version.error()}; }

            // Reference counts move inside the SAME transaction as the update
            // that changed them. A count committed on its own is wrong the
            // moment the section write aborts, and the two failure modes are a
            // file the collector deletes out from under live content, or one it
            // can never reclaim.
            for (const SectionImage& image : merged.images) {
                const SectionImage* previous = before.content.find_image(image.slot);
                if (previous != nullptr && previous->media_id == image.media_id) { continue; }
                if (const Status attached =
                        media_.attach(client, *txn, config_.image_namespace, image.media_id);
                    !attached) {
                    throw AbortTransaction{attached.error()};
                }
            }
            for (const SectionImage& image : before.content.images) {
                const SectionImage* now = merged.find_image(image.slot);
                if (now != nullptr && now->media_id == image.media_id) { continue; }
                if (const Status released =
                        media_.release(client, *txn, config_.image_namespace, image.media_id);
                    !released) {
                    throw AbortTransaction{released.error()};
                }
            }

            outcome.version = version.value();
        });
    } catch (const AbortTransaction& aborted) {
        return aborted.failure;
    } catch (const mongocxx::exception& e) {
        LOG_ERROR << "section write transaction failed: " << e.what();
        return fail(ErrorCode::ServiceUnavailable);
    }

    // Serialised ONCE, here, so the first reader after a write does not pay for
    // it — and so the etag the client is handed is the etag a conditional GET
    // will compare against.
    //
    // THE ORDER IS LOAD-BEARING and it is the reverse of the obvious one.
    // publish_invalidation drops this instance's entry as its first act, so
    // storing the fresh bytes before it publishes means storing them and then
    // immediately nulling them: the write path would serialise twice as much as
    // it needed to and still leave the next reader going to MongoDB. Invalidate
    // first, then install what this instance already knows to be current.
    if (state == SectionState::Published) {
        publish_invalidation(spec.key);
        const std::size_t index = section_index(registry_, &spec);
        for (const Locale locale : kAllLocales) {
            store_local(index, locale,
                        std::make_shared<const SerializedSection>(serialize(
                            spec, merged, outcome.version, locale, config_.image_url_base)));
        }
    }

    outcome.etag = crypto::etag_of(next.etag);
    return outcome;
}

// --- invalidation -----------------------------------------------------------

void SectionService::publish_invalidation(std::string_view key) noexcept {
    // The local drop happens first and unconditionally. It is the one step that
    // cannot fail, and it is the one that matters most on a single-instance
    // deployment.
    //
    // It also means a caller holding fresher bytes than this must install them
    // AFTER calling here, never before — see the ordering note in write().
    invalidate_local(key);
    try {
        sw::redis::Redis& redis = redis::RedisClient::instance();
        for (const Locale locale : kAllLocales) { redis.del(cache_key(key, locale)); }
        redis.publish(channel_, std::string{key});
    } catch (const std::exception& e) {
        LOG_WARN << "section invalidation not published; other instances may serve stale "
                    "content until their entries are replaced: "
                 << e.what();
    }
}

void SectionService::start_invalidation_listener() {
    if (listening_.exchange(true)) { return; }
    listener_ = std::thread{[this]() { listener_loop(); }};
}

void SectionService::stop_invalidation_listener() noexcept {
    if (!listening_.exchange(false)) { return; }
    // The subscriber blocks in consume() with a socket timeout, so it observes
    // the flag within one timeout period rather than needing to be interrupted.
    if (listener_.joinable()) { listener_.join(); }
}

void SectionService::listener_loop() noexcept {
    while (listening_.load(std::memory_order_acquire)) {
        try {
            sw::redis::Subscriber subscriber = redis::RedisClient::instance().subscriber();
            subscriber.on_message([this](std::string /*channel*/, std::string message) {
                // The message is a section key. It is looked up in the registry
                // and discarded if it is not there — a published key is data
                // from another process, and it gets the same allow-list
                // treatment a request would.
                invalidate_local(message);
            });
            subscriber.subscribe(channel_);

            while (listening_.load(std::memory_order_acquire)) {
                try {
                    subscriber.consume();
                } catch (const sw::redis::TimeoutError&) {
                    // Expected: the socket timeout is what makes the loop
                    // observe the stop flag without an interrupt mechanism.
                    continue;
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN << "section invalidation listener reconnecting: " << e.what();
            // A tight reconnect loop against a down Redis would spin a core.
            for (int i = 0; i < 20 && listening_.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
    }
}

}  // namespace anvil::sections
