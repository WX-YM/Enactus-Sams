#pragma once

// Teams and their rosters, as anvil entries (entries.h: kinds `team` and
// `team.member`).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mongocxx/client.hpp>

#include "anvil/core/result.h"
#include "anvil/entries/document.h"
#include "app/http.h"

namespace enactus {

struct TeamSummary final {
    anvil::Uuid  id;
    std::string  name;
    std::string  desc;
    std::int64_t version;
    std::int64_t members;
    bool         recruiting;
    bool         showcase;
};

// Every team, in display order.
[[nodiscard]] anvil::Result<std::vector<TeamSummary>> list_teams(mongocxx::client& client);

// The team named `name` (case-insensitive, trimmed), if any.
[[nodiscard]] anvil::Result<std::optional<TeamSummary>> find_team_by_name(mongocxx::client& client,
                                                                          std::string_view name);

// Adds `person` to the team's roster unless a member of that name is already
// on it. Used when an application is accepted.
[[nodiscard]] anvil::Status add_member_if_absent(mongocxx::client& client, const anvil::Uuid& team,
                                                 std::string_view person, const anvil::Uuid& actor);

[[nodiscard]] bool same_team_name(std::string_view a, std::string_view b) noexcept;

// A URL-safe slug for a team name: lowercase ASCII letters and digits joined by
// single hyphens (anvil entries slug rule), or empty when the name has none.
[[nodiscard]] std::string slugify(std::string_view name);

namespace routes {
void teams_list(const http::HttpRequestPtr& req, http::Responder&& respond);
void teams_create(const http::HttpRequestPtr& req, http::Responder&& respond);
void teams_update(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);
void teams_delete(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);
void teams_reorder(const http::HttpRequestPtr& req, http::Responder&& respond);
void members_list(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);
void members_add(const http::HttpRequestPtr& req, http::Responder&& respond, const std::string& id);
void members_update(const http::HttpRequestPtr& req, http::Responder&& respond,
                    const std::string& id, const std::string& member);
void members_remove(const http::HttpRequestPtr& req, http::Responder&& respond,
                    const std::string& id, const std::string& member);
}  // namespace routes

}  // namespace enactus
