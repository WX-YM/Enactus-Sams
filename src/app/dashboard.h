#pragma once

// The staff dashboard and the audit log.

#include "app/http.h"

namespace enactus::routes {

void dashboard_get(const http::HttpRequestPtr& req, http::Responder&& respond);
void audit_list(const http::HttpRequestPtr& req, http::Responder&& respond);

}  // namespace enactus::routes
