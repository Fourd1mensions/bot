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
#include <services/beatmap_extract_service.h>
#include <services/webhook_service.h>
#include <services/message_crawler_service.h>

#include <dpp/dpp.h>
#include <state/session_state.h>
#include <commands/command_router.h>
#include <services/service_container.h>

// Forward declare handlers
namespace handlers {
class ButtonHandler;
class SlashCommandHandler;
class MessageHandler;
class MemberHandler;
class ReadyHandler;
}

class Random {
private:
  std::random_device _rd;
  std::mt19937       _gen;

public:
  template <typename T>
  T get_real(T min, T max) {
    static_assert(std::is_floating_point<T>::value, "Type must be a floating-point type");
    std::uniform_real_distribution<> distr(min, max);
    return distr(_gen);
  }

  template <typename T>
  T get_int(T min, T max) {
    static_assert(std::is_integral<T>::value, "Type must be an integral type");
    std::uniform_int_distribution<> distr(min, max);
    return distr(_gen);
  }

  bool get_bool() {
    std::bernoulli_distribution distr(0.5);
    return distr(_gen);
  }

  Random() : _rd(), _gen(_rd()) {}
};

using snowflake_string_map = std::unordered_map<dpp::snowflake, std::string>;

class Bot {
private:
  dpp::cluster          bot;

  Random                rand;
  Request               request;
  Config                config;
  std::unique_ptr<HttpServer> http_server;
  BeatmapDownloader     beatmap_downloader;

  tbb::task_arena       arena;

  // Services
  services::WebhookService            webhook_service;
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
  std::unique_ptr<services::BeatmapExtractService> beatmap_extract_service;
  std::unique_ptr<services::MessageCrawlerService> message_crawler_service;

  // Event handlers
  std::unique_ptr<handlers::ButtonHandler> button_handler;
  std::unique_ptr<handlers::SlashCommandHandler> slash_command_handler;
  std::unique_ptr<handlers::MessageHandler> message_handler;
  std::unique_ptr<handlers::MemberHandler> member_handler;
  std::unique_ptr<handlers::ReadyHandler> ready_handler;

  // Command routing
  commands::CommandRouter             command_router;
  std::unique_ptr<ServiceContainer>   service_container;

  // Register text commands with the router
  void                  register_commands();

public:
  Bot(const std::string& token, bool delete_commands);
  ~Bot();  // Destructor defined in bot.cpp for unique_ptr with forward declarations
  void start();
  void shutdown();

};
