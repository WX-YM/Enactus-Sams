// enactus_migrate: the deploy-time step (anvil docs/00-architecture.md §7.7).
//
//   enactus_migrate                         indexes, then the default sections,
//                                           photos and teams on a fresh database
//   enactus_migrate --create-superadmin E   also creates superadmin E; the
//                                           password is read from stdin
//   enactus_migrate --legacy [--uploads D]  also imports the pre-anvil
//                                           collections, once (src/app/legacy.h);
//                                           --force-legacy repeats it
//
// Reads the same environment as the server. Safe to run on every deploy: every
// step is idempotent.

#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/update.hpp>
#include <trantor/utils/Logger.h>

#include "anvil/accounts/identifier.h"
#include "anvil/core/thread_pools.h"
#include "anvil/core/uuid.h"
#include "anvil/db/migrations.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/db/versioned.h"
#include "anvil/fs/paths.h"
#include "anvil/identity/login_identity.h"
#include "anvil/images/probe.h"
#include "anvil/input/fields.h"
#include "anvil/redis/redis_client.h"
#include "anvil/sections/bootstrap.h"
#include "anvil/sections/default_images.h"
#include "app/config.h"
#include "app/legacy.h"
#include "app/services.h"
#include "app/staff.h"
#include "entries.h"
#include "indexes.h"
#include "perms.h"
#include "sections.h"

namespace {

struct Arguments final {
    std::optional<std::string> superadmin;
    bool                       legacy = false;
    bool                       force = false;
    std::string                uploads = "uploads";
};

[[nodiscard]] std::optional<Arguments> parse(int argc, char** argv) {
    Arguments out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--legacy") {
            out.legacy = true;
        } else if (arg == "--force-legacy") {
            out.legacy = true;
            out.force = true;
        } else if (arg == "--uploads" && i + 1 < argc) {
            out.uploads = argv[++i];
        } else if (arg == "--create-superadmin" && i + 1 < argc) {
            out.superadmin = argv[++i];
        } else {
            return std::nullopt;
        }
    }
    return out;
}

// One line from stdin, without echo when stdin is a terminal.
[[nodiscard]] std::string read_secret(const char* prompt) {
    const bool tty = ::isatty(STDIN_FILENO) == 1;
    termios saved{};
    if (tty) {
        std::fprintf(stderr, "%s", prompt);
        ::tcgetattr(STDIN_FILENO, &saved);
        termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    std::string line;
    std::getline(std::cin, line);
    if (tty) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        std::fprintf(stderr, "\n");
    }
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    return line;
}

int create_superadmin(mongocxx::client& client, const std::string& raw_email) {
    std::string email;
    if (!anvil::input::is_ok(anvil::accounts::canonicalise(anvil::identity::LoginIdentity::Email, raw_email, email))) {
        std::fprintf(stderr, "enactus_migrate: not a valid email address\n");
        return 1;
    }
    const std::string password = read_secret("Password for the new superadmin: ");
    if (const auto reason = anvil::input::check_password(password); !anvil::input::is_ok(reason)) {
        std::fprintf(stderr, "enactus_migrate: that password is refused by the password policy "
                             "(12 to 128 characters)\n");
        return 1;
    }
    enactus::Services& s = enactus::services();
    const std::string record = s.prehash.enroll_plaintext(
        password, s.prehash.derive_salt(static_cast<std::uint8_t>(anvil::identity::LoginIdentity::Email), email));
    const anvil::Uuid id = anvil::uuid::generate_v7();
    const anvil::Status inserted = s.accounts_repo.insert(
        client, anvil::identity::NewUser{.id = id,
                                         .email_normalised = email,
                                         .email_display = email,
                                         .username_normalised = {},
                                         .username_display = {},
                                         .password_hash = record,
                                         .phone_e164 = {},
                                         .locale = anvil::Locale{},
                                         .status = anvil::UserStatus::Active});
    if (!inserted) {
        std::fprintf(stderr, "enactus_migrate: %s\n",
                     inserted.error().code == anvil::ErrorCode::Conflict ? "an account with that email already exists"
                                                                         : "the account could not be created");
        return 1;
    }
    const auto typed = s.staff.set_user_type(client, id, anvil::repo::kInitialVersion, anvil::UserType::SuperAdmin,
                                             enactus::with_implied(enactus::kGrantable), {},
                                             anvil::identity::RoleTable{}, anvil::UserType::SuperAdmin);
    if (!typed) {
        std::fprintf(stderr, "enactus_migrate: the account was created but could not be made superadmin\n");
        return 1;
    }
    (void)enactus::StaffProfiles::put(client, id, enactus::StaffProfile{"high board", ""});
    std::fprintf(stderr, "enactus_migrate: superadmin %s created\n", email.c_str());
    return 0;
}

void print_report(const enactus::legacy::Report& r) {
    std::fprintf(stderr,
                 "legacy import:\n"
                 "  users          %zu imported, %zu already present, %zu refused\n"
                 "  teams          %zu created, %zu merged, %zu roster members\n"
                 "  applications   %zu imported, %zu skipped\n"
                 "  site content   %zu fields written, %zu kept their default\n"
                 "  galleries      %zu photos, %zu images missing\n"
                 "  forms          %zu imported with %zu responses\n",
                 r.users_imported, r.users_skipped, r.users_refused, r.teams_created, r.teams_merged,
                 r.members_imported, r.applications_imported, r.applications_skipped, r.section_fields,
                 r.section_fields_refused, r.gallery_photos, r.images_missing, r.forms_imported, r.responses_imported);
    for (const std::string& note : r.notes) { std::fprintf(stderr, "  note: %s\n", note.c_str()); }
}

int run(const Arguments& args, const enactus::Config& config) {
    anvil::Pools::init(anvil::PoolSizes{.db_threads = 2, .db_queue = 16, .cpu_threads = 1, .cpu_queue = 8,
                                        .hash_threads = 1, .hash_queue = 4, .audit_threads = 1,
                                        .audit_queue = 8, .analytics_threads = 1, .analytics_queue = 8});
    anvil::db::MongoPool::init(config.mongodb_uri, 4);
    try {
        anvil::redis::RedisClient::init(anvil::redis::RedisConfig{
            .url = config.redis_url, .pool_size = 2, .connect_timeout_ms = 1000, .socket_timeout_ms = 1000});
    } catch (const std::exception&) {
        // Only cross-instance cache notices go to Redis from here; a running
        // server refills its caches on its own expiry.
    }
    anvil::images::init("enactus_migrate");
    anvil::fs::Storage::init(config.storage_root);
    enactus::install_services(std::make_unique<enactus::Services>(config));
    enactus::Services& s = enactus::services();

    auto client = anvil::db::MongoPool::instance().acquire();

    const anvil::db::MigrationReport indexes =
        anvil::db::apply_migrations(*client, s.databases, enactus::kIndexes, enactus::kSchemaVersion,
                                    enactus::kRetiredIndexes);
    std::fprintf(stderr, "indexes: %zu applied, schema %d -> %d\n", indexes.indexes_applied,
                 indexes.previous_version, indexes.current_version);

    const anvil::sections::DefaultImageResolver resolve = anvil::sections::default_image_resolver(
        *client, s.media, enactus::site_namespace(), config.defaults_dir, anvil::Uuid{});
    const auto sections = anvil::sections::bootstrap_sections(*client, s.sections.repository(), enactus::kSections,
                                                              enactus::kSectionDefaults, resolve, anvil::Uuid{});
    if (!sections) {
        std::fprintf(stderr, "enactus_migrate: seeding the site sections failed\n");
        return 1;
    }
    std::fprintf(stderr, "sections: %zu created, %zu present, %zu images registered, %zu images missing\n",
                 sections.value().sections_created, sections.value().sections_present,
                 sections.value().images_registered, sections.value().images_missing);

    const auto entries = s.entries.bootstrap(*client, enactus::kSeeds, resolve, anvil::Uuid{});
    if (!entries) {
        std::fprintf(stderr, "enactus_migrate: seeding the galleries and teams failed\n");
        return 1;
    }
    std::fprintf(stderr, "entries: %zu kinds seeded, %zu already seeded, %zu entries, %zu images missing\n",
                 entries.value().kinds_seeded, entries.value().kinds_already_seeded, entries.value().entries_created,
                 entries.value().images_missing);

    if (args.legacy) {
        // One run, ever: a second would lay the old site's copy and photos back
        // over whatever staff have edited since.
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        auto marker = (*client)[s.database]["legacy_import"];
        const auto done = marker.find_one(make_document(kvp("_id", "done")));
        if (done && !args.force) {
            std::fprintf(stderr, "legacy import: already done on this database; skipped "
                                 "(--force-legacy re-runs it)\n");
        } else {
            const enactus::legacy::Report report =
                enactus::legacy::import_all(*client, enactus::legacy::Options{config.doc_root, args.uploads});
            print_report(report);
            for (const std::string_view key : enactus::kGalleryKinds) { s.entries.publish_invalidation(key); }
            mongocxx::options::update upsert;
            upsert.upsert(true);
            marker.update_one(make_document(kvp("_id", "done")),
                              make_document(kvp("$set", make_document(kvp("at", bsoncxx::types::b_date{
                                                                                   std::chrono::system_clock::now()})))),
                              upsert);
        }
    }
    if (args.superadmin.has_value()) {
        if (const int verdict = create_superadmin(*client, *args.superadmin); verdict != 0) { return verdict; }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    trantor::Logger::setLogLevel(trantor::Logger::kWarn);
    const std::optional<Arguments> args = parse(argc, argv);
    if (!args.has_value()) {
        std::fprintf(stderr, "usage: enactus_migrate [--legacy | --force-legacy] [--uploads DIR] [--create-superadmin EMAIL]\n");
        return 2;
    }
    enactus::Config config;
    try {
        config = enactus::load_config(anvil::config::system_environment());
    } catch (const std::exception& refused) {
        std::fprintf(stderr, "enactus_migrate: configuration refused: %s\n", refused.what());
        return 1;
    }
    int verdict = 1;
    try {
        verdict = run(*args, config);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "enactus_migrate: %s\n", failure.what());
        verdict = 1;
    }
    anvil::Pools::shutdown();
    (void)enactus::uninstall_services();
    if (anvil::fs::Storage::initialised()) { anvil::fs::Storage::shutdown(); }
    anvil::images::shutdown();
    return verdict;
}
