#pragma once
#include <array>
#include "anvil/audit/spec.h"

inline constexpr std::array<anvil::audit::ActionSpec, 2> kAuditActions{{
    {"user_created", anvil::audit::Severity::Info},
    {"application_reviewed", anvil::audit::Severity::Info},
}};
