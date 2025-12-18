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

// Apply speed mods (DT/NC/HT) to BPM
float Bot::apply_speed_mods_to_bpm(float bpm, const std::string& mods) const {
  bool has_dt = mods.find("DT") != std::string::npos || mods.find("NC") != std::string::npos;
  bool has_ht = mods.find("HT") != std::string::npos;

  if (has_dt) {
    return bpm * 1.5f;  // DT/NC increases speed by 50%
  } else if (has_ht) {
    return bpm * 0.75f; // HT decreases speed by 25%
  }
  return bpm;
}

// Apply speed mods (DT/NC/HT) to length
uint32_t Bot::apply_speed_mods_to_length(uint32_t length_seconds, const std::string& mods) const {
  bool has_dt = mods.find("DT") != std::string::npos || mods.find("NC") != std::string::npos;
  bool has_ht = mods.find("HT") != std::string::npos;

  if (has_dt) {
    return static_cast<uint32_t>(length_seconds / 1.5f);  // DT/NC shortens map by 1.5x
  } else if (has_ht) {
    return static_cast<uint32_t>(length_seconds / 0.75f); // HT lengthens map by 0.75x
  }
  return length_seconds;
}

dpp::message Bot::build_lb_page(const LeaderboardState& state, const std::string& mods_filter) {
  constexpr size_t SCORES_PER_PAGE = 5;

  size_t start = state.current_page * SCORES_PER_PAGE;
  size_t end = std::min(start + SCORES_PER_PAGE, state.scores.size());

  // Use mods_filter from state if available, otherwise use parameter
  const std::string& active_mods = state.mods_filter.empty() ? mods_filter : state.mods_filter;

  // Parse .osu file once for PP calculation (in case of Loved maps)
  float approach_rate = 9.0f;
  float overall_difficulty = 9.0f;
  bool has_beatmap_data = false;
  uint32_t beatmap_id = state.beatmap.get_beatmap_id();
  uint32_t beatmapset_id = state.beatmap.get_beatmapset_id();

  // Get .osu file path and difficulty using performance service
  auto osu_file_path_opt = performance_service.get_osu_file_path(beatmapset_id, beatmap_id);
  if (osu_file_path_opt) {
    auto diff_opt = performance_service.get_difficulty(beatmapset_id, beatmap_id, active_mods);
    if (diff_opt) {
      approach_rate = diff_opt->approach_rate;
      overall_difficulty = diff_opt->overall_difficulty;
      has_beatmap_data = true;
      spdlog::debug("[LB] Got beatmap data from performance service: AR={:.2f} OD={:.2f}", approach_rate, overall_difficulty);
    }
  }

  std::string title = state.beatmap.to_string();
  if (!active_mods.empty()) {
    title += fmt::format(" +{}", active_mods);
  }

  // Find caller's rank if they have a score
  std::string caller_rank_text;
  if (state.caller_discord_id != 0) {
    try {
      auto& db = db::Database::instance();
      auto osu_user_id_opt = db.get_osu_user_id(state.caller_discord_id);

      if (osu_user_id_opt) {
        int64_t osu_user_id = *osu_user_id_opt;

        // Find user's position in leaderboard
        for (size_t i = 0; i < state.scores.size(); ++i) {
          if (state.scores[i].get_user_id() == static_cast<size_t>(osu_user_id)) {
            caller_rank_text = fmt::format(" • your rank: #{}", i + 1);
            break;
          }
        }
      }
    } catch (const std::exception& e) {
      spdlog::warn("Failed to look up caller rank: {}", e.what());
    }
  }

  // Build footer text based on number of pages
  std::string sort_text = (state.sort_method != LbSortMethod::PP)
    ? fmt::format(" • sorted by {}", sort_method_to_string(state.sort_method))
    : "";

  std::string footer_text;
  if (state.total_pages == 1) {
    // Simplified footer for single page
    footer_text = fmt::format("{} {} shown{}{}{}",
      state.scores.size(),
      state.scores.size() == 1 ? "score" : "scores",
      active_mods.empty() ? "" : fmt::format(" • Filter: +{}", active_mods),
      sort_text,
      caller_rank_text);
  } else {
    // Full footer for multiple pages
    footer_text = fmt::format("Page {}/{} • {}/{} {} shown{}{}{}",
      state.current_page + 1,
      state.total_pages,
      end,
      state.scores.size(),
      end == 1 ? "score" : "scores",
      active_mods.empty() ? "" : fmt::format(" • Filter: +{}", active_mods),
      sort_text,
      caller_rank_text);
  }

  // Build score presentations for the current page
  std::vector<services::ScorePresentation> scores_on_page;
  for (size_t i = start; i < end; i++) {
    const auto& score = state.scores[i];

    // Calculate PP if API returns 0 (for Loved maps)
    double display_pp = score.get_pp();
    if (display_pp <= 0.01 && osu_file_path_opt.has_value() && score.get_mode() == "osu") {
      // Use osu-tools for accurate PP calculation
      auto perf_opt = osu_tools::simulate_performance(
        *osu_file_path_opt,
        score.get_accuracy(),
        "osu",
        score.get_mods(),
        score.get_max_combo(),
        score.get_count_miss(),
        score.get_count_100(),
        score.get_count_50()
      );

      if (perf_opt.has_value()) {
        display_pp = perf_opt->pp;
        spdlog::debug("[LB] Calculated PP using osu-tools for score by {}: {:.2f}pp (aim: {:.2f}, speed: {:.2f}, acc: {:.2f})",
          score.get_user_id(), display_pp, perf_opt->aim_pp, perf_opt->speed_pp, perf_opt->accuracy_pp);
      }
    }

    // Build header with calculated PP if needed
    std::string header;
    if (display_pp != score.get_pp()) {
      header = fmt::format("{} `{:.0f}pp` +{}", score.get_username(), display_pp, score.get_mods());
    } else {
      header = score.get_header();
    }

    scores_on_page.push_back({
      .rank = i + 1,
      .header = header,
      .body = score.get_body(state.beatmap.get_max_combo()),
      .display_pp = display_pp
    });
  }

  // Use presenter service to build the message
  return message_presenter.build_leaderboard_page(
    state.beatmap,
    scores_on_page,
    footer_text,
    active_mods,
    state.total_pages,
    state.current_page
  );
}

dpp::message Bot::build_rs_page(RecentScoreState& state) {
  if (state.scores.empty() || state.current_index >= state.scores.size()) {
    dpp::message err_msg;
    err_msg.set_content("no score to display");
    return err_msg;
  }

  const Score& score = state.scores[state.current_index];

  // Check cache first for fast navigation
  auto cache_it = state.page_content_cache.find(state.current_index);
  if (cache_it != state.page_content_cache.end()) {
    try {
      json cached_data = json::parse(cache_it->second);

      // Rebuild message from cached data using presenter
      services::MessagePresenterService::RecentScoreCacheData cache_data{
        .title = cached_data["title"].get<std::string>(),
        .url = cached_data["url"].get<std::string>(),
        .description = cached_data["description"].get<std::string>(),
        .thumbnail = cached_data["thumbnail"].get<std::string>(),
        .beatmap_info = cached_data["beatmap_info"].get<std::string>(),
        .footer = cached_data["footer"].get<std::string>(),
        .timestamp = cached_data["timestamp"].get<time_t>()
      };

      services::PaginationInfo pagination{
        .current = state.current_index,
        .total = state.scores.size(),
        .has_refresh = true,
        .refresh_count = state.refresh_count
      };

      spdlog::debug("[RS] Using cached page data for index {}", state.current_index);
      return message_presenter.build_from_cache_data(cache_data, pagination);
    } catch (const std::exception& e) {
      spdlog::warn("[RS] Failed to use cached page data: {}, rebuilding", e.what());
      // Fall through to rebuild
    }
  }

  // Get beatmap info from the score's beatmap data
  std::string beatmap_response = request.get_beatmap(std::to_string(score.get_beatmap_id()));
  if (beatmap_response.empty()) {
    dpp::message err_msg;
    err_msg.set_content("failed to fetch beatmap data");
    return err_msg;
  }

  Beatmap beatmap(beatmap_response);

  // Get mod-adjusted difficulty if mods are present
  if (!score.get_mods().empty() && score.get_mods() != "NM") {
    uint32_t mods_bitset = utils::mods_string_to_bitset(score.get_mods());
    std::string attributes_response = request.get_beatmap_attributes(
      std::to_string(score.get_beatmap_id()), mods_bitset);

    if (!attributes_response.empty()) {
      try {
        json attributes_json = json::parse(attributes_response);
        beatmap.set_modded_attributes(attributes_json);
      } catch (...) {}
    }
  }

  // Get AR/OD/CS/HP/total_objects from cache or performance service
  float approach_rate = 9.0f;
  float overall_difficulty = 9.0f;
  float circle_size = 5.0f;
  float hp_drain_rate = 5.0f;
  int total_objects = 0;
  uint32_t beatmap_id = score.get_beatmap_id();
  uint32_t beatmapset_id = beatmap.get_beatmapset_id();
  std::optional<std::string> osu_file_path_opt;

  // Check cache first
  auto difficulty_cache_it = state.beatmap_difficulty_cache.find(beatmap_id);
  if (difficulty_cache_it != state.beatmap_difficulty_cache.end()) {
    approach_rate = std::get<0>(difficulty_cache_it->second);
    overall_difficulty = std::get<1>(difficulty_cache_it->second);
    circle_size = std::get<2>(difficulty_cache_it->second);
    hp_drain_rate = std::get<3>(difficulty_cache_it->second);
    total_objects = std::get<4>(difficulty_cache_it->second);
    spdlog::debug("[PP] Using cached difficulty for beatmap {}", beatmap_id);

    // Get .osu file path for PP calculation
    osu_file_path_opt = performance_service.get_osu_file_path(beatmapset_id, beatmap_id);
  } else {
    // Use performance service to get difficulty
    osu_file_path_opt = performance_service.get_osu_file_path(beatmapset_id, beatmap_id);
    if (osu_file_path_opt) {
      auto diff_opt = performance_service.get_difficulty(beatmapset_id, beatmap_id, score.get_mods());
      if (diff_opt) {
        approach_rate = diff_opt->approach_rate;
        overall_difficulty = diff_opt->overall_difficulty;
        circle_size = diff_opt->circle_size;
        hp_drain_rate = diff_opt->hp_drain_rate;
        total_objects = diff_opt->total_objects;

        // Add to cache
        state.beatmap_difficulty_cache[beatmap_id] = std::make_tuple(
          approach_rate, overall_difficulty, circle_size, hp_drain_rate, total_objects);

        spdlog::debug("[PP] Got and cached difficulty from performance service for beatmap {}", beatmap_id);
      }
    }
  }

  // Calculate map completion percentage
  int hits_made = score.get_count_300() + score.get_count_100() + score.get_count_50() + score.get_count_miss();
  float completion_percent = (total_objects > 0) ? (static_cast<float>(hits_made) / total_objects) * 100.0f : 100.0f;

  // Use calculator PP if API returns 0 (e.g., for Loved maps)
  double current_pp = score.get_pp();

  // Simple structure for FC performance
  struct {
    double total_pp = 0.0;
    double aim_pp = 0.0;
    double speed_pp = 0.0;
    double accuracy_pp = 0.0;
  } fc_perf;

  // Use full beatmap parsing if .osu file is available (osu!standard only)
  if (osu_file_path_opt.has_value() && score.get_mode() == "osu") {
    if (current_pp <= 0.01) {
      // Use osu-tools for accurate PP calculation
      auto calculated_perf_opt = osu_tools::simulate_performance(
        *osu_file_path_opt,
        score.get_accuracy(),
        "osu",
        score.get_mods(),
        score.get_max_combo(),
        score.get_count_miss(),
        score.get_count_100(),
        score.get_count_50()
      );

      if (calculated_perf_opt.has_value()) {
        current_pp = calculated_perf_opt->pp;
        spdlog::debug("[PP] Calculated PP using osu-tools: {:.2f}pp (aim: {:.2f}, speed: {:.2f}, acc: {:.2f})",
          current_pp, calculated_perf_opt->aim_pp, calculated_perf_opt->speed_pp, calculated_perf_opt->accuracy_pp);
      }
    }

    // Calculate FC PP using osu-tools (converting misses to 300s for accuracy)
    if (score.get_count_miss() > 0) {
      int total_objects = score.get_count_300() + score.get_count_100() + score.get_count_50() + score.get_count_miss();
      double fc_accuracy = ((score.get_count_300() + score.get_count_miss()) * 300.0 + score.get_count_100() * 100.0 + score.get_count_50() * 50.0) / (total_objects * 300.0);

      spdlog::info("[PP] FC calculation inputs: count_300={}, count_100={}, count_50={}, count_miss=0 (was {}), acc={:.4f}",
        score.get_count_300() + score.get_count_miss(), score.get_count_100(), score.get_count_50(), score.get_count_miss(), fc_accuracy);

      // Use osu-tools to calculate FC PP
      auto fc_perf_opt = osu_tools::simulate_performance(
        *osu_file_path_opt,
        fc_accuracy,
        "osu",
        score.get_mods(),
        0,  // combo = 0 means use beatmap max
        0,  // misses = 0 for FC
        score.get_count_100(),
        score.get_count_50()
      );

      if (fc_perf_opt.has_value()) {
        fc_perf.total_pp = fc_perf_opt->pp;
        fc_perf.aim_pp = fc_perf_opt->aim_pp;
        fc_perf.speed_pp = fc_perf_opt->speed_pp;
        fc_perf.accuracy_pp = fc_perf_opt->accuracy_pp;

        spdlog::info("[PP] FC PP calculation successful: {:.2f}pp (current: {:.2f}pp, {} misses -> 0)",
          fc_perf.total_pp, current_pp, score.get_count_miss());
      } else {
        spdlog::warn("[PP] FC PP calculation failed - osu-tools returned no result");
      }
    }
  }

  // Prepare PP info for presenter
  services::PPInfo pp_info{
    .current_pp = current_pp,
    .fc_pp = fc_perf.total_pp,
    .fc_accuracy = 0.0,
    .has_fc_pp = false
  };

  // Calculate FC accuracy if applicable
  if (score.get_mode() == "osu" && score.get_count_miss() > 0 && fc_perf.total_pp > current_pp) {
    int fc_total_hits = score.get_count_300() + score.get_count_100() + score.get_count_50() + score.get_count_miss();
    pp_info.fc_accuracy = ((score.get_count_300() + score.get_count_miss()) * 300.0 + score.get_count_100() * 100.0 + score.get_count_50() * 50.0) / (fc_total_hits * 300.0) * 100.0;
    pp_info.has_fc_pp = true;
  }

  // Prepare difficulty info for presenter
  services::DifficultyInfo difficulty_info{
    .approach_rate = approach_rate,
    .overall_difficulty = overall_difficulty,
    .circle_size = circle_size,
    .hp_drain_rate = hp_drain_rate
  };

  // Prepare pagination info
  services::PaginationInfo pagination{
    .current = state.current_index,
    .total = state.scores.size(),
    .has_refresh = true,
    .refresh_count = state.refresh_count
  };

  // Calculate modded BPM and length
  float modded_bpm = apply_speed_mods_to_bpm(beatmap.get_bpm(), score.get_mods());
  uint32_t modded_length = apply_speed_mods_to_length(beatmap.get_total_length(), score.get_mods());

  // Build message using presenter service
  std::string score_type = state.use_best_scores ? "best" : "recent";
  dpp::message msg = message_presenter.build_recent_score_page(
    score,
    beatmap,
    difficulty_info,
    pp_info,
    pagination,
    score_type,
    completion_percent,
    modded_bpm,
    modded_length
  );

  // Cache the page content for fast navigation using presenter's cache data builder
  try {
    auto cache_data = message_presenter.build_recent_score_cache_data(
      score, beatmap, difficulty_info, pp_info, pagination,
      score_type, completion_percent, modded_bpm, modded_length
    );

    json page_data;
    page_data["title"] = cache_data.title;
    page_data["url"] = cache_data.url;
    page_data["description"] = cache_data.description;
    page_data["thumbnail"] = cache_data.thumbnail;
    page_data["beatmap_info"] = cache_data.beatmap_info;
    page_data["footer"] = cache_data.footer;
    page_data["timestamp"] = cache_data.timestamp;

    state.page_content_cache[state.current_index] = page_data.dump();
    spdlog::debug("[RS] Cached page data for index {}", state.current_index);
  } catch (const std::exception& e) {
    spdlog::warn("[RS] Failed to cache page data: {}", e.what());
  }

  return msg;
}

void Bot::remove_message_components(dpp::snowflake channel_id, dpp::snowflake message_id) {
  // Get the message first, then remove components
  bot.message_get(message_id, channel_id, [this, message_id](const dpp::confirmation_callback_t& callback) {
    if (callback.is_error()) {
      spdlog::warn("Failed to get message {} for button removal", message_id.str());
      return;
    }

    auto msg = callback.get<dpp::message>();
    msg.components.clear();

    // Edit message to remove components only
    bot.message_edit(msg, [this, message_id](const dpp::confirmation_callback_t& edit_callback) {
      if (!edit_callback.is_error()) {
        // Delete from Memcached (try both leaderboard and recent_scores)
        try {
          auto& cache = cache::MemcachedCache::instance();
          cache.delete_leaderboard(message_id.str());
          cache.delete_recent_scores(message_id.str());
          spdlog::info("Buttons removed for message {}", message_id.str());
        } catch (const std::exception& e) {
          spdlog::debug("Cache cleanup for message {}: {}", message_id.str(), e.what());
        }
      } else {
        spdlog::warn("Failed to edit message {} to remove buttons: {}", message_id.str(), edit_callback.get_error().message);
      }
    });
  });
}

void Bot::schedule_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id, std::chrono::minutes ttl) {
  auto expires_at = std::chrono::system_clock::now() + ttl;

  // Store in database for persistence across restarts
  try {
    auto& db = db::Database::instance();
    db.register_pending_button_removal(channel_id, message_id, expires_at);
  } catch (const std::exception& e) {
    spdlog::error("Failed to register pending button removal in database: {}", e.what());
  }

  // Schedule removal thread
  std::jthread([this, channel_id, message_id, ttl]() {
    std::this_thread::sleep_for(ttl);

    // Remove components (works for any message type)
    remove_message_components(channel_id, message_id);

    // Remove from database after invalidation
    try {
      auto& db = db::Database::instance();
      db.remove_pending_button_removal(channel_id, message_id);
    } catch (const std::exception& e) {
      spdlog::warn("Failed to remove pending button removal from database: {}", e.what());
    }
  }).detach();

  spdlog::info("Scheduled button removal for message {} (TTL: {}min)", message_id.str(), ttl.count());
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
        remove_message_components(channel_id, message_id);
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
          remove_message_components(channel_id, message_id);
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

          remove_message_components(channel_id, message_id);

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

std::string Bot::get_username_cached(int64_t user_id) {
  // Try Memcached first (hot cache)
  try {
    auto& cache = cache::MemcachedCache::instance();
    if (auto cached = cache.get_username(user_id)) {
      spdlog::info("[CACHE] Username HIT (Memcached) for user {} -> {}", user_id, *cached);
      return *cached;
    }
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] Memcached get_username failed for user {}: {}", user_id, e.what());
  }

  // Try PostgreSQL cache (warm cache)
  try {
    auto& db = db::Database::instance();
    if (auto cached = db.get_cached_username(user_id)) {
      spdlog::info("[CACHE] Username HIT (PostgreSQL) for user {} -> {}", user_id, *cached);

      // Update Memcached with this username
      try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_username(user_id, *cached);
        spdlog::debug("[CACHE] Promoted username to Memcached");
      } catch (const std::exception& e) {
        spdlog::debug("[CACHE] Failed to promote to Memcached: {}", e.what());
      }

      return *cached;
    }
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] PostgreSQL get_cached_username failed for user {}: {}", user_id, e.what());
  }

  // Cache miss - fetch from API
  spdlog::info("[CACHE] Username MISS for user {}, fetching from API", user_id);
  std::string usr_j = request.get_user(fmt::format("{}", user_id), true);
  json usr = json::parse(usr_j);
  std::string username = usr.at("username");
  spdlog::info("[CACHE] Fetched username from API: {} -> {}", user_id, username);

  // Cache in both layers
  try {
    auto& db = db::Database::instance();
    db.cache_username(user_id, username);
    spdlog::debug("[CACHE] Cached username in PostgreSQL");
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] Failed to cache username in PostgreSQL: {}", e.what());
  }

  try {
    auto& cache = cache::MemcachedCache::instance();
    cache.cache_username(user_id, username);
    spdlog::debug("[CACHE] Cached username in Memcached");
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] Failed to cache username in Memcached: {}", e.what());
  }

  return username;
}

void Bot::form_submit_event(const dpp::form_submit_t& event) {
  spdlog::info("[FORM] user={} ({}) channel={} form_id={}",
    event.command.usr.id.str(), event.command.usr.username,
    event.command.channel_id.str(), event.custom_id);

  if (event.custom_id == "lb_jump_modal") {
    auto msg_id = event.command.message_id;

    // Get the page number from the form
    std::string page_str = std::get<std::string>(event.components[0].components[0].value);

    try {
      int page_num = std::stoi(page_str);

      // Fetch from Memcached
      std::optional<LeaderboardState> state_opt;
      try {
        auto& cache = cache::MemcachedCache::instance();
        state_opt = cache.get_leaderboard(msg_id.str());
      } catch (const std::exception& e) {
        spdlog::warn("Failed to fetch leaderboard from cache: {}", e.what());
      }

      if (!state_opt) {
        // Leaderboard expired - buttons should already be removed by timer thread
        // Silently ignore this interaction (likely a race condition)
        spdlog::debug("Ignoring form submit for expired leaderboard {}", msg_id.str());
        return;
      }

      auto state = *state_opt;

      // Validate page number
      if (page_num < 1 || page_num > static_cast<int>(state.total_pages)) {
        event.reply(dpp::ir_channel_message_with_source,
          dpp::message(fmt::format("Invalid page number. Please enter a number between 1 and {}.", state.total_pages))
            .set_flags(dpp::m_ephemeral));
        return;
      }

      // Update to the requested page (convert to 0-indexed)
      state.current_page = page_num - 1;

      // Save updated state back to Memcached
      try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_leaderboard(msg_id.str(), state);
      } catch (const std::exception& e) {
        spdlog::warn("Failed to save updated leaderboard to cache: {}", e.what());
      }

      // Build updated message with new page
      dpp::message updated_msg = build_lb_page(state);

      // Update the message
      event.reply(dpp::ir_update_message, updated_msg);

    } catch (const std::exception& e) {
      event.reply(dpp::ir_channel_message_with_source,
        dpp::message("Invalid input. Please enter a valid number.").set_flags(dpp::m_ephemeral));
    }
  }
}

void Bot::button_click_event(const dpp::button_click_t& event) {
  const std::string& button_id = event.custom_id;

  spdlog::info("[BTN] user={} ({}) channel={} button={}",
    event.command.usr.id.str(), event.command.usr.username,
    event.command.channel_id.str(), button_id);

  // Handle page jump modal (center button)
  if (button_id == "lb_select") {
    auto msg_id = event.command.message_id;

    // Fetch from Memcached
    std::optional<LeaderboardState> state_opt;
    try {
      auto& cache = cache::MemcachedCache::instance();
      state_opt = cache.get_leaderboard(msg_id.str());
    } catch (const std::exception& e) {
      spdlog::warn("Failed to fetch leaderboard from cache: {}", e.what());
    }

    if (!state_opt) {
      // Leaderboard expired - buttons should already be removed by timer thread
      // Silently ignore this interaction (likely a race condition)
      spdlog::debug("Ignoring button click for expired leaderboard {}", msg_id.str());
      return;
    }

    size_t total_pages = state_opt->total_pages;

    // Create modal for page jump
    dpp::interaction_modal_response modal("lb_jump_modal", "Jump to Page");

    dpp::component text_input;
    text_input.set_label("Page Number")
      .set_id("page_number")
      .set_text_style(dpp::text_short)
      .set_placeholder(fmt::format("Enter 1-{}", total_pages))
      .set_min_length(1)
      .set_max_length(3)
      .set_required(true);

    modal.add_component(text_input);

    event.dialog(modal);
    return;
  }

  // Handle leaderboard pagination
  if (button_id == "lb_prev" || button_id == "lb_next" || button_id == "lb_first" || button_id == "lb_last") {
    auto msg_id = event.command.message_id;

    // Fetch from Memcached
    std::optional<LeaderboardState> state_opt;
    try {
      auto& cache = cache::MemcachedCache::instance();
      state_opt = cache.get_leaderboard(msg_id.str());
      spdlog::info("[BTN] Retrieved leaderboard state from cache: {}", state_opt.has_value() ? "success" : "not found");
    } catch (const std::exception& e) {
      spdlog::warn("Failed to fetch leaderboard from cache: {}", e.what());
    }

    if (!state_opt) {
      // Leaderboard expired - buttons should already be removed by timer thread
      spdlog::info("[BTN] Ignoring button click for expired/missing leaderboard {}", msg_id.str());
      return;
    }

    auto state = *state_opt;

    // Update page number
    if (button_id == "lb_first") {
      state.current_page = 0;
    } else if (button_id == "lb_prev" && state.current_page > 0) {
      state.current_page--;
    } else if (button_id == "lb_next" && state.current_page < state.total_pages - 1) {
      state.current_page++;
    } else if (button_id == "lb_last") {
      state.current_page = state.total_pages - 1;
    } else {
      // Button shouldn't be clickable if at boundary, but just in case
      return;
    }

    // Save updated state back to Memcached
    try {
      auto& cache = cache::MemcachedCache::instance();
      cache.cache_leaderboard(msg_id.str(), state);
      spdlog::info("[BTN] Saved updated state to cache, page={}/{}", state.current_page + 1, state.total_pages);
    } catch (const std::exception& e) {
      spdlog::warn("Failed to save updated leaderboard to cache: {}", e.what());
    }

    // Build updated message with new page
    dpp::message updated_msg = build_lb_page(state);

    // Update the message
    spdlog::info("[BTN] Updating message with new page {}", state.current_page + 1);
    event.reply(dpp::ir_update_message, updated_msg);
  }

  // Handle recent scores pagination
  if (button_id == "rs_prev" || button_id == "rs_next" || button_id == "rs_first" || button_id == "rs_last" || button_id == "rs_refresh") {
    auto msg_id = event.command.message_id;

    // Fetch from Memcached
    std::optional<RecentScoreState> state_opt;
    try {
      auto& cache = cache::MemcachedCache::instance();
      state_opt = cache.get_recent_scores(msg_id.str());
      spdlog::info("[BTN] Retrieved recent scores state from cache: {}", state_opt.has_value() ? "success" : "not found");
    } catch (const std::exception& e) {
      spdlog::warn("Failed to fetch recent scores from cache: {}", e.what());
    }

    if (!state_opt) {
      // Recent scores expired - buttons should already be removed by timer thread
      spdlog::info("[BTN] Ignoring button click for expired/missing recent scores {}", msg_id.str());
      return;
    }

    auto state = *state_opt;

    // Handle refresh button - re-fetch scores
    if (button_id == "rs_refresh") {
      state.refresh_count++;

      std::string scores_response;
      if (state.use_best_scores) {
        scores_response = request.get_user_best_scores(std::to_string(state.osu_user_id), "osu", 100, 0);
      } else {
        scores_response = request.get_user_recent_scores(
          std::to_string(state.osu_user_id), state.include_fails, "osu", 50, 0);
      }

      if (!scores_response.empty()) {
        try {
          json scores_json = json::parse(scores_response);
          std::vector<Score> new_scores;

          // Parse all scores (beatmap will be fetched when displaying)
          for (const auto& score_j : scores_json) {
            Score score;
            score.from_json(score_j);
            new_scores.push_back(std::move(score));
          }

          if (!new_scores.empty()) {
            // Check if there are new scores by comparing first score
            bool has_new_scores = false;
            if (state.scores.empty() ||
                new_scores[0].get_created_at() != state.scores[0].get_created_at() ||
                new_scores[0].get_total_score() != state.scores[0].get_total_score()) {
              has_new_scores = true;
            }

            if (has_new_scores) {
              state.scores = std::move(new_scores);
              state.current_index = 0;
              spdlog::info("[BTN] Refreshed scores, got {} new scores", state.scores.size());
            } else {
              spdlog::info("[BTN] Refreshed scores, no new scores found (refresh count: {})", state.refresh_count);
            }
          }
        } catch (const json::exception& e) {
          spdlog::error("Failed to parse refreshed scores: {}", e.what());
        }
      }
    } else {
      // Update index for navigation buttons
      if (button_id == "rs_first") {
        state.current_index = 0;
      } else if (button_id == "rs_prev" && state.current_index > 0) {
        state.current_index--;
      } else if (button_id == "rs_next" && state.current_index < state.scores.size() - 1) {
        state.current_index++;
      } else if (button_id == "rs_last") {
        state.current_index = state.scores.size() - 1;
      } else {
        // Button shouldn't be clickable if at boundary, but just in case
        return;
      }
    }

    // Save updated state back to Memcached
    try {
      auto& cache = cache::MemcachedCache::instance();
      cache.cache_recent_scores(msg_id.str(), state);
      spdlog::info("[BTN] Saved updated recent scores state to cache, index={}/{}", state.current_index + 1, state.scores.size());
    } catch (const std::exception& e) {
      spdlog::warn("Failed to save updated recent scores to cache: {}", e.what());
    }

    // Build updated message with new score
    dpp::message updated_msg = build_rs_page(state);

    // Update the message
    spdlog::info("[BTN] Updating message with new score {}/{}", state.current_index + 1, state.scores.size());
    event.reply(dpp::ir_update_message, updated_msg);
  }
}

void Bot::create_lb_message(const dpp::message_create_t& event,
                            const std::string& mods_filter,
                            const std::optional<std::string>& beatmap_id_override,
                            LbSortMethod sort_method) {
  dpp::snowflake channel_id = event.msg.channel_id;

  // Use override if provided, otherwise get from chat context
  std::string beatmap_id;
  if (beatmap_id_override.has_value()) {
    beatmap_id = *beatmap_id_override;
  } else {
    std::string stored_id = chat_context_service.get_beatmap_id(channel_id);
    beatmap_id = beatmap_resolver_service.resolve_beatmap_id(stored_id);
  }

  if (beatmap_id.empty()) {
    event.reply(message_presenter.build_error_message(error_messages::NO_BEATMAP_IN_CHANNEL));
    return;
  }

  // Show typing indicator
  bot.channel_typing(event.msg.channel_id);

  auto start = std::chrono::steady_clock::now();

  std::string response_beatmap = request.get_beatmap(beatmap_id);
  if (response_beatmap.empty()) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - start).count();

    if (elapsed > 8) {
      event.reply(message_presenter.build_error_message(
        fmt::format(error_messages::API_TIMEOUT_FORMAT, elapsed)));
    } else {
      event.reply(message_presenter.build_error_message(error_messages::API_NO_RESPONSE));
    }
    spdlog::error("Unable to send request");
    return;
  }

  Beatmap            beatmap(response_beatmap);

  // Download .osz file asynchronously
  uint32_t beatmapset_id = beatmap.get_beatmapset_id();
  std::jthread([this, beatmapset_id]() {
    beatmap_downloader.download_osz(beatmapset_id);
  }).detach();

  // Fetch mod-adjusted beatmap attributes if mods filter is present
  if (!mods_filter.empty()) {
    uint32_t mods_bitset = utils::mods_string_to_bitset(mods_filter);
    std::string attributes_response = request.get_beatmap_attributes(beatmap_id, mods_bitset);
    if (!attributes_response.empty()) {
      try {
        json attributes_json = json::parse(attributes_response);
        beatmap.set_modded_attributes(attributes_json);
      } catch (const json::exception& e) {
        spdlog::error("Failed to parse beatmap attributes: {}", e.what());
      }
    }
  }

  auto user_mappings = user_mapping_service.get_all_mappings();
  std::vector<Score> scores(user_mappings.size());

  // Create stable index mapping to avoid race condition
  std::unordered_map<std::string, size_t> user_to_index;
  size_t idx = 0;
  for (const auto& [dis_id, user_id] : user_mappings) {
    user_to_index[user_id] = idx++;
  }

  // Parse required mods for filtering
  std::vector<std::string> required_mods;
  if (!mods_filter.empty()) {
    for (size_t i = 0; i + 1 < mods_filter.length(); i += 2) {
      required_mods.push_back(mods_filter.substr(i, 2));
    }
  }

  spdlog::info("[!lb] Fetching scores and usernames for {} users", user_mappings.size());

  // force tbb parallelization ???
  arena.execute([&]() { tbb::parallel_for_each(std::begin(user_mappings), std::end(user_mappings),
    [&](const auto& pair) {
      const auto& [dis_id, user_id] = pair;
      auto& score = scores[user_to_index[user_id]];
      std::string scores_j = request.get_user_beatmap_score(beatmap_id, user_id, true);
      if (!scores_j.empty()) {
        json j = json::parse(scores_j);
        j = j["scores"];

        // Filter scores by mods if filter is specified
        if (!required_mods.empty()) {
          auto filtered_scores = json::array();
          for (const auto& score_json : j) {
            // Parse mods from this score
            std::string score_mods_str;
            if (score_json.contains("mods") && score_json["mods"].is_array()) {
              for (const auto& mod : score_json["mods"]) {
                score_mods_str += mod.get<std::string>();
              }
            }
            if (score_mods_str.empty()) score_mods_str = "NM";

            // Check if all required mods are present
            bool has_all_mods = true;
            for (const auto& required_mod : required_mods) {
              if (score_mods_str.find(required_mod) == std::string::npos) {
                has_all_mods = false;
                break;
              }
            }

            if (has_all_mods) {
              filtered_scores.push_back(score_json);
            }
          }
          j = filtered_scores;
        }

        // If we have scores after filtering, take the best one
        if (!j.empty()) {
          // sort specific user's scores
          std::sort(j.begin(), j.end(), [](const json& a, const json& b) {
            return std::make_tuple(a["pp"], a["score"]) > std::make_tuple(b["pp"], b["score"]);
          });
          score.from_json(j.at(0));
          std::string username = get_username_cached(score.get_user_id());
          score.set_username(username);
        }
      }
    });
  });
  // Remove empty scores
  for (auto it = scores.begin(); it != scores.end();) {
    if (!it->is_empty) ++it;
    else scores.erase(it);
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::steady_clock::now() - start).count();

  if (scores.empty()) {
    if (elapsed > 8) {
      event.reply(message_presenter.build_error_message(
        fmt::format(error_messages::API_TIMEOUT_FORMAT, elapsed)));
      spdlog::warn("[CMD] !lb took {}s and found no scores (slow API response)", elapsed);
    } else {
      event.reply(message_presenter.build_error_message(
        fmt::format(error_messages::NO_SCORES_ON_BEATMAP_FORMAT, beatmap.to_string())));
    }
    return;
  }

  // Sort scores based on selected method
  if (scores.size() > 1) {
    switch (sort_method) {
      case LbSortMethod::Score:
        stdr::sort(scores, [](const Score& a, const Score& b) {
          return a.get_total_score() > b.get_total_score();
        });
        break;
      case LbSortMethod::Acc:
        stdr::sort(scores, [](const Score& a, const Score& b) {
          return a.get_accuracy() > b.get_accuracy();
        });
        break;
      case LbSortMethod::Combo:
        stdr::sort(scores, [](const Score& a, const Score& b) {
          return a.get_max_combo() > b.get_max_combo();
        });
        break;
      case LbSortMethod::Date:
        stdr::sort(scores, [](const Score& a, const Score& b) {
          return a.get_created_at() > b.get_created_at();
        });
        break;
      case LbSortMethod::PP:
      default:
        stdr::sort(scores, [](const Score& a, const Score& b) {
          return std::make_tuple(a.get_pp(), a.get_total_score()) >
              std::make_tuple(b.get_pp(), b.get_total_score());
        });
        break;
    }
  }

  if (elapsed > 8) {
    spdlog::warn("[CMD] !lb took {}s to complete (slow API response)", elapsed);
  }

  // Create leaderboard state and build first page
  std::string beatmap_mode = beatmap.get_mode(); // Extract mode before moving
  LeaderboardState lb_state(std::move(scores), std::move(beatmap), 0, beatmap_mode, mods_filter, sort_method, event.msg.author.id);
  dpp::message msg = build_lb_page(lb_state);

  // Reply with leaderboard
  event.reply(msg, false, [this, lb_state = std::move(lb_state)](const dpp::confirmation_callback_t& callback) {
    if (callback.is_error()) {
      spdlog::error("Failed to send leaderboard message");
      return;
    }
    auto reply_msg = callback.get<dpp::message>();

    // Store state in Memcached with 5-minute TTL
    bool cache_success = false;
    try {
      auto& cache = cache::MemcachedCache::instance();
      cache.cache_leaderboard(reply_msg.id.str(), lb_state);
      spdlog::debug("Stored leaderboard state for message {} in Memcached", reply_msg.id.str());
      cache_success = true;
    } catch (const std::exception& e) {
      spdlog::error("Failed to cache leaderboard state - pagination will not work: {}", e.what());
      // Continue anyway - we'll still schedule button removal
    }

    // Schedule button removal only if there's more than one page
    if (lb_state.total_pages > 1) {
      dpp::snowflake msg_id = reply_msg.id;
      dpp::snowflake chan_id = reply_msg.channel_id;

      // Remove buttons after 2 minutes
      auto ttl = std::chrono::minutes(2);
      schedule_button_removal(chan_id, msg_id, ttl);
    }
  });
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
    button_click_event(event);
  });
  bot.on_form_submit([this](const dpp::form_submit_t& event) {
    form_submit_event(event);
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
    .beatmap_cache_service = beatmap_cache_service.get()
  });

  // Register text commands
  command_router.set_services(service_container.get());
  register_commands();

  // Start HTTP health check server
  http_server->start();
}

void Bot::register_commands() {
  command_router.register_command(std::make_unique<commands::LbCommand>(*this));
  command_router.register_command(std::make_unique<commands::RsCommand>());  // Uses ServiceContainer
  command_router.register_command(std::make_unique<commands::BgCommand>());  // Uses ServiceContainer
  command_router.register_command(std::make_unique<commands::AudioCommand>());  // Uses ServiceContainer
  command_router.register_command(std::make_unique<commands::MapCommand>());  // Uses ServiceContainer
  command_router.register_command(std::make_unique<commands::CompareCommand>());  // Uses ServiceContainer
  command_router.register_command(std::make_unique<commands::SimCommand>());  // Uses ServiceContainer
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
