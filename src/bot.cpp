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
#include <commands/users_command.h>
#include <commands/admin_set_command.h>
#include <commands/admin_unset_command.h>
#include <commands/settings_command.h>
#include <commands/osc_command.h>
#include <commands/profile_command.h>
#include <commands/top_command.h>
#include <commands/link_command.h>

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
  bot.intents = dpp::i_default_intents | dpp::i_message_content | dpp::i_guild_members;

  // Configure spdlog with structured logging pattern
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  spdlog::set_level(spdlog::level::info);
  spdlog::info("Initializing bot...");

  // Load configuration
  if (!utils::load_config(config)) {
    spdlog::error("Failed to load config.json");
  }

  // Configure webhook service
  webhook_service.set_webhook(services::WebhookChannel::MirrorErrors, config.webhooks.mirror_errors);
  webhook_service.set_webhook(services::WebhookChannel::General, config.webhooks.general);
  webhook_service.set_webhook(services::WebhookChannel::Debug, config.webhooks.debug);

  // Configure beatmap downloader from config
  beatmap_downloader.set_mirrors(config.beatmap_mirrors);
  beatmap_downloader.set_webhook_service(&webhook_service);

  // Initialize HTTP server with config values
  http_server = std::make_unique<HttpServer>(config.http_host, config.http_port, config.beatmap_mirrors);

  // Initialize beatmap cache service for proactive caching
  beatmap_cache_service = std::make_unique<services::BeatmapCacheService>(beatmap_downloader, request, bot);
  beatmap_cache_service->set_error_channel(dpp::snowflake(1284189678035009670ULL));

  // Link performance service to cache service for background .osz downloads
  performance_service.set_cache_service(beatmap_cache_service.get());

  // Initialize recent score service
  recent_score_service = std::make_unique<services::RecentScoreService>(
      request, performance_service, message_presenter, bot);

  // Initialize leaderboard service
  leaderboard_service = std::make_unique<services::LeaderboardService>(
      request, beatmap_downloader, chat_context_service, beatmap_resolver_service,
      user_mapping_service, user_resolver_service, message_presenter, performance_service, bot);

  // Initialize beatmap extract service
  beatmap_extract_service = std::make_unique<services::BeatmapExtractService>(
      beatmap_downloader, request, chat_context_service, beatmap_resolver_service, message_presenter);

  // Initialize message crawler service
  message_crawler_service = std::make_unique<services::MessageCrawlerService>(bot);

  // Initialize music player service
  music_player_service = std::make_unique<services::MusicPlayerService>(bot);

  // Initialize handlers
  button_handler = std::make_unique<handlers::ButtonHandler>(
      *leaderboard_service, *recent_score_service, message_presenter, request);

  slash_command_handler = std::make_unique<handlers::SlashCommandHandler>(
      command_router, request, rand, config,
      chat_context_service, user_mapping_service, beatmap_resolver_service,
      message_presenter, beatmap_downloader, bot);

  message_handler = std::make_unique<handlers::MessageHandler>(
      command_router, chat_context_service, bot);

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

  // Register event handlers - route DPP logs through spdlog
  bot.on_log([](const dpp::log_t& event) {
    switch (event.severity) {
      case dpp::ll_trace:
        spdlog::trace("[DPP] {}", event.message);
        break;
      case dpp::ll_debug:
        spdlog::debug("[DPP] {}", event.message);
        break;
      case dpp::ll_info:
        spdlog::info("[DPP] {}", event.message);
        break;
      case dpp::ll_warning:
        spdlog::warn("[DPP] {}", event.message);
        break;
      case dpp::ll_error:
        spdlog::error("[DPP] {}", event.message);
        break;
      case dpp::ll_critical:
        spdlog::critical("[DPP] {}", event.message);
        break;
      default:
        spdlog::info("[DPP] {}", event.message);
        break;
    }
  });

  bot.on_button_click([this](const dpp::button_click_t& event) {
    button_handler->handle_button_click(event);
  });

  bot.on_form_submit([this](const dpp::form_submit_t& event) {
    button_handler->handle_form_submit(event);
  });

  bot.on_message_create([this](const dpp::message_create_t& event) {
    message_handler->handle_create(event);
    // Store new message in crawler database
    if (message_crawler_service) {
      message_crawler_service->on_new_message(event.msg);
    }
  });

  bot.on_message_update([this](const dpp::message_update_t& event) {
    message_handler->handle_update(event);
    // Update message in crawler database if it was edited
    if (message_crawler_service) {
      message_crawler_service->on_message_update(event.msg);
    }
  });

  bot.on_guild_member_add([this](const dpp::guild_member_add_t& event) {
    member_handler->handle_add(event);
  });

  bot.on_guild_member_remove([this](const dpp::guild_member_remove_t& event) {
    member_handler->handle_remove(event);
  });

  bot.on_guild_member_update([](const dpp::guild_member_update_t& event) {
    auto uid = std::to_string(static_cast<uint64_t>(event.updated.user_id));
    cache::MemcachedCache::instance().del("custom_tmpl_perm:" + uid);
  });

  bot.on_slashcommand([this](const dpp::slashcommand_t& event) {
    // Call thinking() immediately for commands that make API calls
    const std::string& cmd = event.command.get_command_name();
    if (cmd == "score" || cmd == "weather" || cmd == "avatar" ||
        cmd == "rs" || cmd == "bg" || cmd == "audio" || cmd == "map" ||
        cmd == "compare" || cmd == "sim" || cmd == "lb" || cmd == "settings" || cmd == "osc" ||
        cmd == "osu" || cmd == "taiko" || cmd == "mania" || cmd == "ctb" || cmd == "catch") {
      event.thinking();
    }
    std::jthread([this, event = std::move(event)]() mutable {
      slash_command_handler->handle(event);
    }).detach();
  });

  bot.on_voice_ready([this](const dpp::voice_ready_t& event) {
    if (music_player_service) {
      music_player_service->on_voice_ready(event);
    }
  });

  bot.on_voice_state_update([this](const dpp::voice_state_update_t& event) {
    if (music_player_service) {
      music_player_service->on_voice_state_update(event);
    }
  });

  bot.on_ready([this, delete_commands](const dpp::ready_t& event) {
    ready_handler->handle(event, delete_commands);
    if (music_player_service) {
      music_player_service->restore_all_states();
    }
  });

  bot.on_guild_members_chunk([this](const dpp::guild_members_chunk_t& event) {
    ready_handler->handle_member_chunk(event);
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
    .user_mapping_service = user_mapping_service,
    .message_presenter = message_presenter,
    .command_params_service = command_params_service,
    .performance_service = performance_service,
    .recent_score_service = *recent_score_service,
    .leaderboard_service = *leaderboard_service,
    .beatmap_extract_service = *beatmap_extract_service,
    .user_settings_service = user_settings_service,
    .beatmap_cache_service = beatmap_cache_service.get(),
    .template_service = &embed_template_service
  });

  // Load user settings from DB
  user_settings_service.load_from_db();

  // Load embed templates from DB (seeds defaults if empty)
  embed_template_service.load_from_db();
  message_presenter.set_template_service(&embed_template_service);

  // Register text commands
  command_router.set_services(service_container.get());
  command_router.set_prefix(config.command_prefix);
  register_commands();

  // Connect services to HTTP server
  http_server->set_config(&config);
  http_server->set_crawler_service(message_crawler_service.get());
  http_server->set_user_settings_service(&user_settings_service);
  http_server->set_template_service(&embed_template_service);
  http_server->set_music_service(music_player_service.get());

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
  command_router.register_command(std::make_unique<commands::UsersCommand>());
  command_router.register_command(std::make_unique<commands::AdminSetCommand>());
  command_router.register_command(std::make_unique<commands::AdminUnsetCommand>());
  command_router.register_command(std::make_unique<commands::SettingsCommand>());
  command_router.register_command(std::make_unique<commands::OscCommand>());
  command_router.register_command(std::make_unique<commands::ProfileCommand>());
  command_router.register_command(std::make_unique<commands::TopCommand>());
  command_router.register_command(std::make_unique<commands::LinkCommand>());
}

void Bot::start() {
  // Start beatmap cache service
  if (beatmap_cache_service) {
    beatmap_cache_service->start();
  }

  // Start message crawler service
  if (message_crawler_service) {
    message_crawler_service->start();
  }

  // Non-blocking start so main thread can manage shutdown
  bot.start(dpp::st_return);
}

void Bot::shutdown() {
  spdlog::info("Shutting down bot...");

  // Request download abort first so ongoing downloads stop quickly
  beatmap_downloader.request_shutdown();

  // Stop music player service
  if (music_player_service) {
    music_player_service->shutdown();
  }

  // Stop message crawler service
  if (message_crawler_service) {
    message_crawler_service->stop();
  }

  // Stop beatmap cache service
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
