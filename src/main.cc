#include <iostream>
#include <string>
#include "anvil/db/mongo_pool.h"
#include "anvil/core/thread_pools.h"
#include <drogon/drogon.h>

int main() {
    std::string uri = "mongodb://127.0.0.1:27017/?replicaSet=rs0";
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

        std::cout << "Starting Enactus SAMS Backend..." << std::endl;
        
        extern void registerApiHandlers();
        registerApiHandlers();
        
#include <filesystem>

        auto serveCompressedFile = [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& path) {
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
                    collection.update_one(
                        make_document(kvp("_id", "visits")),
                        make_document(kvp("$inc", make_document(kvp("count", (int64_t)1)))),
                        mongocxx::options::update{}.upsert(true)
                    );
                } catch (...) {}
            }));
            serveCompressedFile(req, std::move(callback), "public/index.html");
        });
        drogon::app().registerHandler("/admin", [serveCompressedFile](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            serveCompressedFile(req, std::move(callback), "public/admin/index.html");
        });
        drogon::app().registerHandler("/admin/", [serveCompressedFile](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            serveCompressedFile(req, std::move(callback), "public/admin/index.html");
        });
        drogon::app().registerHandler("/apply", [serveCompressedFile](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            serveCompressedFile(req, std::move(callback), "public/form.html");
        });
        drogon::app().registerHandler("/apply/", [serveCompressedFile](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            serveCompressedFile(req, std::move(callback), "public/form.html");
        });

        drogon::app()
            .setLogPath("")
            .setLogLevel(trantor::Logger::kInfo)
            .addListener("0.0.0.0", 8080)
            .setThreadNum(16)
            .setDocumentRoot("public")
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
