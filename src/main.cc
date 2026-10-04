#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include "anvil/db/mongo_pool.h"
#include "anvil/core/thread_pools.h"
#include <drogon/drogon.h>
#include <bsoncxx/builder/basic/document.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/options/update.hpp>
#include "security/jwt.h"

int main() {
    const char* env_uri = std::getenv("MONGODB_URI");
    std::string uri = (env_uri && std::strlen(env_uri) > 0) ? env_uri : "mongodb://127.0.0.1:27017/?replicaSet=rs0";

    const char* env_port = std::getenv("PORT");
    uint16_t port = 8085;
    if (env_port && std::strlen(env_port) > 0) {
        char* end = nullptr;
        long parsed = std::strtol(env_port, &end, 10);
        if (*end != '\0' || parsed < 1 || parsed > 65535) {
            std::cerr << "Fatal error: PORT must be a number between 1 and 65535" << std::endl;
            return 1;
        }
        port = static_cast<uint16_t>(parsed);
    }

    const char* env_bind = std::getenv("BIND_ADDR");
    std::string bind_addr = (env_bind && std::strlen(env_bind) > 0) ? env_bind : "0.0.0.0";

    const char* env_docroot = std::getenv("DOC_ROOT");
    std::string docRoot = (env_docroot && std::strlen(env_docroot) > 0) ? env_docroot : "public";

    try {
        // Refuse to boot without a strong signing key; see security/jwt.h.
        enactus::security::initJwtSecret();

        anvil::db::MongoPool::init(uri, 16);
        
        anvil::Pools::init(anvil::PoolSizes{
            .db_threads = 16,
            .db_queue = 256,
            .cpu_threads = 8,
            .cpu_queue = 64,
            .hash_threads = 8,
            .hash_queue = 32,
            .audit_threads = 1,
            .audit_queue = 32,
            .analytics_threads = 1,
            .analytics_queue = 32
        });

        std::cout << "Starting Enactus SAMS Backend on " << bind_addr << ":" << port << "..." << std::endl;
        
        extern void registerApiHandlers();
        registerApiHandlers();

        // Security headers on every response. The public site's runtime
        // (support.js) compiles its inline component script with
        // `new Function` and loads React/Babel from unpkg under SRI, so its
        // policy has to allow inline/eval; the admin panel is a plain Vite
        // bundle and gets a strict script policy. API responses are data and
        // are never rendered or cached.
        drogon::app().registerPreSendingAdvice([](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            static const std::string kApiCsp = "default-src 'none'; frame-ancestors 'none'";
            static const std::string kAdminCsp =
                "default-src 'self'; script-src 'self'; "
                "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
                "font-src 'self' https://fonts.gstatic.com; img-src 'self' data: blob: https:; "
                "connect-src 'self'; object-src 'none'; base-uri 'self'; form-action 'self'; "
                "frame-ancestors 'none'";
            static const std::string kPublicCsp =
                "default-src 'self'; script-src 'self' 'unsafe-inline' 'unsafe-eval' blob: https://unpkg.com; "
                "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
                "font-src 'self' data: https://fonts.gstatic.com; img-src 'self' data: blob: https:; "
                "connect-src 'self'; object-src 'none'; base-uri 'self'; form-action 'self'; "
                "frame-ancestors 'self'";

            const std::string& path = req->path();
            if (path.starts_with("/api/")) {
                resp->addHeader("Content-Security-Policy", kApiCsp);
                resp->addHeader("Cache-Control", "no-store");
            } else if ((path == "/admin" || path.starts_with("/admin/"))) {
                resp->addHeader("Content-Security-Policy", kAdminCsp);
            } else {
                resp->addHeader("Content-Security-Policy", kPublicCsp);
            }
            resp->addHeader("X-Frame-Options", "SAMEORIGIN");
            resp->addHeader("X-Content-Type-Options", "nosniff");
            resp->addHeader("Referrer-Policy", "strict-origin-when-cross-origin");
            resp->addHeader("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
            resp->addHeader("Cross-Origin-Opener-Policy", "same-origin");
            // Browsers ignore HSTS over plain HTTP, so this is inert until the
            // site is served over TLS, and then pins it there.
            resp->addHeader("Strict-Transport-Security", "max-age=31536000");
        });

        auto serveCompressedFile = [docRoot](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& relPath) {
            std::string path = docRoot + "/" + relPath;
            std::string ae = req->getHeader("accept-encoding");
            std::string finalPath = path;
            std::string enc = "";
            if (ae.find("br") != std::string::npos && std::filesystem::exists(path + ".br")) {
                finalPath = path + ".br";
                enc = "br";
            } else if (ae.find("gzip") != std::string::npos && std::filesystem::exists(path + ".gz")) {
                finalPath = path + ".gz";
                enc = "gzip";
            }
            auto resp = drogon::HttpResponse::newFileResponse(finalPath, "", drogon::CT_NONE, "", req);
            if (!enc.empty()) {
                resp->addHeader("Content-Encoding", enc);
                if (path.ends_with(".html")) resp->setContentTypeCode(drogon::CT_TEXT_HTML);
                else if (path.ends_with(".js")) resp->setContentTypeCode(drogon::CT_TEXT_JAVASCRIPT);
                else if (path.ends_with(".css")) resp->setContentTypeCode(drogon::CT_TEXT_CSS);
            }
            callback(resp);
        };

        drogon::app().registerHandler("/", [serveCompressedFile](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            anvil::Pools::db().try_post(anvil::guarded("db", [] {
                try {
                    auto client = anvil::db::MongoPool::instance().acquire();
                    auto collection = (*client)["application"]["analytics"];
                    using bsoncxx::builder::basic::kvp;
                    using bsoncxx::builder::basic::make_document;

                    auto now = std::chrono::system_clock::now();
                    std::time_t tt = std::chrono::system_clock::to_time_t(now);
                    std::tm tm_buf;
                    gmtime_r(&tt, &tm_buf);
                    char date_str[32];
                    std::strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_buf);

                    collection.update_one(
                        make_document(kvp("_id", "visits")),
                        make_document(kvp("$inc", make_document(
                            kvp("count", (int64_t)1),
                            kvp(std::string("daily.") + date_str, (int64_t)1)
                        ))),
                        mongocxx::options::update{}.upsert(true)
                    );
                } catch (...) {}
            }));
            serveCompressedFile(req, std::move(callback), "index.html");
        });
        drogon::app().registerHandler("/admin", [serveCompressedFile](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            serveCompressedFile(req, std::move(callback), "admin/index.html");
        });
        drogon::app().registerHandler("/admin/", [serveCompressedFile](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            serveCompressedFile(req, std::move(callback), "admin/index.html");
        });
        drogon::app().registerHandler("/apply", [serveCompressedFile](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            serveCompressedFile(req, std::move(callback), "form.html");
        });
        drogon::app().registerHandler("/apply/", [serveCompressedFile](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            serveCompressedFile(req, std::move(callback), "form.html");
        });

        drogon::app()
            .setLogPath("")
            .setLogLevel(trantor::Logger::kInfo)
            .addListener(bind_addr, port)
            .setThreadNum(16)
            .setDocumentRoot(docRoot)
            .enableGzip(true)
            .enableBrotli(true)
            .setGzipStatic(true)
            .setBrStatic(true)
            // 15 MB image uploads arrive base64-encoded (~20 MB).
            .setClientMaxBodySize(21 * 1024 * 1024)
            .setClientMaxMemoryBodySize(21 * 1024 * 1024)
            .enableServerHeader(false)
            .run();
            
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
