#include "fmt/format.h"
#include "osu.h"
#include <bot.h>
#include <requests.h>
#include <utils.h>
#include <database.h>
#include <cache.h>

// Commands
#include <commands/lb_command.h>
#include <commands/rs_command.h>
#include <commands/bg_command.h>
#include <commands/audio_command.h>
#include <commands/map_command.h>
#include <commands/compare_command.h>
#include <commands/sim_command.h>

// Handlers
#include <handlers/button_handler.h>
#include <handlers/slash_command_handler.h>
#include <handlers/message_handler.h>
#include <handlers/member_handler.h>
#include <handlers/ready_handler.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <type_traits>

#include <fmt/base.h>
#include <spdlog/spdlog.h>

namespace stdr = std::ranges;

Bot::Bot(const std::string& token, bool delete_commands)
    : bot(token),
      arena(tbb::task_arena(16)),
      beatmap_resolver_service(request),
      performance_service(beatmap_downloader),
      user_resolver_service(request) {
  bot.intents = dpp::i_default_intents | dpp::i_message_content;

  // Configure spdlog with structured logging pattern
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  spdlog::set_level(spdlog::level::info);
  spdlog::info("Initializing bot...");

  // Load configuration
  if (!utils::load_config(config)) {
    spdlog::error("Failed to load config.json");
  }

  // Initialize HTTP server with config values
  http_server = std::make_unique<HttpServer>(config.http_host, config.http_port, config.beatmap_mirrors);

  // Initialize beatmap cache service for proactive caching
  beatmap_cache_service = std::make_unique<services::BeatmapCacheService>(beatmap_downloader, request, bot);
  beatmap_cache_service->set_error_channel(dpp::snowflake(1284189678035009670ULL));

  // Initialize recent score service
  recent_score_service = std::make_unique<services::RecentScoreService>(
      request, performance_service, message_presenter, bot);

  // Initialize leaderboard service
  leaderboard_service = std::make_unique<services::LeaderboardService>(
      request, beatmap_downloader, chat_context_service, beatmap_resolver_service,
      user_mapping_service, user_resolver_service, message_presenter, performance_service, bot);

  // Initialize handlers
  button_handler = std::make_unique<handlers::ButtonHandler>(
      *leaderboard_service, *recent_score_service, request);

  slash_command_handler = std::make_unique<handlers::SlashCommandHandler>(
      command_router, request, rand, config,
      chat_context_service, user_mapping_service, beatmap_resolver_service,
      message_presenter, beatmap_downloader, bot);

  message_handler = std::make_unique<handlers::MessageHandler>(
      command_router, chat_context_service);

  member_handler = std::make_unique<handlers::MemberHandler>(
      user_mapping_service, *slash_command_handler, bot);

  ready_handler = std::make_unique<handlers::ReadyHandler>(
      user_mapping_service, *leaderboard_service, beatmap_cache_service.get(),
      *slash_command_handler, *member_handler, bot);

  // Set up callbacks for proactive beatmap caching
  chat_context_service.set_beatmapset_callback([this](uint32_t beatmapset_id) {
    beatmap_cache_service->queue_download(beatmapset_id);
  });
  chat_context_service.set_beatmap_callback([this](uint32_t beatmap_id) {
    beatmap_cache_service->queue_download_by_beatmap_id(beatmap_id);
  });

  // Cleanup database entries for missing beatmap files
  beatmap_downloader.cleanup_missing_files();

  // Register event handlers
  bot.on_log(dpp::utility::cout_logger());

  bot.on_button_click([this](const dpp::button_click_t& event) {
    button_handler->handle_button_click(event);
  });

  bot.on_form_submit([this](const dpp::form_submit_t& event) {
    button_handler->handle_form_submit(event);
  });

  bot.on_message_create([this](const dpp::message_create_t& event) {
    message_handler->handle_create(event);
  });

  bot.on_message_update([this](const dpp::message_update_t& event) {
    message_handler->handle_update(event);
  });

  bot.on_guild_member_add([this](const dpp::guild_member_add_t& event) {
    member_handler->handle_add(event);
  });

  bot.on_guild_member_remove([this](const dpp::guild_member_remove_t& event) {
    member_handler->handle_remove(event);
  });

  bot.on_slashcommand([this](const dpp::slashcommand_t& event) {
    // Call thinking() immediately for commands that make API calls
    const std::string& cmd = event.command.get_command_name();
    if (cmd == "set" || cmd == "score" || cmd == "weather") {
      event.thinking();
    }
    std::jthread([this, event = std::move(event)]() mutable {
      slash_command_handler->handle(event);
    }).detach();
  });

  bot.on_ready([this, delete_commands](const dpp::ready_t& event) {
    ready_handler->handle(event, delete_commands);
  });

  // Create service container for command dependency injection
  service_container = std::make_unique<ServiceContainer>(ServiceContainer{
    .bot = bot,
    .request = request,
    .beatmap_downloader = beatmap_downloader,
    .config = config,
    .chat_context_service = chat_context_service,
    .beatmap_resolver_service = beatmap_resolver_service,
    .user_resolver_service = user_resolver_service,
    .message_presenter = message_presenter,
    .command_params_service = command_params_service,
    .performance_service = performance_service,
    .recent_score_service = *recent_score_service,
    .leaderboard_service = *leaderboard_service,
    .beatmap_cache_service = beatmap_cache_service.get()
  });

  // Register text commands
  command_router.set_services(service_container.get());
  register_commands();

  // Start HTTP health check server
  http_server->start();
}

void Bot::register_commands() {
  command_router.register_command(std::make_unique<commands::LbCommand>());
  command_router.register_command(std::make_unique<commands::RsCommand>());
  command_router.register_command(std::make_unique<commands::BgCommand>());
  command_router.register_command(std::make_unique<commands::AudioCommand>());
  command_router.register_command(std::make_unique<commands::MapCommand>());
  command_router.register_command(std::make_unique<commands::CompareCommand>());
  command_router.register_command(std::make_unique<commands::SimCommand>());
}

void Bot::start() {
  // Start beatmap cache service
  if (beatmap_cache_service) {
    beatmap_cache_service->start();
  }

  // Non-blocking start so main thread can manage shutdown
  bot.start(dpp::st_return);
}

void Bot::shutdown() {
  spdlog::info("Shutting down bot...");

  // Stop beatmap cache service first
  if (beatmap_cache_service) {
    beatmap_cache_service->stop();
  }

  // Stop HTTP server
  if (http_server) {
    http_server->stop();
  }

  // Stop Discord bot
  bot.shutdown();

  spdlog::info("Shutdown complete");
}

// Destructor must be defined in cpp where handler types are complete
Bot::~Bot() = default;
