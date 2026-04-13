#pragma once

#include <dpp/dpp.h>

// Forward declarations
class Request;
class BeatmapDownloader;
struct Config;

namespace services {
class ChatContextService;
class BeatmapResolverService;
class UserResolverService;
class UserMappingService;
class MessagePresenterService;
class CommandParamsService;
class BeatmapCacheService;
class BeatmapPerformanceService;
class RecentScoreService;
class LeaderboardService;
class BeatmapExtractService;
class UserSettingsService;
class EmbedTemplateService;
}

/**
 * Container for all services used by commands.
 * Provides dependency injection without circular dependencies.
 */
struct ServiceContainer {
  dpp::cluster& bot;
  Request& request;
  BeatmapDownloader& beatmap_downloader;
  const Config& config;

  services::ChatContextService& chat_context_service;
  services::BeatmapResolverService& beatmap_resolver_service;
  services::UserResolverService& user_resolver_service;
  services::UserMappingService& user_mapping_service;
  services::MessagePresenterService& message_presenter;
  services::CommandParamsService& command_params_service;
  services::BeatmapPerformanceService& performance_service;
  services::RecentScoreService& recent_score_service;
  services::LeaderboardService& leaderboard_service;
  services::BeatmapExtractService& beatmap_extract_service;
  services::UserSettingsService& user_settings_service;
  services::BeatmapCacheService* beatmap_cache_service;  // Optional, may be null
  services::EmbedTemplateService* template_service = nullptr;  // Optional
};
