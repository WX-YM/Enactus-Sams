// The Enactus SAMS backend: configuration, then anvil's services in dependency
// order, then the routes, then the listener. Every refusal before the listener
// opens is an exit with a message, never a degraded server.

#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include "anvil/accesscontrol/access_filter.h"
#include "anvil/accesscontrol/stealth.h"
#include "anvil/core/thread_pools.h"
#include "anvil/db/mongo_pool.h"
#include "anvil/fs/paths.h"
#include "anvil/http/client_address.h"
#include "anvil/http/origin_check.h"
#include "anvil/http/request_scope.h"
#include "anvil/images/probe.h"
#include "anvil/redis/redis_client.h"
#include "app/config.h"
#include "app/router.h"
#include "app/services.h"
#include "routes.h"

namespace {

namespace ac = anvil::accesscontrol;

// The browser-facing headers. The public site's runtime compiles its inline
// component script with `new Function`, using React and Babel served from
// /vendor (same origin, pinned by SRI), so its policy has to allow inline and
// eval; the admin panel is a built bundle and gets a strict script policy. API
// answers are data and are never framed.
void install_security_headers() {
    drogon::app().registerPreSendingAdvice([](const drogon::HttpRequestPtr& req,
                                              const drogon::HttpResponsePtr& resp) {
        static const std::string kApiCsp = "default-src 'none'; frame-ancestors 'none'";
        static const std::string kAdminCsp =
            "default-src 'self'; script-src 'self'; "
            "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
            "font-src 'self' https://fonts.gstatic.com; img-src 'self' data: blob:; "
            "connect-src 'self'; object-src 'none'; base-uri 'self'; form-action 'self'; "
            "frame-ancestors 'none'";
        static const std::string kPublicCsp =
            "default-src 'self'; script-src 'self' 'unsafe-inline' 'unsafe-eval' blob:; "
            "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
            "font-src 'self' data: https://fonts.gstatic.com; img-src 'self' data: blob:; "
            "connect-src 'self'; object-src 'none'; base-uri 'self'; form-action 'self'; "
            "frame-ancestors 'self'";

        const std::string& path = req->path();
        if (path.starts_with("/api/") || path.starts_with("/media/")) {
            resp->addHeader("Content-Security-Policy", kApiCsp);
        } else if (path == "/admin" || path.starts_with("/admin/")) {
            resp->addHeader("Content-Security-Policy", kAdminCsp);
        } else {
            resp->addHeader("Content-Security-Policy", kPublicCsp);
        }
        resp->addHeader("X-Frame-Options", "SAMEORIGIN");
        resp->addHeader("X-Content-Type-Options", "nosniff");
        resp->addHeader("Referrer-Policy", "strict-origin-when-cross-origin");
        resp->addHeader("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
        resp->addHeader("Cross-Origin-Opener-Policy", "same-origin");
        // Inert over plain HTTP; pins the site to TLS once it is served over it.
        resp->addHeader("Strict-Transport-Security", "max-age=31536000");
    });
}

// The three pages that are not files at their own path.
void install_pages(const std::string& doc_root) {
    const auto page = [doc_root](std::string file) {
        return [doc_root, file = std::move(file)](const drogon::HttpRequestPtr& req,
                                                  std::function<void(const drogon::HttpResponsePtr&)>&& respond) {
            auto response = drogon::HttpResponse::newFileResponse(doc_root + "/" + file, "", drogon::CT_TEXT_HTML,
                                                                  "", req);
            response->addHeader("Cache-Control", "no-cache");
            respond(response);
        };
    };
    drogon::app().registerHandler("/", page("index.html"), {drogon::Get});
    drogon::app().registerHandler("/apply", page("form.html"), {drogon::Get});
    drogon::app().registerHandler("/admin", page("admin/index.html"), {drogon::Get});
    drogon::app().registerHandler("/admin/", page("admin/index.html"), {drogon::Get});
}

// Submission counters are batched in memory by anvil and written here once a
// second, off the request path (anvil docs/13-dynamic-forms.md §4).
void schedule_count_flush() {
    drogon::app().getLoop()->runEvery(1.0, [] {
        (void)anvil::Pools::db().try_post(anvil::guarded("db", [] {
            auto client = anvil::db::MongoPool::instance().acquire();
            enactus::services().submissions.flush_counts(
                [&client](std::span<const std::pair<anvil::Uuid, std::int64_t>> counts) {
                    return enactus::services().forms.repository().add_submission_counts(*client, counts);
                });
        }));
    });
}

int boot(const enactus::Config& config) {
    anvil::Pools::init(anvil::PoolSizes{.db_threads = config.db_threads,
                                        .db_queue = config.db_threads * 16,
                                        .cpu_threads = 2,
                                        .cpu_queue = 16,
                                        // 64 MiB per Argon2 hash. Sign-in needs none
                                        // (the browser prehashes); only the one-off
                                        // plaintext enrolment path does.
                                        .hash_threads = config.hash_threads,
                                        .hash_queue = 8,
                                        .audit_threads = 1,
                                        .audit_queue = 32,
                                        .analytics_threads = 1,
                                        .analytics_queue = 32});

    anvil::db::MongoPool::init(config.mongodb_uri, config.db_threads + 4);
    {
        auto probe = anvil::db::MongoPool::instance().acquire();
        (*probe)["admin"].run_command(
            bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("ping", 1)));
    }
    try {
        anvil::redis::RedisClient::init(anvil::redis::RedisConfig{
            .url = config.redis_url, .pool_size = 8, .connect_timeout_ms = 1000, .socket_timeout_ms = 1000});
    } catch (const std::exception&) {
        // The health probe below gives one message for a bad URL and a down server.
    }
    if (!anvil::redis::RedisClient::healthy()) {
        std::fprintf(stderr, "enactus_backend: Redis is not reachable at REDIS_URL. Sessions, "
                             "revocation and rate limits live there; refusing to start without it.\n");
        return 3;
    }

    anvil::images::init("enactus_backend");
    anvil::fs::Storage::init(config.storage_root);

    {
        auto proxies = std::make_shared<anvil::http::TrustedProxies>();
        if (!config.trusted_proxies.empty() && !proxies->parse(config.trusted_proxies)) {
            std::fprintf(stderr, "enactus_backend: TRUSTED_PROXIES does not parse\n");
            return 1;
        }
        anvil::http::install_trusted_proxies(std::move(proxies));
        auto origins = std::make_shared<anvil::http::AllowedOrigins>();
        if (!origins->parse(config.site_origin)) {
            std::fprintf(stderr, "enactus_backend: SITE_ORIGIN does not parse\n");
            return 1;
        }
        anvil::http::install_allowed_origins(std::move(origins));
    }

    enactus::install_services(std::make_unique<enactus::Services>(config));
    enactus::Services& s = enactus::services();

    {
        auto client = anvil::db::MongoPool::instance().acquire();
        // Warm the section cache so no visitor ever pays a cold read.
        const auto preloaded = s.sections.preload(*client);
        if (!preloaded) {
            std::fprintf(stderr, "enactus_backend: the section cache could not be filled; has "
                                 "enactus_migrate been run against this database?\n");
            return 1;
        }
    }

    s.audit.start();
    s.events.start();
    s.sections.start_invalidation_listener();
    s.entries.start_invalidation_listener();
    if (!s.jobs.ensure_group()) {
        std::fprintf(stderr, "enactus_backend: the job queue could not be created in Redis\n");
        return 3;
    }
    s.jobs.start();

    ac::AccessControl::init(ac::AccessControlDeps{
        .keys = s.token_keys,
        .epochs = &s.authz,
        .denials = &s.audit,
        .routes = enactus::kRoutes,
    });
    ac::install_as_framework_404();
    anvil::http::install_request_scope();
    enactus::install_routes();
    install_pages(config.doc_root);
    install_security_headers();
    schedule_count_flush();
    return 0;
}

void shutdown() {
    if (enactus::services_installed()) {
        enactus::Services& s = enactus::services();
        s.jobs.stop();
        s.entries.stop_invalidation_listener();
        s.sections.stop_invalidation_listener();
        s.submissions.stop();
        s.events.stop();
        s.audit.stop();
    }
    anvil::Pools::shutdown();
    (void)enactus::uninstall_services();
    if (anvil::fs::Storage::initialised()) { anvil::fs::Storage::shutdown(); }
    anvil::images::shutdown();
}

}  // namespace

int main() {
    enactus::Config config;
    try {
        config = enactus::load_config(anvil::config::system_environment());
    } catch (const std::exception& refused) {
        std::fprintf(stderr, "enactus_backend: configuration refused: %s\n", refused.what());
        return 1;
    }
    std::fprintf(stderr, "%s", enactus::summarise(config).c_str());

    try {
        if (const int verdict = boot(config); verdict != 0) { return verdict; }
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "enactus_backend: boot failed: %s\n", failure.what());
        return 1;
    }

    drogon::app()
        .setLogPath("")
        .setLogLevel(trantor::Logger::kWarn)
        .addListener(config.bind_addr, config.port)
        .setThreadNum(config.http_threads)
        .setDocumentRoot(config.doc_root)
        // Only files with a known static extension are served from DOC_ROOT.
        .setFileTypes({"html", "js", "css", "png", "jpg", "jpeg", "webp", "avif", "svg", "ico", "woff",
                       "woff2", "txt", "json", "map"})
        .enableGzip(true)
        .enableBrotli(true)
        .setGzipStatic(true)
        .setBrStatic(true)
        .setClientMaxBodySize(anvil::images::kMaxBytes + 1024 * 1024)
        .setClientMaxMemoryBodySize(anvil::images::kMaxBytes + 1024 * 1024)
        .enableServerHeader(false)
        .enableDateHeader(true)
        .run();

    shutdown();
    return 0;
}
