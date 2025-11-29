#include "services/user_resolver_service.h"
#include "services/command_params_service.h"
#include "requests.h"
#include "database.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace services {

using json = nlohmann::json;

UserResolverService::UserResolverService(Request& request)
    : request_(request) {}

UserResolveResult UserResolverService::resolve(
    const std::string& username,
    dpp::snowflake caller_discord_id
) const {
    UserResolveResult result;

    if (username.empty()) {
        // Use caller's linked account
        auto& db = db::Database::instance();
        auto osu_id_opt = db.get_osu_user_id(caller_discord_id);

        if (!osu_id_opt) {
            result.error_message = "you need to link your osu! account first with /set";
            return result;
        }

        result.osu_user_id = *osu_id_opt;
        return result;
    }

    // Check if username is a Discord mention
    auto mention_id = CommandParamsService::parse_discord_mention(username);
    if (mention_id) {
        try {
            dpp::snowflake mentioned_discord_id = std::stoull(*mention_id);

            auto& db = db::Database::instance();
            auto osu_id_opt = db.get_osu_user_id(mentioned_discord_id);

            if (!osu_id_opt) {
                result.error_message = fmt::format(
                    "user <@{}> hasn't linked their osu! account yet", *mention_id);
                return result;
            }

            result.osu_user_id = *osu_id_opt;
            spdlog::debug("[UserResolver] Resolved mention: Discord {} -> osu! {}",
                *mention_id, result.osu_user_id);
            return result;

        } catch (const std::exception& e) {
            result.error_message = "invalid user mention";
            spdlog::error("[UserResolver] Failed to parse Discord mention: {}", e.what());
            return result;
        }
    }

    // Look up user by username via osu! API
    std::string user_response = request_.get_user(username, false);
    if (user_response.empty()) {
        result.error_message = fmt::format("user '{}' not found", username);
        return result;
    }

    try {
        json user_json = json::parse(user_response);
        result.osu_user_id = user_json.value("id", static_cast<int64_t>(0));

        if (result.osu_user_id == 0) {
            result.error_message = fmt::format("user '{}' not found", username);
        }
    } catch (const json::exception& e) {
        result.error_message = "failed to parse user data";
        spdlog::error("[UserResolver] Failed to parse user response: {}", e.what());
    }

    return result;
}

} // namespace services
