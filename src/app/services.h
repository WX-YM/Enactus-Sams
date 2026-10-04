#pragma once

// Every anvil service this backend uses, constructed once at boot in dependency
// order and torn down in reverse. Handlers reach them through `services()`;
// nothing here is constructed lazily on a request path.

#include <memory>
#include <string>

#include "anvil/accounts/service.h"
#include "anvil/analytics/ingest.h"
#include "anvil/analytics/query.h"
#include "anvil/analytics/rollup.h"
#include "anvil/audit/service.h"
#include "anvil/auth/prehash.h"
#include "anvil/auth/token.h"
#include "anvil/db/collections.h"
#include "anvil/entries/service.h"
#include "anvil/forms/pii.h"
#include "anvil/forms/service.h"
#include "anvil/forms/submission_service.h"
#include "anvil/http/rate_limit.h"
#include "anvil/identity/authz.h"
#include "anvil/identity/session_service.h"
#include "anvil/identity/staff.h"
#include "anvil/identity/users.h"
#include "anvil/identity/verification.h"
#include "anvil/media/service.h"
#include "anvil/sections/service.h"
#include "anvil/timer/queue.h"
#include "app/config.h"

namespace enactus {

inline constexpr std::string_view kAccountsCollection = "accounts";
inline constexpr std::string_view kSessionsCollection = "user_sessions";
inline constexpr std::string_view kVerificationsCollection = "email_verifications";
inline constexpr std::string_view kStaffGuardCollection = "staff_guard";
inline constexpr std::string_view kStaffProfilesCollection = "staff_profiles";
inline constexpr std::string_view kMediaCollection = "media";
inline constexpr std::string_view kAuditCollection = "audit_log";

// The one media namespace (anvil_app_config.h), as the validated type.
[[nodiscard]] anvil::fs::Ns site_namespace();

class Services final {
public:
    explicit Services(const Config& config);
    ~Services();

    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;

    const Config& config;
    const anvil::db::DatabaseNames databases;
    const std::string database;  // the primary database's physical name

    std::shared_ptr<const anvil::auth::TokenKeys> token_keys;
    anvil::http::RateLimiter limiter;

    anvil::identity::AuthzService authz;
    anvil::identity::SessionService sessions;
    anvil::identity::VerificationService verification;
    anvil::identity::StaffService staff;
    anvil::identity::UserRepository accounts_repo;
    anvil::auth::PrehashHasher prehash;
    anvil::accounts::AccountService accounts;

    anvil::audit::AuditService audit;

    anvil::media::MediaService media;
    anvil::sections::SectionService sections;
    anvil::entries::EntryService entries;

    anvil::forms::FormService forms;
    anvil::forms::PiiKeys pii_keys;
    anvil::forms::SubmissionService submissions;

    anvil::analytics::EventSink events;
    anvil::analytics::AnalyticsQuery analytics;
    anvil::analytics::RollupJob hourly_rollup;
    anvil::analytics::RollupJob daily_rollup;

    anvil::timer::JobQueue jobs;

    // The policy the account flows hash under. A function rather than a member
    // because the policy owns its keys and both the account service and the
    // staff-creation path need one.
    [[nodiscard]] anvil::auth::PrehashPolicy prehash_policy() const;
};

// Installed by main() after construction; the handlers' only route to a
// service.
void install_services(std::unique_ptr<Services> services);
[[nodiscard]] Services& services();
[[nodiscard]] bool services_installed() noexcept;
std::unique_ptr<Services> uninstall_services() noexcept;

}  // namespace enactus
