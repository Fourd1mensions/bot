#pragma once

#include <dpp/dpp.h>
#include <string>

namespace services {
class UserMappingService;
class LeaderboardService;
class BeatmapCacheService;
}

namespace handlers {

class SlashCommandHandler;
class MemberHandler;

/**
 * Handles Discord ready event and bot initialization.
 */
class ReadyHandler {
public:
    ReadyHandler(
        services::UserMappingService& user_mapping_service,
        services::LeaderboardService& leaderboard_service,
        services::BeatmapCacheService* beatmap_cache_service,
        SlashCommandHandler& slash_command_handler,
        MemberHandler& member_handler,
        dpp::cluster& bot
    );

    /**
     * Handle ready event.
     * Initializes the bot: registers commands, loads data, schedules tasks.
     */
    void handle(const dpp::ready_t& event, bool delete_commands);

private:
    /**
     * Process pending button removals from database.
     * Restores scheduled removals after bot restart.
     */
    void process_pending_button_removals();

    services::UserMappingService& user_mapping_service_;
    services::LeaderboardService& leaderboard_service_;
    services::BeatmapCacheService* beatmap_cache_service_;
    SlashCommandHandler& slash_command_handler_;
    MemberHandler& member_handler_;
    dpp::cluster& bot_;
};

} // namespace handlers
