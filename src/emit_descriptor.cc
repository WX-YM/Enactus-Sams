// Prints the API descriptor the admin panel's client is generated from
// (anvil docs/01-seams.md §14; `hammer codegen`). Every table is the one the
// server compiles against, so the generated client cannot drift from it.
//
//   build/dev/emit_descriptor > admin/descriptor.json

#include <cstdio>
#include <string>

#include "anvil/descriptor/descriptor.h"

#include "accounts.h"
#include "events.h"
#include "field_types.h"
#include "perms.h"
#include "rate_limits.h"
#include "route_descriptions.h"
#include "routes.h"
#include "sections.h"

namespace {

// The server's own limits: an image upload (anvil images::kMaxBytes), a JSON
// body, and the longest page any list route returns.
constexpr anvil::descriptor::Limits kLimits{.upload_max_bytes = 26214400,
                                            .body_max_bytes = 262144,
                                            .page_limit_max = 100};
static_assert(anvil::descriptor::page_ceiling_covers(enactus::kRouteDescriptions, kLimits),
              "a route pages past the descriptor's own ceiling");

}  // namespace

int main() {
    const anvil::descriptor::DescriptorInput input{
        .app_name = "enactus-sams",
        .app_version = "2.0.0",
        .permissions = enactus::kPermNameTable,
        .routes = enactus::kRoutes,
        .route_descriptions = enactus::kRouteDescriptions,
        .capability_scopes = {},
        .rate_limits = enactus::kRateLimits,
        .field_types = enactus::kFieldTypes,
        .sections = enactus::kSections,
        .topics = {},
        .events = enactus::kEvents,
        .accounts = &enactus::kAccounts,
        .chat_kinds = {},
        .limits = kLimits,
    };
    const std::string doc = anvil::descriptor::emit_descriptor(input);
    std::fwrite(doc.data(), 1, doc.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
