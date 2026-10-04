#pragma once

// The Form Maker (anvil forms): staff build forms, the public fills them in,
// staff read and export the responses.

#include <string>

#include "app/http.h"

namespace enactus::routes {

void forms_list(const http::HttpRequestPtr& req, http::Responder&& respond);
void forms_create(const http::HttpRequestPtr& req, http::Responder&& respond);
void forms_update(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);
void forms_delete(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);
void forms_public(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);
void forms_submit(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);
void responses_list(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);
void responses_delete(const http::HttpRequestPtr& req, http::Responder&& respond,
                      const std::string& id, const std::string& response);
void responses_export(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);

}  // namespace enactus::routes
