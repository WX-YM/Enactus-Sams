#pragma once

// The public website's data: one read for everything the home page renders,
// and the page-view beacon.

#include "app/http.h"

namespace enactus::routes {

void site_get(const http::HttpRequestPtr& req, http::Responder&& respond);
void visits_record(const http::HttpRequestPtr& req, http::Responder&& respond);

}  // namespace enactus::routes
