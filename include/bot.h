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

#include <dpp/dpp.h>
#include <state/session_state.h>

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

  // Note: Leaderboard states are now stored in Memcached with message_id as key (5-min TTL)
  dpp::message          build_lb_page(const LeaderboardState& state, const std::string& mods_filter = "");
  void                  remove_message_components(dpp::snowflake channel_id, dpp::snowflake message_id);
  void                  schedule_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id, std::chrono::minutes ttl);
  void                  process_pending_button_removals();
  std::string           get_username_cached(int64_t user_id);
  bool                  is_admin(const std::string& user_id) const;

  // Helper functions for mod-adjusted BPM and length
  float                 apply_speed_mods_to_bpm(float bpm, const std::string& mods) const;
  uint32_t              apply_speed_mods_to_length(uint32_t length_seconds, const std::string& mods) const;

  // TODO: delete all this shit
 void                  create_lb_message(const dpp::message_create_t& event, const std::string& mods_filter = "");
  void                  create_bg_message(const dpp::message_create_t& event);
  void                  create_audio_message(const dpp::message_create_t& event);
  void                  create_rs_message(const dpp::message_create_t& event, const std::string& mode = "osu", const std::string& params = "");
  void                  create_compare_message(const dpp::message_create_t& event, const std::string& params = "");
  void                  create_map_message(const dpp::message_create_t& event, const std::string& mods_filter = "");
  void                  create_sim_message(const dpp::message_create_t& event, double accuracy, const std::string& mode = "osu", const std::string& mods_filter = "", int combo = 0, int count_100 = -1, int count_50 = -1, int misses = 0, double ratio = -1.0);
  dpp::message          build_rs_page(RecentScoreState& state);
  // TODO: check guild members 

  // Handle events

  void button_click_event(const dpp::button_click_t& event);
  void form_submit_event(const dpp::form_submit_t& event);
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
