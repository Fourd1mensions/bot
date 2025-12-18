#pragma once

#include <tbb/tbb.h>
#include <random>
#include <unordered_map>
#include <memory>
#include <chrono>

#include <osu.h>
#include <requests.h>
#include <http_server.h>
#include <beatmap_downloader.h>
#include <services/chat_context_service.h>
#include <services/user_mapping_service.h>
#include <services/beatmap_resolver_service.h>
#include <services/message_presenter_service.h>
#include <services/beatmap_performance_service.h>
#include <services/command_params_service.h>
#include <services/user_resolver_service.h>
#include <services/beatmap_cache_service.h>
#include <services/recent_score_service.h>
#include <services/leaderboard_service.h>
#include <handlers/button_handler.h>

#include <dpp/dpp.h>
#include <state/session_state.h>
#include <commands/command_router.h>
#include <services/service_container.h>

class Random {
private:
  std::random_device _rd;
  std::mt19937       _gen;

public:
  template <typename T>
  T get_real(T min, T max);
  template <typename T>
  T    get_int(T min, T max);
  bool get_bool();

  Random() : _rd(), _gen(_rd()) {}
};

using snowflake_string_map = std::unordered_map<dpp::snowflake, std::string>;

class Bot {
private:
  bool                  give_autorole = true;

  dpp::cluster          bot;
  dpp::snowflake        guild_id,
                        autorole_id;

  Random                rand;
  Request               request;
  Config                config;
  std::unique_ptr<HttpServer> http_server;
  BeatmapDownloader     beatmap_downloader;

  std::mutex            mutex;
  tbb::task_arena       arena;

  // Services
  services::ChatContextService        chat_context_service;
  services::UserMappingService        user_mapping_service;
  services::BeatmapResolverService    beatmap_resolver_service;
  services::MessagePresenterService   message_presenter;
  services::BeatmapPerformanceService performance_service;
  services::CommandParamsService      command_params_service;
  services::UserResolverService       user_resolver_service;
  std::unique_ptr<services::BeatmapCacheService> beatmap_cache_service;
  std::unique_ptr<services::RecentScoreService> recent_score_service;
  std::unique_ptr<services::LeaderboardService> leaderboard_service;

  // Event handlers
  std::unique_ptr<handlers::ButtonHandler> button_handler;

  // Command routing
  commands::CommandRouter             command_router;
  std::unique_ptr<ServiceContainer>   service_container;

  void                  process_pending_button_removals();
  bool                  is_admin(const std::string& user_id) const;

  // Register text commands with the router
  void                  register_commands(); 

  // Handle events
  void message_create_event(const dpp::message_create_t& event);
  void message_update_event(const dpp::message_update_t& event);
  void member_add_event(const dpp::guild_member_add_t& event);
  void member_remove_event(const dpp::guild_member_remove_t& event);
  void slashcommand_event(const dpp::slashcommand_t& event);
  void ready_event(const dpp::ready_t& event, bool);

public:
  Bot(const std::string& token, bool delete_commands);
  void start();
  void shutdown();

};
