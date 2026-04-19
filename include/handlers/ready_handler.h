#pragma once

#include <dpp/dpp.h>
#include <string>
#include <set>

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
  ReadyHandler(services::UserMappingService&  user_mapping_service,
               services::LeaderboardService&  leaderboard_service,
               services::BeatmapCacheService* beatmap_cache_service,
               SlashCommandHandler& slash_command_handler, MemberHandler& member_handler,
               dpp::cluster& bot);

  /**
     * Handle ready event.
     * Initializes the bot: registers commands, loads data, schedules tasks.
     */
  void handle(const dpp::ready_t& event, bool delete_commands);

  /**
     * Handle guild members chunk event.
     * Called when receiving member data from gateway request.
     */
  void handle_member_chunk(const dpp::guild_members_chunk_t& event);

private:
  /**
     * Process pending button removals from database.
     * Restores scheduled removals after bot restart.
     */
  void process_pending_button_removals();

  /**
     * Sync all guild members to discord_users cache.
     * Called on startup and periodically.
     */
  void sync_guild_members();

  /**
     * Fetch a page of guild members (pagination helper).
     */
  void fetch_members_page(dpp::snowflake guild_id, dpp::snowflake after);

  /**
     * Validate that all tracked users are still on the server.
     * Removes mappings for users who have left.
     * @param synced_count Number of members synced - validation skipped if too low
     */
  void validate_tracked_users(size_t synced_count);

  /**
     * Check top-1 user by message count (24h) and assign/transfer the role.
     */
  void                           update_top_user_role();

  services::UserMappingService&  user_mapping_service_;
  services::LeaderboardService&  leaderboard_service_;
  services::BeatmapCacheService* beatmap_cache_service_;
  SlashCommandHandler&           slash_command_handler_;
  MemberHandler&                 member_handler_;
  dpp::cluster&                  bot_;

  // Sync state
  dpp::snowflake      target_guild_id_{0};
  std::atomic<size_t> sync_member_count_{0};
  std::atomic<size_t> expected_member_count_{0};
  std::atomic<bool>   sync_in_progress_{false};

  bool                top_role_initialized_{false};

  // Minimum members required to run validation (safety threshold)
  static constexpr size_t MIN_MEMBERS_FOR_VALIDATION = 10;
};

} // namespace handlers
