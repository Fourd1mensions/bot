#include "commands/admin_unset_command.h"
#include "services/service_container.h"
#include "services/user_resolver_service.h"
#include "services/user_mapping_service.h"
#include "services/command_params_service.h"
#include <database.h>
#include <utils.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <dpp/dpp.h>

namespace commands {

std::vector<std::string> AdminUnsetCommand::get_aliases() const {
    return {"!adminunset", "!aunset", "!adminremove", "!aremove"};
}

void AdminUnsetCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[adminunset] ServiceContainer is null");
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

    // Parse arguments: !adminunset <discord_id_or_mention>
    std::string args = utils::extract_args(ctx.content);
    if (args.empty()) {
        ctx.reply(":x: Usage: `!adminunset <discord_id_or_mention>`");
        return;
    }

    // Trim whitespace
    size_t start = args.find_first_not_of(" \t");
    size_t end = args.find_last_not_of(" \t");
    if (start == std::string::npos) {
        ctx.reply(":x: Usage: `!adminunset <discord_id_or_mention>`");
        return;
    }
    std::string discord_arg = args.substr(start, end - start + 1);

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

    // Check if user is tracked (check in-memory first, fallback to DB)
    auto osu_id_opt = s->user_mapping_service.get_osu_id(target_discord_id);
    if (!osu_id_opt) {
        // Fallback to database check
        auto db_opt = db::Database::instance().get_osu_user_id(target_discord_id);
        if (!db_opt) {
            ctx.reply(fmt::format(":x: <@{}> is not tracked.", target_discord_id.str()));
            return;
        }
        osu_id_opt = std::to_string(*db_opt);
    }

    int64_t osu_user_id;
    try {
        osu_user_id = std::stoll(*osu_id_opt);
    } catch (const std::exception& e) {
        spdlog::error("[adminunset] Failed to parse osu ID '{}': {}", *osu_id_opt, e.what());
        ctx.reply(":x: Internal error: invalid user ID format in database.");
        return;
    }

    // Get username before removing (for confirmation message)
    std::string osu_username = s->user_resolver_service.get_username_cached(osu_user_id);

    // Remove mapping from memory and database
    try {
        bool removed = s->user_mapping_service.remove_mapping(target_discord_id);
        if (removed) {
            spdlog::info("[adminunset] Admin {} removed Discord {} from tracking (was linked to osu! {} - {})",
                caller_id, target_discord_id.str(), osu_user_id, osu_username);

            ctx.reply(fmt::format(":white_check_mark: Removed <@{}> from tracking (was [{}](https://osu.ppy.sh/users/{})).",
                target_discord_id.str(), osu_username, osu_user_id));
        } else {
            ctx.reply(":x: Failed to remove user from tracking.");
        }
    } catch (const std::exception& e) {
        spdlog::error("[adminunset] Database error: {}", e.what());
        ctx.reply(":x: Database error. Please try again.");
    }
}

} // namespace commands
