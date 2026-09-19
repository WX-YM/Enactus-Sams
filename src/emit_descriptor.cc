#include <cstdio>
#include <string>
#include "anvil/descriptor/descriptor.h"
#include "perms.h"
#include "routes.h"
#include "route_descriptions.h"

int main() {
    const anvil::descriptor::DescriptorInput input{
        .app_name = "enactus",
        .app_version = "1.0.0",
        .permissions = enactus::kPermNameTable,
        .routes = enactus::kRoutes,
        .route_descriptions = enactus::kRouteDescriptions,
        .limits = anvil::descriptor::Limits{.upload_max_bytes = 26214400,
                                            .body_max_bytes = 262144,
                                            .page_limit_max = 100},
    };
    const std::string doc = anvil::descriptor::emit_descriptor(input);
    std::fwrite(doc.data(), 1, doc.size(), stdout);
    return 0;
}
