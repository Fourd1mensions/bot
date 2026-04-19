#pragma once

#include <osu.h>
#include <state/session_state.h>
#include <dpp/dpp.h>
#include <chrono>
#include <optional>
#include <tbb/tbb.h>

// Forward declarations
class Request;
class BeatmapDownloader;

namespace commands {
struct UnifiedContext;
}

namespace services {

class ChatContextService;
class BeatmapResolverService;
class UserMappingService;
class UserResolverService;
class MessagePresenterService;
class BeatmapPerformanceService;

/**
 * Service for building leaderboard pages and managing leaderboard creation.
 */
class LeaderboardService {
public:
  LeaderboardService(Request& request, BeatmapDownloader& beatmap_downloader,
                     ChatContextService&        chat_context_service,
                     BeatmapResolverService&    beatmap_resolver_service,
                     UserMappingService&        user_mapping_service,
                     UserResolverService&       user_resolver_service,
                     MessagePresenterService&   message_presenter,
                     BeatmapPerformanceService& performance_service, dpp::cluster& bot);

  /**
     * Build a message page for the leaderboard state.
     */
  dpp::message build_page(const LeaderboardState& state, const std::string& mods_filter = "");

  void create_leaderboard(const commands::UnifiedContext& ctx, const std::string& mods_filter = "",
                          const std::optional<std::string>& beatmap_id_override = std::nullopt,
                          LbSortMethod                      sort_method         = LbSortMethod::PP);

  /**
     * Schedule removal of message buttons after TTL.
     */
  void schedule_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id,
                               std::chrono::minutes ttl);

  /**
     * Remove components from a message.
     */
  void remove_message_components(dpp::snowflake channel_id, dpp::snowflake message_id);

private:
  Request&                   request_;
  BeatmapDownloader&         beatmap_downloader_;
  ChatContextService&        chat_context_service_;
  BeatmapResolverService&    beatmap_resolver_service_;
  UserMappingService&        user_mapping_service_;
  UserResolverService&       user_resolver_service_;
  MessagePresenterService&   message_presenter_;
  BeatmapPerformanceService& performance_service_;
  dpp::cluster&              bot_;
  tbb::task_arena            arena_;
};

} // namespace services
