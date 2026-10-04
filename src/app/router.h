#pragma once

namespace enactus {

// Registers every handler against the route table (src/config/routes.h). Throws
// at boot for a handler the table does not describe. Call after
// install_services() and before drogon::app().run().
void install_routes();

}  // namespace enactus
