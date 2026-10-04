#pragma once

// Staff accounts: who is signed in (session, me) and Access Control (the
// staff list, creating, editing and disabling accounts).
//
// The account itself is anvil's (identity::UserRepository, StaffService). What
// this application adds is a profile — a role label and a team — kept in
// `staff_profiles`, keyed by the account id.

#include <optional>
#include <string>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/core/types.h"
#include "app/http.h"

namespace enactus {

struct StaffProfile final {
    std::string role;
    std::string team;
};

// The roles Access Control offers. A label, not an authority: what an account
// may do is its permission set. `manager` and `vice manager` additionally scope
// application review to the account's team.
inline constexpr std::array<std::string_view, 6> kStaffRoles{{
    "HR", "director", "high board", "manager", "member", "vice manager",
}};

[[nodiscard]] bool is_staff_role(std::string_view role) noexcept;
[[nodiscard]] bool is_team_scoped_role(std::string_view role) noexcept;

class StaffProfiles final {
public:
    [[nodiscard]] static anvil::Result<std::optional<StaffProfile>> find(mongocxx::client& client,
                                                                         const anvil::Uuid& user);
    [[nodiscard]] static anvil::Status put(mongocxx::client& client, const anvil::Uuid& user,
                                           const StaffProfile& profile);
    // Every profile whose team is `from`, moved to `to` (a team rename).
    [[nodiscard]] static anvil::Status rename_team(mongocxx::client& client,
                                                   std::string_view from, std::string_view to);
};

// The signed-in account's team when its role scopes it to one, otherwise
// nullopt. Superadmins are never scoped.
[[nodiscard]] anvil::Result<std::optional<std::string>> team_scope(
    mongocxx::client& client, const anvil::UserContext& ctx);

namespace routes {
void session(const http::HttpRequestPtr& req, http::Responder&& respond);
void me(const http::HttpRequestPtr& req, http::Responder&& respond);
void staff_list(const http::HttpRequestPtr& req, http::Responder&& respond);
void staff_create(const http::HttpRequestPtr& req, http::Responder&& respond);
void staff_update(const http::HttpRequestPtr& req, http::Responder&& respond,
                  const std::string& id);
void staff_disable(const http::HttpRequestPtr& req, http::Responder&& respond,
                   const std::string& id);
}  // namespace routes

}  // namespace enactus
