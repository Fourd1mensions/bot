#include "commands/admin_set_command.h"
#include "services/service_container.h"
#include "services/user_resolver_service.h"
#include "services/user_mapping_service.h"
#include "services/command_params_service.h"
#include "requests.h"
#include <database.h>
#include <utils.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <dpp/dpp.h>
#include <nlohmann/json.hpp>

namespace commands {

using json = nlohmann::json;

std::vector<std::string> AdminSetCommand::get_aliases() const {
    return {"!adminset", "!aset"};
}

void AdminSetCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[adminset] ServiceContainer is null");
        return;
    }

    // Check if caller is admin
    std::string caller_id = ctx.author_id().str();
    bool is_admin = std::find(
        s->config.admin_users.begin(),
        s->config.admin_users.end(),
        caller_id
    ) != s->config.admin_users.end();

    if (!is_admin) {
        ctx.reply(":x: This command is admin-only.");
        return;
    }

    // Parse arguments: !adminset <discord_id_or_mention> <osu_username>
    std::string args = utils::extract_args(ctx.content);
    if (args.empty()) {
        ctx.reply(":x: Usage: `!adminset <discord_id_or_mention> <osu_username>`");
        return;
    }

    // Split args into discord_id and osu_username
    size_t space_pos = args.find(' ');
    if (space_pos == std::string::npos) {
        ctx.reply(":x: Usage: `!adminset <discord_id_or_mention> <osu_username>`");
        return;
    }

    std::string discord_arg = args.substr(0, space_pos);
    std::string osu_username = args.substr(space_pos + 1);

    // Trim whitespace from osu_username
    size_t start = osu_username.find_first_not_of(" \t");
    if (start == std::string::npos) {
        ctx.reply(":x: Usage: `!adminset <discord_id_or_mention> <osu_username>`");
        return;
    }
    osu_username = osu_username.substr(start);

    // Parse discord_id (support both raw ID and mention)
    dpp::snowflake target_discord_id;

    // Check if it's a mention
    auto mention_id = services::CommandParamsService::parse_discord_mention(discord_arg);
    if (mention_id) {
        try {
            target_discord_id = std::stoull(*mention_id);
        } catch (...) {
            ctx.reply(":x: Invalid Discord mention.");
            return;
        }
    } else {
        // Try parsing as raw ID
        try {
            target_discord_id = std::stoull(discord_arg);
        } catch (...) {
            ctx.reply(":x: Invalid Discord ID. Use a mention or numeric ID.");
            return;
        }
    }

    // Resolve osu! username to user ID
    std::string user_response = s->request.get_user(osu_username, false);
    if (user_response.empty()) {
        ctx.reply(fmt::format(":x: osu! user '{}' not found.", osu_username));
        return;
    }

    int64_t osu_user_id;
    std::string resolved_username;
    try {
        json user_json = json::parse(user_response);
        osu_user_id = user_json.value("id", static_cast<int64_t>(0));
        resolved_username = user_json.value("username", osu_username);

        if (osu_user_id == 0) {
            ctx.reply(fmt::format(":x: osu! user '{}' not found.", osu_username));
            return;
        }
    } catch (const json::exception& e) {
        spdlog::error("[adminset] Failed to parse user response: {}", e.what());
        ctx.reply(":x: Failed to resolve osu! user.");
        return;
    }

    // Save mapping to memory and database
    try {
        // Update in-memory cache
        s->user_mapping_service.set_mapping(target_discord_id, std::to_string(osu_user_id));

        // Save to database
        db::Database::instance().set_user_mapping(target_discord_id, osu_user_id);

        spdlog::info("[adminset] Admin {} linked Discord {} to osu! {} ({})",
            caller_id, target_discord_id.str(), osu_user_id, resolved_username);

        ctx.reply(fmt::format(":white_check_mark: Linked <@{}> to [{}](https://osu.ppy.sh/users/{}).",
            target_discord_id.str(), resolved_username, osu_user_id));
    } catch (const std::exception& e) {
        spdlog::error("[adminset] Database error: {}", e.what());
        ctx.reply(":x: Database error. Please try again.");
    }
}

} // namespace commands
