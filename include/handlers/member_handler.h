#pragma once

#include <dpp/dpp.h>

namespace services {
class UserMappingService;
}

namespace handlers {

class SlashCommandHandler;  // Forward declaration for autorole state

/**
 * Handles Discord guild member events (add and remove).
 */
class MemberHandler {
public:
    MemberHandler(
        services::UserMappingService& user_mapping_service,
        SlashCommandHandler& slash_command_handler,
        dpp::cluster& bot
    );

    /**
     * Set guild and autorole IDs.
     * Called during bot initialization after reading config.
     */
    void set_guild_config(dpp::snowflake guild_id, dpp::snowflake autorole_id);

    /**
     * Handle guild member add event.
     * Assigns autorole to new members if enabled.
     */
    void handle_add(const dpp::guild_member_add_t& event);

    /**
     * Handle guild member remove event.
     * Removes user mapping when a member leaves.
     */
    void handle_remove(const dpp::guild_member_remove_t& event);

private:
    services::UserMappingService& user_mapping_service_;
    SlashCommandHandler& slash_command_handler_;
    dpp::cluster& bot_;
    dpp::snowflake guild_id_;
    dpp::snowflake autorole_id_;
};

} // namespace handlers
