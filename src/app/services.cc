#include "app/services.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include <trantor/utils/Logger.h>

#include "anvil/auth/password.h"
#include "accounts.h"
#include "audit_actions.h"
#include "entries.h"
#include "events.h"
#include "field_types.h"
#include "rate_limits.h"
#include "sections.h"

namespace enactus {

namespace {

std::unique_ptr<Services>& slot() {
    static std::unique_ptr<Services> installed;
    return installed;
}

[[nodiscard]] anvil::crypto::Key256 copy_key(const anvil::crypto::SecretBuffer<32>& from) {
    anvil::crypto::Key256 key;
    std::copy(from.span().begin(), from.span().end(), key.mutable_span().begin());
    return key;
}

// The physical name of every logical database in anvil_app_config.h. One
// database, named by configuration.
[[nodiscard]] anvil::db::DatabaseNames database_names(const Config& config) {
    static std::string stored;
    stored = config.mongo_database;
    return anvil::db::DatabaseNames{{std::string_view{stored}}};
}

// Five consecutive failures lock the account for fifteen minutes; each further
// one doubles it, to a day (anvil docs/05-auth-sessions.md §4: the backoff is
// the application's).
[[nodiscard]] std::optional<anvil::db::TimeMs> lock_after(std::int32_t failures,
                                                          anvil::db::TimeMs now) {
    if (failures < 5) { return std::nullopt; }
    const std::int32_t doublings = std::min(failures - 5, 7);
    return now + std::chrono::minutes{15} * (1 << doublings);
}

// No route this application declares sends a code (accounts.h): there is no
// self-registration, no verification and no email reset. A delivery reaching
// here is a configuration change somebody made without wiring a transport, and
// it is logged rather than silently dropped.
void no_delivery(anvil::accounts::CodeDelivery delivery) {
    LOG_ERROR << "an account code was issued but this deployment has no code transport "
                 "(purpose "
              << static_cast<int>(delivery.purpose) << ")";
}

}  // namespace

anvil::fs::Ns site_namespace() {
    static const anvil::fs::Ns ns = [] {
        const std::optional<anvil::fs::Ns> found = anvil::fs::Ns::from_dir("site");
        if (!found.has_value()) { throw std::logic_error{"the site namespace is not declared"}; }
        return *found;
    }();
    return ns;
}

anvil::auth::PrehashPolicy Services::prehash_policy() const {
    return anvil::auth::PrehashPolicy{
        .client = anvil::auth::kDefaultArgon2Params,
        .server = anvil::auth::PrehashKeyedDigestStage{.key_id = config.prehash_pepper_id,
                                                       .key = copy_key(config.prehash_pepper)},
        .retired_peppers = {},
        .salt_key = copy_key(config.prehash_salt_key),
    };
}

Services::Services(const Config& cfg)
    : config{cfg},
      databases{database_names(cfg)},
      database{cfg.mongo_database},
      token_keys{std::make_shared<const anvil::auth::TokenKeys>(
          std::uint8_t{1}, std::span<const std::uint8_t>{cfg.token_signing_key.span()})},
      limiter{},
      authz{database, kAccountsCollection},
      sessions{database,  kSessionsCollection,          kAccountsCollection,
               cfg.session_pepper.span(), token_keys, authz,
               anvil::identity::SessionPolicy{}},
      verification{database, kVerificationsCollection, cfg.code_pepper.span(),
                   cfg.address_index_key.span()},
      staff{database, kAccountsCollection, kSessionsCollection, kStaffGuardCollection, authz},
      accounts_repo{database, kAccountsCollection},
      prehash{prehash_policy()},
      accounts{anvil::accounts::AccountServiceDeps{database, kAccountsCollection, sessions,
                                                   authz, verification, limiter},
               anvil::accounts::AccountConfig{
                   .description = kAccounts,
                   .credentials = anvil::accounts::ClientHashing{prehash_policy()},
                   .budgets = {.sign_in_ip = rate_rule("login"),
                               .sign_in_account = rate_rule("login-acct"),
                               .register_ip = rate_rule("closed"),
                               .code_ip = rate_rule("closed"),
                               .code_account = rate_rule("closed"),
                               .issue_ip = rate_rule("closed"),
                               .issue_account = rate_rule("closed")},
                   .lock_after = &lock_after,
                   .deliver = &no_delivery,
               }},
      audit{database, kAuditCollection, kAuditActions, enactus::audit(Action::AccessDenied)},
      media{database, kMediaCollection},
      sections{database, kSectionsCollection, kSections, media,
               anvil::sections::SectionServiceConfig{
                   .content_origin = cfg.site_origin,
                   .image_url_base = "/media/site/",
                   .cache_prefix = "enactus:sec",
                   .image_namespace = site_namespace(),
                   .on_invalidated = {}}},
      entries{database, kEntriesCollection, kKinds, media,
              anvil::entries::EntryServiceConfig{.image_namespace = site_namespace(),
                                                 .channel_prefix = "enactus:ent",
                                                 .on_invalidated = {}}},
      forms{database, kFormDefinitionsCollection, kFormResponsesCollection, kFieldTypes,
            anvil::forms::AttachmentHooks{}},
      pii_keys{cfg.pii_seal_key.span(), cfg.pii_index_key.span()},
      submissions{database, kFormDefinitionsCollection, kFormResponsesCollection, kFieldTypes,
                  anvil::forms::AttachmentHooks{}, pii_keys},
      events{databases, kAnalyticsCollections, kEvents, anvil::analytics::IngestConfig{}},
      analytics{databases, kAnalyticsCollections},
      hourly_rollup{databases, kAnalyticsCollections, anvil::analytics::Granularity::Hour},
      daily_rollup{databases, kAnalyticsCollections, anvil::analytics::Granularity::Day},
      jobs{anvil::timer::JobQueueConfig{.prefix = "enactus:" + cfg.mongo_database + ":jobs",
                                        .consumer = "backend"}} {}

Services::~Services() = default;

void install_services(std::unique_ptr<Services> services) {
    if (slot() != nullptr) { throw std::logic_error{"services installed twice"}; }
    slot() = std::move(services);
}

Services& services() {
    Services* const installed = slot().get();
    if (installed == nullptr) { throw std::logic_error{"services used before install"}; }
    return *installed;
}

bool services_installed() noexcept { return slot() != nullptr; }

std::unique_ptr<Services> uninstall_services() noexcept { return std::move(slot()); }

}  // namespace enactus
