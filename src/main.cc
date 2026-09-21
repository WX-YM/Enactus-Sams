#include <iostream>
#include <string>
#include "anvil/db/mongo_pool.h"
#include "anvil/core/thread_pools.h"
#include <drogon/drogon.h>

int main() {
    const char* env_uri = std::getenv("MONGODB_URI");
    std::string uri = (env_uri && std::strlen(env_uri) > 0) ? env_uri : "mongodb://127.0.0.1:27017/?replicaSet=rs0";

    const char* env_port = std::getenv("PORT");
    uint16_t port = (env_port && std::strlen(env_port) > 0) ? static_cast<uint16_t>(std::stoi(env_port)) : 8085;

    const char* env_bind = std::getenv("BIND_ADDR");
    std::string bind_addr = (env_bind && std::strlen(env_bind) > 0) ? env_bind : "0.0.0.0";

    const char* env_docroot = std::getenv("DOC_ROOT");
    std::string docRoot = (env_docroot && std::strlen(env_docroot) > 0) ? env_docroot : "public";

    try {
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

        drogon::app().registerPreSendingAdvice([](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
            resp->addHeader("X-Frame-Options", "SAMEORIGIN");
            resp->addHeader("X-Content-Type-Options", "nosniff");
            resp->addHeader("X-XSS-Protection", "1; mode=block");
            resp->addHeader("Referrer-Policy", "strict-origin-when-cross-origin");
            resp->addHeader("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
        });
        
#include <filesystem>

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
            .setClientMaxBodySize(50 * 1024 * 1024)
            .setClientMaxMemoryBodySize(50 * 1024 * 1024)
            .run();
            
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
