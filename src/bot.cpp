#include "fmt/format.h"
#include "osu.h"
#include <bot.h>
#include <requests.h>
#include <utils.h>
#include <database.h>
#include <cache.h>
#include <osu_tools.h>
#include <osu_parser.h>
#include <error_messages.h>

// Commands
#include <commands/lb_command.h>
#include <commands/rs_command.h>
#include <commands/bg_command.h>
#include <commands/audio_command.h>
#include <commands/map_command.h>
#include <commands/compare_command.h>
#include <commands/sim_command.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <thread>
#include <type_traits>

#include <fmt/base.h>
#include <spdlog/spdlog.h>

namespace stdr = std::ranges;

template <typename T>
T Random::get_real(T min, T max) {
  static_assert(std::is_floating_point<T>::value, "Type must be a floating-point type");
  std::uniform_real_distribution<> distr(min, max);
  return distr(_gen);
}

template <typename T>
T Random::get_int(T min, T max) {
  static_assert(std::is_integral<T>::value, "Type must be an integral type");
  std::uniform_int_distribution<> distr(min, max);
  return distr(_gen);
}

bool Random::get_bool() {
  std::bernoulli_distribution distr(0.5);
  return distr(_gen);
}

bool Bot::is_admin(const std::string& user_id) const {
  return std::find(config.admin_users.begin(), config.admin_users.end(), user_id) != config.admin_users.end();
}

void Bot::process_pending_button_removals() {
  spdlog::info("Processing pending button removals from database...");

  try {
    auto& db = db::Database::instance();

    // Get all expired removals and process them immediately
    auto expired = db.get_expired_button_removals();
    if (!expired.empty()) {
      spdlog::info("Found {} expired button removals, processing immediately", expired.size());
      for (const auto& [channel_id, message_id, removal_type] : expired) {
        leaderboard_service->remove_message_components(channel_id, message_id);
        db.remove_pending_button_removal(channel_id, message_id);
      }
    }

    // Get all future removals and schedule them
    auto pending = db.get_all_pending_removals();
    if (!pending.empty()) {
      auto now = std::chrono::system_clock::now();
      spdlog::info("Found {} pending button removals to schedule", pending.size());

      for (const auto& [channel_id, message_id, expires_at, removal_type] : pending) {
        // Skip if already expired (shouldn't happen, but safety check)
        if (expires_at <= now) {
          leaderboard_service->remove_message_components(channel_id, message_id);
          db.remove_pending_button_removal(channel_id, message_id);
          continue;
        }

        auto time_until = std::chrono::duration_cast<std::chrono::minutes>(expires_at - now);

        // Schedule removal thread
        std::jthread([this, channel_id, message_id, expires_at]() {
          auto now_local = std::chrono::system_clock::now();
          if (expires_at > now_local) {
            auto wait_duration = std::chrono::duration_cast<std::chrono::milliseconds>(expires_at - now_local);
            std::this_thread::sleep_for(wait_duration);
          }

          leaderboard_service->remove_message_components(channel_id, message_id);

          try {
            auto& db = db::Database::instance();
            db.remove_pending_button_removal(channel_id, message_id);
          } catch (const std::exception& e) {
            spdlog::warn("Failed to remove pending button removal from database: {}", e.what());
          }
        }).detach();

        spdlog::debug("Scheduled button removal for message {} in {}min", message_id.str(), time_until.count());
      }
    }

    spdlog::info("Finished processing pending button removals");
  } catch (const std::exception& e) {
    spdlog::error("Failed to process pending button removals: {}", e.what());
  }
}

void Bot::message_create_event(const dpp::message_create_t& event) {
  spdlog::info("[MSG] user={} ({}) channel={} content=\"{}\"",
    event.msg.author.id.str(), event.msg.author.username,
    event.msg.channel_id.str(), event.msg.content);

  {
    std::lock_guard<std::mutex> lock(mutex);
    chat_context_service.update_context(event.raw_event, event.msg.content, event.msg.channel_id.str(), event.msg.id.str());
  }

  // Route to command handlers
  command_router.route(event);
}

void Bot::message_update_event(const dpp::message_update_t& event) {
  dpp::snowflake channel_id = event.msg.channel_id;

  // Check if the updated message is the one tracked in chat context
  dpp::snowflake tracked_msg_id = chat_context_service.get_message_id(channel_id);
  if (tracked_msg_id == event.msg.id) {
    chat_context_service.update_context(event.raw_event, event.msg.content, channel_id, event.msg.id);
  }
}

void Bot::member_add_event(const dpp::guild_member_add_t& event) {
  if (!event.added.get_user()->is_bot() && give_autorole) 
    bot.guild_member_add_role(guild_id, event.added.get_user()->id, autorole_id);
}

void Bot::member_remove_event(const dpp::guild_member_remove_t& event) {
  user_mapping_service.remove_mapping(event.removed.id.str());
}

void Bot::slashcommand_event(const dpp::slashcommand_t& event) {
  // Log all slash commands with structured context
  const std::string& cmd_name = event.command.get_command_name();
  const std::string& user_id = event.command.usr.id.str();
  const std::string& username = event.command.usr.username;
  const std::string& channel_id = event.command.channel_id.str();

  spdlog::info("[CMD] user={} ({}) channel={} command=/{}", user_id, username, channel_id, cmd_name);

  // lol
  if (cmd_name == "гандон") {
    float_t    f     = rand.get_real(0.0f, 100.0f);
    auto embed = dpp::embed()
      .set_color(dpp::colors::cream)
      .set_title("Тест на гандона")
      .set_description(fmt::format("**Вы гандон на {:.2f}%**", f))
      .set_timestamp(time(0));
    dpp::message msg(event.command.channel_id, embed);
    event.reply(msg);
  }

  // avatar
  if (event.command.get_command_name() == "avatar") {
    std::string username = std::get<std::string>(event.get_parameter("username"));
    std::string userid = request.get_userid_v1(username);
    auto embed = dpp::embed()
      .set_color(dpp::colors::cream)
      .set_author(username, "https://osu.ppy.sh/users/" + userid, "")
      .set_image("https://a.ppy.sh/" + userid)
      .set_timestamp(time(0));
    dpp::message msg(event.command.channel_id, embed);
    event.reply(msg);
  }

  // update_token
  if (event.command.get_command_name() == "update_token") {
    auto invoker_id = event.command.usr.id.str();
    if (!is_admin(invoker_id)) {
      event.reply(dpp::message("<:FRICK:1241513672480653475>"));
      return;
    }
    if (request.update_token())
      event.reply(dpp::message("Token update - success"));
    else
      event.reply(dpp::message("Token update - fail"));
  }

  // set
  if (event.command.get_command_name() == "set") {
    auto start = std::chrono::steady_clock::now();

    std::string u_from_com = std::get<std::string>(event.get_parameter("username"));
    std::string req        = request.get_user(u_from_com);

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - start).count();

    if (req.empty()) {
      if (elapsed > 8) {
        event.edit_original_response(message_presenter.build_error_message(
          fmt::format(error_messages::API_TIMEOUT_FORMAT, elapsed)));
      } else {
        event.edit_original_response(dpp::message(fmt::format("Can't find {} on Bancho.", u_from_com)));
      }
      return;
    }

    try {
      json j = json::parse(req);
      std::string key = event.command.usr.id.str();
      std::string u_from_req = j.at("username").get<std::string>();
      int user_id = j.at("id").get<int>();

      user_mapping_service.set_mapping(key, fmt::to_string(user_id));

      // Save to database
      try {
        auto& db = db::Database::instance();
        db.set_user_mapping(dpp::snowflake(key), user_id);
      } catch (const std::exception& e) {
        spdlog::error("Failed to save user mapping to database: {}", e.what());
      }

      // Invalidate username caches for this user (they may have changed their username)
      // We cache the new username immediately
      try {
        auto& db = db::Database::instance();
        db.cache_username(user_id, u_from_req);
      } catch (const std::exception& e) {
        spdlog::warn("Failed to update username cache in PostgreSQL: {}", e.what());
      }

      try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_username(user_id, u_from_req);
      } catch (const std::exception& e) {
        spdlog::warn("Failed to update username cache in Memcached: {}", e.what());
      }

      if (elapsed > 8) {
        spdlog::warn("[CMD] /set took {}s to complete (slow API response)", elapsed);
      }

      event.edit_original_response(dpp::message(fmt::format("Your osu username: {}", u_from_req)));
    } catch (const json::exception& e) {
      spdlog::error("Failed to parse user data for {}: {}", u_from_com, e.what());
      event.edit_original_response(dpp::message("Failed to process user data. Please try again later."));
    }
  }

  // score
  if (event.command.get_command_name() == "score") {
    auto user_opt = user_mapping_service.get_osu_id(event.command.usr.id.str());
    if (!user_opt.has_value()) {
      event.edit_original_response(dpp::message("Please /set your osu username before using this command."));
      return;
    }
    const auto& user = user_opt.value();

    std::string stored_id = chat_context_service.get_beatmap_id(event.command.channel_id);
    std::string beatmap_id = beatmap_resolver_service.resolve_beatmap_id(stored_id);
    if (beatmap_id.empty()) {
      event.edit_original_response(message_presenter.build_error_message(error_messages::NO_BEATMAP_IN_CHANNEL));
      return;
    }

    auto start = std::chrono::steady_clock::now();

    std::string response_beatmap = request.get_beatmap(beatmap_id);
    std::string response_score   = request.get_user_beatmap_score(beatmap_id, user);

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - start).count();

    if (response_score.empty() || response_beatmap.empty()) {
      if (elapsed > 8) {
        event.edit_original_response(message_presenter.build_error_message(
          fmt::format(error_messages::API_TIMEOUT_FORMAT, elapsed)));
      } else {
        event.edit_original_response(dpp::message("Can't find score on this map."));
      }
      return;
    }

    Score      score(response_score);
    Beatmap    beatmap(response_beatmap);

    // Download .osz file asynchronously
    uint32_t beatmapset_id = beatmap.get_beatmapset_id();
    std::jthread([this, beatmapset_id]() {
      beatmap_downloader.download_osz(beatmapset_id);
    }).detach();

    auto embed = dpp::embed()
      .set_color(dpp::colors::cream)
      .set_title(beatmap.to_string())
      .set_url(beatmap.get_beatmap_url())
      .set_thumbnail(beatmap.get_image_url())
      .add_field("Your best score on map", score.to_string(beatmap.get_max_combo()));

    if (elapsed > 8) {
      spdlog::warn("[CMD] /score took {}s to complete (slow API response)", elapsed);
    }

    event.edit_original_response(dpp::message(event.command.channel_id, embed));
  }

  // autorole_switch
  if (event.command.get_command_name() == "autorole_switch") {
    auto invoker_id = event.command.usr.id.str();
    if (!is_admin(invoker_id)) {
      event.reply(dpp::message("<:FRICK:1241513672480653475>"));
      return;
    }
    if (give_autorole) {
      give_autorole = false;
      event.reply("Giving autorole switched to off");
    }
    else {
      give_autorole = true;
      event.reply("Giving autorole switched to on");
    }
  }

  // weather
  if (event.command.get_command_name() == "weather") {
    const std::string city = std::get<std::string>(event.get_parameter("city"));
    std::string w = request.get_weather(city);

    // Check if response is empty before parsing (happens on 404 or other errors)
    if (w.empty()) {
      event.edit_original_response(dpp::message("City not found. Please check the spelling and try again."));
      return;
    }

    json j = json::parse(w);

    if (j.empty() || j.is_null()) {
      event.edit_original_response(dpp::message("Failed to get weather data. Please try again later."));
      return;
    }

    std::string c  = j.value("name", "Unknown");
    std::string desc  = j["weather"].at(0).value("description", "");
    double temp       = j["main"].value("temp", 0.0);
    double feels      = j["main"].value("feels_like", 0.0);
    int humidity      = j["main"].value("humidity", 0);
    double wind       = j["wind"].value("speed", 0.0);

    std::time_t timestamp = j.value("dt", 0);
    timestamp += j.value("timezone", 0);
    std::tm tm = *std::gmtime(&timestamp);
    std::ostringstream time;
    time << std::put_time(&tm, "%d.%m.%Y %H:%M");

    auto embed = dpp::embed()
        .set_color(dpp::colors::cream)
        .set_title(c + " - " + desc)
        //.set_description(desc)
        .add_field("Температура", fmt::format("{:.1f}°C, ощущается как {:.1f}°C", temp, feels ))
        .add_field("Влажность", fmt::format("{}%", humidity), true)
        .add_field("Ветер", fmt::format("{:.1f}м/с", wind), true)
        .set_footer(dpp::embed_footer().set_text(time.str()));

    event.edit_original_response(dpp::message(event.command.channel_id, embed));
  }
}

void Bot::ready_event(const dpp::ready_t& event, bool delete_commands) {
  if (dpp::run_once<struct register_bot_commands>()) {
    if (delete_commands) 
      bot.global_bulk_command_delete();

    bot.global_command_create(dpp::slashcommand("гандон", "Проверка", bot.me.id));
    bot.global_command_create(dpp::slashcommand("pages", "test", bot.me.id));
    bot.global_command_create(dpp::slashcommand("avatar", "Display osu! profile avatar", bot.me.id)
      .add_option(dpp::command_option(dpp::co_string, "username", "osu! profile username", true))
    );
    bot.global_command_create(dpp::slashcommand("update_token", "If peppy doesn't respond", bot.me.id));
    bot.global_command_create(dpp::slashcommand("set", "Set osu username", bot.me.id)
      .add_option(dpp::command_option(dpp::co_string, "username", "Your osu! profile username", true))
    );
    bot.global_command_create(dpp::slashcommand("autorole_switch", "Manage autorole issuance", bot.me.id));
    /*bot.global_command_create(
        dpp::slashcommand("score", "Displays your score", bot.me.id));*/
    bot.global_command_create(dpp::slashcommand("weather", "Shows current weather", bot.me.id)
      .add_option(dpp::command_option(dpp::co_string, "city", "Location", true))
    );
  }
  guild_id = utils::read_field("GUILD_ID", "config.json");
  autorole_id =  utils::read_field("AUTOROLE_ID", "config.json");

  // Load user mappings from database via service
  user_mapping_service.load_from_file("");  // Empty path - loads from database

  // Chat map loading is not needed - will be populated on-demand from database
  spdlog::info("Chat map will be populated on-demand from database");

  // Process pending button removals from database (for persistence across restarts)
  process_pending_button_removals();

  // Note: Leaderboard states are now stored in Memcached with automatic expiry
  // No periodic cleanup needed - Memcached handles TTL automatically

  // Set initial presence and start periodic updates
  if (beatmap_cache_service) {
    auto update_presence = [this]() {
      std::string status = beatmap_cache_service->get_status_string();
      bot.set_presence(dpp::presence(dpp::ps_online, dpp::at_watching, status));
    };

    // Initial presence
    update_presence();

    // Update every 60 seconds
    bot.start_timer([this, update_presence](const dpp::timer&) {
      update_presence();
    }, 60);
  }
}

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

  // Initialize button handler
  button_handler = std::make_unique<handlers::ButtonHandler>(
      *leaderboard_service, *recent_score_service, request);

  // Set up callbacks for proactive beatmap caching
  chat_context_service.set_beatmapset_callback([this](uint32_t beatmapset_id) {
    beatmap_cache_service->queue_download(beatmapset_id);
  });
  chat_context_service.set_beatmap_callback([this](uint32_t beatmap_id) {
    beatmap_cache_service->queue_download_by_beatmap_id(beatmap_id);
  });

  // Cleanup database entries for missing beatmap files
  beatmap_downloader.cleanup_missing_files();

  bot.on_log(dpp::utility::cout_logger());
  bot.on_button_click([this](const dpp::button_click_t& event) {
    button_handler->handle_button_click(event);
  });
  bot.on_form_submit([this](const dpp::form_submit_t& event) {
    button_handler->handle_form_submit(event);
  });
  bot.on_message_create([this](const dpp::message_create_t& event) {
    message_create_event(event);
  });
  bot.on_message_update([this](const dpp::message_update_t& event){
    message_update_event(event);
  });
  bot.on_guild_member_add([this](const dpp::guild_member_add_t& event) {
    member_add_event(event);
  });
  bot.on_guild_member_remove([this](const dpp::guild_member_remove_t& event) {
    member_remove_event(event);
  });
  bot.on_slashcommand([this](const dpp::slashcommand_t& event) {
    // Call thinking() immediately for commands that make API calls
    const std::string& cmd = event.command.get_command_name();
    if (cmd == "set" || cmd == "score" || cmd == "weather") {
      event.thinking();
    }
    std::jthread(&Bot::slashcommand_event, this, std::move(event)).detach();
  });
  bot.on_ready([this, delete_commands](const dpp::ready_t& event) {
    ready_event(event, delete_commands);
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
