// The reference application's descriptor emitter.
//
// This is the program docs/01-seams.md §14 describes, written as an application
// writes it: hand anvil the tables this application already declares, print the
// result, exit. It is thirty lines because everything it needs is already
// validated by a static_assert somewhere above it.
//
// It is built as part of the suite for the reason tests/testapp/ exists at all:
// a seam that cannot be satisfied from outside anvil fails HERE, which is the
// only place it can fail cheaply. An emitter that stopped compiling because a
// spec type grew a field is a build failure in this repository rather than a
// support request from an application.

#include <cstdio>
#include <string>

#include "anvil/descriptor/descriptor.h"

#include "capabilities.h"
#include "events.h"
#include "field_types.h"
#include "perms.h"
#include "rate_limits.h"
#include "route_descriptions.h"
#include "routes.h"
#include "sections.h"
#include "topics.h"

int main() {
    const anvil::descriptor::DescriptorInput input{
        .app_name = "testapp",
        .app_version = "0.0.0",
        .permissions = testapp::kPermNameTable,
        .routes = testapp::kRoutes,
        .route_descriptions = testapp::kRouteDescriptions,
        .capability_scopes = testapp::kScopes,
        .rate_limits = testapp::kRateLimits,
        .field_types = testapp::kFieldTypes,
        .sections = testapp::kSections,
        .topics = testapp::kTopics,
        .events = testapp::kEvents,
        // Deployment numbers. They are the application's because they are
        // decisions about a deployment, not facts about the library.
        .limits = anvil::descriptor::Limits{.upload_max_bytes = 26214400,
                                            .body_max_bytes = 262144,
                                            .page_limit_max = 100},
    };

    const std::string doc = anvil::descriptor::emit_descriptor(input);
    std::fwrite(doc.data(), 1, doc.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
