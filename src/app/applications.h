#pragma once

// Recruitment applications, as anvil entries of kind `application`
// (entries.h). The public form creates them; staff with the Applications
// permission review them, a team manager only those for their own team.

#include <string>
#include <string_view>

#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "app/http.h"

namespace enactus {

// The published `open` switch of home.join. Closed when the section has never
// been published: a fresh database takes no applications until migrate has
// seeded it.
[[nodiscard]] anvil::Result<bool> recruitment_open(mongocxx::client& client);

// One application per email: the slug is the first 128 bits of SHA-256 over
// the canonical address, hex. The unique slug index enforces it.
[[nodiscard]] std::string application_slug(std::string_view canonical_email);

}  // namespace enactus

namespace enactus::routes {

void applications_apply(const http::HttpRequestPtr& req, http::Responder&& respond);
void applications_list(const http::HttpRequestPtr& req, http::Responder&& respond);
void applications_update(const http::HttpRequestPtr& req, http::Responder&& respond,
                         const std::string& id);
void applications_delete(const http::HttpRequestPtr& req, http::Responder&& respond,
                         const std::string& id);

}  // namespace enactus::routes
