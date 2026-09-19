#include <drogon/drogon.h>
#include <drogon/MultiPart.h>
#include <fstream>
#include <chrono>
#include <json/json.h>
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>

#include "anvil/accesscontrol/route_registration.h"
#include "anvil/core/thread_pools.h"
#include "anvil/db/mongo_pool.h"
#include "routes.h"

using namespace drogon;

namespace enactus {

void on_db(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)> callback,
           std::function<HttpResponsePtr(mongocxx::client&)> work) {
    const bool posted = anvil::Pools::db().try_post(anvil::guarded("db", [req, callback, work] {
        auto client = anvil::db::MongoPool::instance().acquire();
        callback(work(*client));
    }));
    if (!posted) {
        Json::Value ret;
        ret["error"] = "Service Unavailable";
        callback(HttpResponse::newHttpJsonResponse(ret));
    }
}


static void logSystemEvent(mongocxx::client& client, const std::string& action, const std::string& details, const std::string& type = "system") {
    try {
        auto logsColl = client["application"]["logs"];
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        logsColl.insert_one(make_document(
            kvp("timestamp", now_ms),
            kvp("action", action),
            kvp("details", details),
            kvp("type", type)
        ));
    } catch (...) {}
}

void login(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) { callback(HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN)); return; }
    
    std::string email = (*json)["email"].asString();
    std::string password = (*json)["password"].asString();

    on_db(req, std::move(callback), [email, password](mongocxx::client& client) {
        auto collection = client["application"]["users"];
        auto doc = collection.find_one(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("email", email)));
        
        if (doc) {
            auto view = doc->view();
            std::string stored_pass = std::string(view["password"].get_string().value);
            std::string role = std::string(view["role"].get_string().value);
            
            if (stored_pass == password) {
                logSystemEvent(client, "User Login", "User " + email + " (" + role + ") logged into admin panel", "auth");
                Json::Value ret;
                ret["status"] = "ok";
                ret["token"] = "mock_jwt_token_for_" + email;
                ret["role"] = role;
                if (view["team"]) {
                    ret["team"] = std::string(view["team"].get_string().value);
                } else {
                    ret["team"] = "";
                }
                if (view["permissions"]) {
                    Json::Value permsArr(Json::arrayValue);
                    for (auto& p : view["permissions"].get_array().value) {
                        permsArr.append(std::string(p.get_string().value));
                    }
                    ret["permissions"] = permsArr;
                } else {
                    ret["permissions"] = Json::arrayValue;
                }
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                return resp;
            }
        }
        
        logSystemEvent(client, "Login Failed", "Failed login attempt for " + email, "auth");
        Json::Value ret;
        ret["status"] = "error";
        ret["message"] = "Invalid email or password";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        return resp;
    });
}

void listUsers(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["users"];
        auto cursor = collection.find({});
        
        Json::Value users(Json::arrayValue);
        for (auto&& doc : cursor) {
            std::string jsonStr = bsoncxx::to_json(doc);
            Json::CharReaderBuilder builder;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            Json::Value jsonDoc;
            reader->parse(jsonStr.c_str(), jsonStr.c_str() + jsonStr.length(), &jsonDoc, nullptr);
            // Hide password
            jsonDoc.removeMember("password");
            users.append(jsonDoc);
        }
        
        Json::Value ret;
        ret["users"] = users;
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void createUser(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) { callback(HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN)); return; }
    
    std::string email    = (*json)["email"].asString();
    std::string password = (*json).isMember("password") ? (*json)["password"].asString() : "";
    std::string role     = (*json)["role"].asString();
    std::string team     = (*json).isMember("team") ? (*json)["team"].asString() : "";
    std::string action   = (*json).isMember("action") ? (*json)["action"].asString() : "";
    Json::Value perms    = (*json)["permissions"];

    if (email == "admin@enactussams.org" && action == "update") {
        Json::Value ret;
        ret["status"] = "error";
        ret["message"] = "Super admin account cannot be modified";
        callback(HttpResponse::newHttpJsonResponse(ret));
        return;
    }

    std::string permsJson = perms.isArray() ? perms.toStyledString() : "[]";

    on_db(req, std::move(callback), [email, password, role, team, action, permsJson](mongocxx::client& client) {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        auto collection = client["application"]["users"];
        auto teamsColl = client["application"]["teams"];

        bsoncxx::builder::basic::array permsArr{};
        Json::CharReaderBuilder rb; std::string errs;
        Json::Value pv;
        std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
        reader->parse(permsJson.c_str(), permsJson.c_str() + permsJson.size(), &pv, &errs);
        if (pv.isArray()) {
            for (auto& p : pv) permsArr.append(p.asString());
        }

        if (action == "update") {
            auto existingDoc = collection.find_one(make_document(kvp("email", email)));
            if (existingDoc) {
                auto view = existingDoc->view();
                std::string oldTeam = view["team"] ? std::string(view["team"].get_string().value) : "";
                std::string oldRole = view["role"] ? std::string(view["role"].get_string().value) : "";

                if (!oldTeam.empty() && (oldTeam != team || oldRole != role)) {
                    std::string oldField = (oldRole == "vice manager" || oldRole == "vice_manager") ? "viceManager" : "manager";
                    teamsColl.update_one(
                        make_document(kvp("name", oldTeam)),
                        make_document(kvp("$unset", make_document(kvp(oldField, ""))))
                    );
                }
            }

            bsoncxx::builder::basic::document updateFields{};
            updateFields.append(kvp("role", role));
            updateFields.append(kvp("team", team));
            updateFields.append(kvp("permissions", permsArr.extract()));
            if (!password.empty()) {
                updateFields.append(kvp("password", password));
            }

            collection.update_one(
                make_document(kvp("email", email)),
                make_document(kvp("$set", updateFields.extract()))
            );

            if (!team.empty() && (role == "manager" || role == "vice manager" || role == "vice_manager")) {
                std::string fieldName = (role == "vice manager" || role == "vice_manager") ? "viceManager" : "manager";
                teamsColl.update_one(
                    make_document(kvp("name", team)),
                    make_document(kvp("$set", make_document(kvp(fieldName, email))))
                );
            }

            logSystemEvent(client, "User Updated", "User " + email + " updated (role: " + role + (team.empty() ? "" : ", team: " + team) + ")", "auth");
            Json::Value ret;
            ret["status"] = "ok";
            return HttpResponse::newHttpJsonResponse(ret);
        }

        bsoncxx::builder::basic::document doc{};
        doc.append(kvp("email", email));
        doc.append(kvp("password", password));
        doc.append(kvp("role", role));
        if (!team.empty()) {
            doc.append(kvp("team", team));
        }
        doc.append(kvp("permissions", permsArr.extract()));
        collection.insert_one(doc.view());

        if (!team.empty() && (role == "manager" || role == "vice manager" || role == "vice_manager")) {
            std::string fieldName = (role == "vice manager" || role == "vice_manager") ? "viceManager" : "manager";
            teamsColl.update_one(
                make_document(kvp("name", team)),
                make_document(kvp("$set", make_document(kvp(fieldName, email))))
            );
        }

        logSystemEvent(client, "User Created", "New user " + email + " created with role " + role, "auth");
        Json::Value ret;
        ret["status"] = "ok";
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void deleteUser(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) { callback(HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN)); return; }
    
    std::string email = (*json)["email"].asString();

    if (email == "admin@enactussams.org") {
        Json::Value ret;
        ret["status"] = "error";
        ret["message"] = "Super admin account cannot be deleted";
        callback(HttpResponse::newHttpJsonResponse(ret));
        return;
    }

    on_db(req, std::move(callback), [email](mongocxx::client& client) {
        auto collection = client["application"]["users"];
        // Check if this user was assigned to any team as manager or vice manager
        auto userDoc = collection.find_one(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("email", email)));
        if (userDoc) {
            auto view = userDoc->view();
            if (view["team"]) {
                std::string teamName = std::string(view["team"].get_string().value);
                std::string role = view["role"] ? std::string(view["role"].get_string().value) : "";
                auto teamsColl = client["application"]["teams"];
                using bsoncxx::builder::basic::kvp;
                using bsoncxx::builder::basic::make_document;
                if (role == "manager") {
                    teamsColl.update_one(
                        make_document(kvp("name", teamName)),
                        make_document(kvp("$unset", make_document(kvp("manager", ""))))
                    );
                } else if (role == "vice manager" || role == "vice_manager") {
                    teamsColl.update_one(
                        make_document(kvp("name", teamName)),
                        make_document(kvp("$unset", make_document(kvp("viceManager", ""))))
                    );
                }
            }
        }

        collection.delete_one(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("email", email)));
        logSystemEvent(client, "User Revoked", "User " + email + " access revoked", "auth");

        Json::Value ret;
        ret["status"] = "ok";
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

// Replace old login placeholder

void me(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    Json::Value ret;
    ret["status"] = "ok";
    ret["role"] = "superadmin";
    auto resp = HttpResponse::newHttpJsonResponse(ret);
    callback(resp);
}

void apply(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) {
        callback(HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN));
        return;
    }

    std::string name = (*json)["name"].asString();
    std::string team = (*json)["team"].asString();
    std::string reason = (*json)["reason"].asString();

    on_db(req, std::move(callback), [name, team, reason](mongocxx::client& client) {
        auto collection = client["application"]["applications"];
        bsoncxx::builder::basic::document doc{};
        doc.append(bsoncxx::builder::basic::kvp("name", name));
        doc.append(bsoncxx::builder::basic::kvp("team", team));
        doc.append(bsoncxx::builder::basic::kvp("reason", reason));
        doc.append(bsoncxx::builder::basic::kvp("status", "pending"));

        collection.insert_one(doc.view());
        logSystemEvent(client, "New Application", "New applicant " + name + " applied for " + team, "application");

        Json::Value ret;
        ret["status"] = "ok";
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void listApplications(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["applications"];
        auto cursor = collection.find({});

        Json::Value arr = Json::arrayValue;
        for (auto&& doc : cursor) {
            Json::Value item;
            std::string json_str = bsoncxx::to_json(doc);
            Json::CharReaderBuilder builder;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            std::string errs;
            reader->parse(json_str.c_str(), json_str.c_str() + json_str.length(), &item, &errs);
            arr.append(item);
        }

        Json::Value ret;
        ret["applications"] = arr;
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void updateApplicationStatus(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) { callback(HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN)); return; }

    std::string id_str = (*json)["id"].asString();
    std::string action = (*json).isMember("action") ? (*json)["action"].asString() : "";
    std::string status  = (*json).isMember("status") ? (*json)["status"].asString() : "";
    std::string reason  = (*json).isMember("reason") ? (*json)["reason"].asString() : "";
    std::string referred_to = (*json).isMember("referredTo") ? (*json)["referredTo"].asString() : "";
    std::string team_override = (*json).isMember("team") ? (*json)["team"].asString() : "";

    on_db(req, std::move(callback), [id_str, action, status, reason, referred_to, team_override](mongocxx::client& client) {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;

        bsoncxx::oid oid(id_str);
        auto collection = client["application"]["applications"];

        if (action == "delete") {
            collection.delete_one(make_document(kvp("_id", oid)));
            logSystemEvent(client, "Application Deleted", "Application " + id_str + " was deleted", "application");
            Json::Value ret;
            ret["status"] = "ok";
            return HttpResponse::newHttpJsonResponse(ret);
        }

        bsoncxx::builder::basic::document update_doc{};
        if (!status.empty()) update_doc.append(kvp("status", status));
        if (!reason.empty()) update_doc.append(kvp("reason", reason));
        if (!referred_to.empty()) update_doc.append(kvp("referredTo", referred_to));

        std::string applicantName = "";
        std::string targetTeam = team_override;

        // When accepting an applicant, automatically add them to the team's card in Manage Teams
        if (status == "accepted") {
            auto appDoc = collection.find_one(make_document(kvp("_id", oid)));
            if (appDoc) {
                auto appView = appDoc->view();
                applicantName = appView["name"] ? std::string(appView["name"].get_string().value) : "";
                if (targetTeam.empty()) {
                    if (appView["referredTo"] && !std::string(appView["referredTo"].get_string().value).empty()) {
                        targetTeam = std::string(appView["referredTo"].get_string().value);
                    } else if (appView["team"]) {
                        targetTeam = std::string(appView["team"].get_string().value);
                    }
                }

                if (!targetTeam.empty()) {
                    update_doc.append(kvp("team", targetTeam));

                    auto teamsColl = client["application"]["teams"];
                    auto teamDoc = teamsColl.find_one(make_document(kvp("name", bsoncxx::types::b_regex{"^" + targetTeam + "$", "i"})));
                    if (teamDoc) {
                        auto teamView = teamDoc->view();
                        std::string realTeamName = std::string(teamView["name"].get_string().value);
                        bool alreadyMember = false;
                        if (teamView["memberList"]) {
                            for (auto&& m : teamView["memberList"].get_array().value) {
                                auto mDoc = m.get_document().value;
                                if (mDoc["name"] && std::string(mDoc["name"].get_string().value) == applicantName) {
                                    alreadyMember = true;
                                    break;
                                }
                            }
                        }

                        if (!alreadyMember && !applicantName.empty()) {
                            auto newMember = make_document(
                                kvp("id", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
                                kvp("name", applicantName),
                                kvp("role", "Member")
                            );
                            teamsColl.update_one(
                                make_document(kvp("name", realTeamName)),
                                make_document(
                                    kvp("$push", make_document(kvp("memberList", newMember))),
                                    kvp("$inc", make_document(kvp("members", 1)))
                                )
                            );
                        }
                    }
                }
            }
        }

        collection.update_one(
            make_document(kvp("_id", oid)),
            make_document(kvp("$set", update_doc.extract()))
        );

        if (status == "accepted") {
            logSystemEvent(client, "Application Accepted", (applicantName.empty() ? ("Application " + id_str) : applicantName) + " accepted into " + targetTeam, "application");
        } else if (status == "rejected") {
            logSystemEvent(client, "Application Rejected", "Application " + id_str + " rejected" + (reason.empty() ? "" : ": " + reason), "application");
        } else if (status == "referred") {
            logSystemEvent(client, "Application Referred", "Application " + id_str + " referred to " + referred_to, "application");
        }

        Json::Value ret;
        ret["status"] = "ok";
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void listTeams(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["teams"];
        auto cursor = collection.find({});

        Json::Value arr = Json::arrayValue;
        for (auto&& doc : cursor) {
            Json::Value item;
            std::string json_str = bsoncxx::to_json(doc);
            Json::CharReaderBuilder builder;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            std::string errs;
            reader->parse(json_str.c_str(), json_str.c_str() + json_str.length(), &item, &errs);
            arr.append(item);
        }

        Json::Value ret;
        ret["teams"] = arr;
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void createTeam(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) {
        callback(HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN));
        return;
    }

    if ((*json).isMember("action") && (*json)["action"].asString() == "delete") {
        std::string teamId = (*json).isMember("id") ? (*json)["id"].asString() : "";
        std::string teamName = (*json).isMember("name") ? (*json)["name"].asString() : "";

        on_db(req, std::move(callback), [teamId, teamName](mongocxx::client& client) {
            auto collection = client["application"]["teams"];
            using bsoncxx::builder::basic::kvp;
            using bsoncxx::builder::basic::make_document;

            bool deleted = false;
            std::string deletedName = teamName;
            if (!teamId.empty() && teamId.length() == 24) {
                try {
                    bsoncxx::oid oid(teamId);
                    auto tDoc = collection.find_one(make_document(kvp("_id", oid)));
                    if (tDoc && tDoc->view()["name"]) {
                        deletedName = std::string(tDoc->view()["name"].get_string().value);
                    }
                    collection.delete_one(make_document(kvp("_id", oid)));
                    deleted = true;
                } catch (...) {}
            }
            if (!deleted && !teamName.empty()) {
                collection.delete_one(make_document(kvp("name", bsoncxx::types::b_regex{"^" + teamName + "$", "i"})));
            }

            logSystemEvent(client, "Team Deleted", "Team " + (deletedName.empty() ? teamId : deletedName) + " removed from the club", "team");
            Json::Value ret;
            ret["status"] = "ok";
            return HttpResponse::newHttpJsonResponse(ret);
        });
        return;
    }

    if ((*json).isMember("action") && (*json)["action"].asString() == "update_roster") {
        std::string teamId = (*json).isMember("id") ? (*json)["id"].asString() : "";
        std::string teamName = (*json).isMember("name") ? (*json)["name"].asString() : "";
        int members = (*json).isMember("members") ? (*json)["members"].asInt() : 0;
        Json::Value memberList = (*json)["memberList"];

        std::string memberListJson = memberList.isArray() ? memberList.toStyledString() : "[]";

        on_db(req, std::move(callback), [teamId, teamName, members, memberListJson](mongocxx::client& client) {
            auto collection = client["application"]["teams"];
            using bsoncxx::builder::basic::kvp;
            using bsoncxx::builder::basic::make_document;

            bsoncxx::builder::basic::array membersArr{};
            Json::CharReaderBuilder rb; std::string errs;
            Json::Value mv;
            std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
            reader->parse(memberListJson.c_str(), memberListJson.c_str() + memberListJson.size(), &mv, &errs);
            if (mv.isArray()) {
                for (auto& m : mv) {
                    bsoncxx::builder::basic::document mDoc{};
                    mDoc.append(kvp("id", m.isMember("id") ? m["id"].asInt64() : 0));
                    mDoc.append(kvp("name", m.isMember("name") ? m["name"].asString() : ""));
                    mDoc.append(kvp("role", m.isMember("role") ? m["role"].asString() : "Member"));
                    membersArr.append(mDoc.extract());
                }
            }

            bool updated = false;
            try {
                if (!teamId.empty() && teamId.length() == 24) {
                    bsoncxx::oid oid(teamId);
                    collection.update_one(
                        make_document(kvp("_id", oid)),
                        make_document(kvp("$set", make_document(
                            kvp("members", members),
                            kvp("memberList", membersArr.extract())
                        )))
                    );
                    updated = true;
                }
            } catch (...) {}

            if (!updated && !teamName.empty()) {
                collection.update_one(
                    make_document(kvp("name", teamName)),
                    make_document(kvp("$set", make_document(
                        kvp("members", members),
                        kvp("memberList", membersArr.extract())
                    )))
                );
            }

            logSystemEvent(client, "Roster Updated", "Team " + teamName + " roster updated (" + std::to_string(members) + " members)", "team");
            Json::Value ret;
            ret["status"] = "ok";
            return HttpResponse::newHttpJsonResponse(ret);
        });
        return;
    }

    std::string name = (*json)["name"].asString();
    std::string desc = (*json)["desc"].asString();

    on_db(req, std::move(callback), [name, desc](mongocxx::client& client) {
        auto collection = client["application"]["teams"];
        bsoncxx::builder::basic::document doc{};
        doc.append(bsoncxx::builder::basic::kvp("name", name));
        doc.append(bsoncxx::builder::basic::kvp("desc", desc));
        doc.append(bsoncxx::builder::basic::kvp("members", 0));

        collection.insert_one(doc.view());
        logSystemEvent(client, "Team Created", "New team " + name + " added to the club", "team");

        Json::Value ret;
        ret["status"] = "ok";
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void getContent(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["content"];
        auto doc = collection.find_one({});
        Json::Value ret;
        if (doc) {
            std::string jsonStr = bsoncxx::to_json(*doc);
            Json::CharReaderBuilder builder;
            std::string errs;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            Json::Value jsonDoc;
            reader->parse(jsonStr.c_str(), jsonStr.c_str() + jsonStr.length(), &jsonDoc, &errs);
            ret["content"] = jsonDoc;
        } else {
            ret["content"] = Json::objectValue;
        }
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void setContent(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) {
        callback(HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN));
        return;
    }

    std::string jsonStr = json->toStyledString();
    
    on_db(req, std::move(callback), [jsonStr](mongocxx::client& client) {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        auto collection = client["application"]["content"];
        
        auto update_doc = bsoncxx::from_json(jsonStr);
        collection.update_one(
            make_document(),
            make_document(kvp("$set", update_doc.view())),
            mongocxx::options::update{}.upsert(true)
        );

        logSystemEvent(client, "Content Updated", "Website content updated in Content CMS", "content");
        Json::Value ret;
        ret["status"] = "ok";
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void trackVisit(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["analytics"];
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        // Upsert: increment visit count in a singleton doc
        collection.update_one(
            make_document(kvp("_id", "visits")),
            make_document(kvp("$inc", make_document(kvp("count", (int64_t)1)))),
            mongocxx::options::update{}.upsert(true)
        );
        Json::Value ret;
        ret["status"] = "ok";
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void getAnalytics(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["analytics"];
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        auto doc = collection.find_one(make_document(kvp("_id", "visits")));
        Json::Value ret;
        int64_t visits = 0;
        if (doc && doc->view()["count"]) {
            auto elem = doc->view()["count"];
            if (elem.type() == bsoncxx::type::k_int64) {
                visits = elem.get_int64().value;
            } else if (elem.type() == bsoncxx::type::k_int32) {
                visits = elem.get_int32().value;
            } else if (elem.type() == bsoncxx::type::k_double) {
                visits = static_cast<int64_t>(elem.get_double().value);
            }
        }
        ret["visits"] = static_cast<Json::Value::Int64>(visits);
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void listLogs(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto logsColl = client["application"]["logs"];
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;

        mongocxx::options::find opts{};
        opts.sort(make_document(kvp("timestamp", -1)));
        opts.limit(50);

        auto cursor = logsColl.find({}, opts);
        Json::Value arr(Json::arrayValue);

        for (auto&& doc : cursor) {
            Json::Value item;
            item["id"] = doc["_id"].get_oid().value.to_string();
            item["timestamp"] = doc["timestamp"] ? static_cast<Json::Value::Int64>(doc["timestamp"].get_int64().value) : 0;
            item["action"] = doc["action"] ? std::string(doc["action"].get_string().value) : "";
            item["details"] = doc["details"] ? std::string(doc["details"].get_string().value) : "";
            item["type"] = doc["type"] ? std::string(doc["type"].get_string().value) : "system";
            arr.append(item);
        }

        Json::Value ret;
        ret["logs"] = arr;
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

static std::string base64_decode_str(const std::string &in) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++)
        T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

void uploadFile(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    std::string savedUrl = "";
    if (req->getContentType() == CT_MULTIPART_FORM_DATA) {
        drogon::MultiPartParser parser;
        if (parser.parse(req) == 0 && !parser.getFiles().empty()) {
            auto &file = parser.getFiles()[0];
            std::string ext = "";
            auto pos = file.getFileName().find_last_of('.');
            if (pos != std::string::npos) ext = file.getFileName().substr(pos);
            std::string fname = "img_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ext;
            std::ofstream out("./public/assets/uploads/" + fname, std::ios::binary);
            if (out) {
                out.write(file.fileData(), file.fileLength());
                out.close();
                savedUrl = "/assets/uploads/" + fname;
            }
        }
    } else if (auto json = req->getJsonObject()) {
        std::string data = (*json)["data"].asString();
        std::string name = (*json)["filename"].asString();
        auto comma = data.find(',');
        if (comma != std::string::npos) data = data.substr(comma + 1);
        std::string decoded = base64_decode_str(data);
        std::string ext = ".png";
        auto pos = name.find_last_of('.');
        if (pos != std::string::npos) ext = name.substr(pos);
        std::string fname = "img_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ext;
        std::ofstream out("./public/assets/uploads/" + fname, std::ios::binary);
        if (out) {
            out.write(decoded.data(), decoded.size());
            out.close();
            savedUrl = "/assets/uploads/" + fname;
        }
    }
    Json::Value ret;
    ret["status"] = savedUrl.empty() ? "error" : "ok";
    ret["url"] = savedUrl;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

void getFormSchema(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        auto collection = client["application"]["form_schema"];
        auto doc = collection.find_one({});
        Json::Value ret;
        if (doc) {
            std::string jsonStr = bsoncxx::to_json(*doc);
            Json::CharReaderBuilder builder;
            std::string errs;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            Json::Value jsonDoc;
            reader->parse(jsonStr.c_str(), jsonStr.c_str() + jsonStr.length(), &jsonDoc, &errs);
            ret["schema"] = jsonDoc;
        } else {
            ret["schema"] = Json::objectValue;
        }
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void setFormSchema(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) { callback(HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN)); return; }
    std::string jsonStr = json->toStyledString();
    on_db(req, std::move(callback), [jsonStr](mongocxx::client& client) {
        auto collection = client["application"]["form_schema"];
        collection.delete_many({});
        auto doc = bsoncxx::from_json(jsonStr);
        collection.insert_one(doc.view());
        Json::Value ret;
        ret["status"] = "ok";
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void getFormSubmissions(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    on_db(req, std::move(callback), [](mongocxx::client& client) {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;

        auto collection = client["application"]["form_submissions"];
        mongocxx::options::find opts;
        opts.sort(make_document(kvp("_id", -1)));
        auto cursor = collection.find({}, opts);
        Json::Value ret;
        ret["submissions"] = Json::arrayValue;
        for (auto&& doc : cursor) {
            std::string jsonStr = bsoncxx::to_json(doc);
            Json::CharReaderBuilder builder;
            std::string errs;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            Json::Value jsonDoc;
            if (reader->parse(jsonStr.c_str(), jsonStr.c_str() + jsonStr.length(), &jsonDoc, &errs)) {
                ret["submissions"].append(jsonDoc);
            }
        }
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

void submitForm(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) { callback(HttpResponse::newHttpResponse(k400BadRequest, CT_TEXT_PLAIN)); return; }
    
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
    
    Json::Value payload = *json;
    payload["submittedAt"] = ss.str();
    std::string jsonStr = payload.toStyledString();

    on_db(req, std::move(callback), [jsonStr](mongocxx::client& client) {
        auto collection = client["application"]["form_submissions"];
        auto doc = bsoncxx::from_json(jsonStr);
        collection.insert_one(doc.view());
        Json::Value ret;
        ret["status"] = "ok";
        return HttpResponse::newHttpJsonResponse(ret);
    });
}

} // namespace enactus

void registerApiHandlers() {
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/auth/login", drogon::Post, &enactus::login);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/auth/me", drogon::Get, &enactus::me);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/applications", drogon::Post, &enactus::apply);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/applications_list", drogon::Get, &enactus::listApplications);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/teams", drogon::Get, &enactus::listTeams);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/teams", drogon::Post, &enactus::createTeam);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/content", drogon::Get, &enactus::getContent);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/content", drogon::Post, &enactus::setContent);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/users", drogon::Get, &enactus::listUsers);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/users", drogon::Post, &enactus::createUser);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/users", drogon::Delete, &enactus::deleteUser);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/track_visit", drogon::Post, &enactus::trackVisit);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/analytics", drogon::Get, &enactus::getAnalytics);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/applications_update", drogon::Post, &enactus::updateApplicationStatus);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/upload", drogon::Post, &enactus::uploadFile);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/form_schema", drogon::Get, &enactus::getFormSchema);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/form_schema", drogon::Post, &enactus::setFormSchema);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/form_submissions", drogon::Get, &enactus::getFormSubmissions);
    anvil::accesscontrol::register_route(enactus::kRoutes, "/api/form_submissions", drogon::Post, &enactus::submitForm);
    drogon::app().registerHandler("/api/logs", &enactus::listLogs);
}


