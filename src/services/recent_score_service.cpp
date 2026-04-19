#include "services/recent_score_service.h"
#include "services/beatmap_performance_service.h"
#include "services/message_presenter_service.h"
#include "services/user_settings_service.h"
#include <requests.h>
#include <osu.h>
#include <utils.h>
#include <database.h>
#include <cache.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <future>
#include <algorithm>

using json = nlohmann::json;

namespace services {

RecentScoreService::RecentScoreService(Request&                   request,
                                       BeatmapPerformanceService& performance_service,
                                       MessagePresenterService&   message_presenter,
                                       dpp::cluster&              bot) :
    request_(request), performance_service_(performance_service),
    message_presenter_(message_presenter), bot_(bot) {}

dpp::message RecentScoreService::build_page(RecentScoreState& state) {
  if (state.scores.empty() || state.current_index >= state.scores.size()) {
    dpp::message err_msg;
    err_msg.set_content("no score to display");
    return err_msg;
  }

  const Score& score = state.scores[state.current_index];
  spdlog::info("[RS] build_page called for index {}/{}, beatmap_id={}, api_pp={:.2f}",
               state.current_index + 1, state.scores.size(), score.get_beatmap_id(),
               score.get_pp());

  // Check cache first for fast navigation
  auto cache_it = state.page_content_cache.find(state.current_index);
  if (cache_it != state.page_content_cache.end()) {
    try {
      json                                          cached_data = json::parse(cache_it->second);

      MessagePresenterService::RecentScoreCacheData cache_data{
          .content      = cached_data.value("content", ""),
          .title        = cached_data.value("title", ""),
          .url          = cached_data.value("url", ""),
          .description  = cached_data.value("description", ""),
          .thumbnail    = cached_data.value("thumbnail", ""),
          .beatmap_info = cached_data.value("beatmap_info", ""),
          .footer       = cached_data.value("footer", ""),
          .footer_icon  = cached_data.value("footer_icon", ""),
          .timestamp    = cached_data.value("timestamp", static_cast<time_t>(0)),
          .username     = cached_data.value("username", ""),
          .user_id      = cached_data.value("user_id", static_cast<uint64_t>(0)),
          .preset = services::embed_preset_from_string(cached_data.value("preset", "classic")),
          .color  = cached_data.value("color", static_cast<uint32_t>(0x7c4dff))};

      // Skip cache if missing user info (old cache format)
      if (cache_data.user_id == 0) {
        throw std::runtime_error("old cache format");
      }

      PaginationInfo pagination{.current       = state.current_index,
                                .total         = state.scores.size(),
                                .has_refresh   = true,
                                .refresh_count = state.refresh_count};

      spdlog::debug("[RS] Using cached page data for index {}", state.current_index);
      return message_presenter_.build_from_cache_data(cache_data, pagination);
    } catch (const std::exception& e) {
      spdlog::warn("[RS] Failed to use cached page data: {}, rebuilding", e.what());
      // Fall through to rebuild
    }
  }

  uint32_t    beatmap_id     = score.get_beatmap_id();
  std::string beatmap_id_str = std::to_string(beatmap_id);
  std::string mods_str       = score.get_mods();
  bool        has_mods       = !mods_str.empty() && mods_str != "NM";

  std::string beatmap_response;
  try {
    auto& mc = cache::MemcachedCache::instance();
    if (auto cached = mc.get_cached_beatmap(beatmap_id)) {
      beatmap_response = *cached;
    }
  } catch (...) {}
  if (beatmap_response.empty()) {
    beatmap_response = request_.get_beatmap(beatmap_id_str);
  }
  if (beatmap_response.empty()) {
    dpp::message err_msg;
    err_msg.set_content("failed to fetch beatmap data");
    return err_msg;
  }

  Beatmap beatmap(beatmap_response);

  if (beatmap.is_ranked() || beatmap.is_loved()) {
    try {
      auto& mc = cache::MemcachedCache::instance();
      mc.cache_beatmap(beatmap_id, beatmap_response);
    } catch (...) {}
  }

  if (has_mods) {
    uint32_t    mods_bitset         = utils::mods_string_to_bitset(mods_str);
    std::string attributes_response = request_.get_beatmap_attributes(beatmap_id_str, mods_bitset);
    if (!attributes_response.empty()) {
      try {
        json attributes_json = json::parse(attributes_response);
        beatmap.set_modded_attributes(attributes_json);
      } catch (...) {}
    }
  }

  std::future<MapPositionInfo> map_position_future;
  if (score.get_passed()) {
    map_position_future = std::async(std::launch::async, [this, &score]() -> MapPositionInfo {
      MapPositionInfo pos;
      try {
        std::string resp = request_.get_user_beatmap_score(
            std::to_string(score.get_beatmap_id()), std::to_string(score.get_user_id()), false);
        if (!resp.empty()) {
          json map_j = json::parse(resp);
          if (map_j.contains("position"))
            pos.position = map_j["position"].get<int>();
          if (map_j.contains("score") && map_j["score"].contains("pp") &&
              !map_j["score"]["pp"].is_null())
            pos.best_pp = map_j["score"]["pp"].get<double>();
        }
      } catch (const std::exception& e) {
        spdlog::debug("[RS] Failed to fetch map position: {}", e.what());
      }
      return pos;
    });
  }

  float                      approach_rate      = 9.0f;
  float                      overall_difficulty = 9.0f;
  float                      circle_size        = 5.0f;
  float                      hp_drain_rate      = 5.0f;
  int                        total_objects      = 0;
  uint32_t                   beatmapset_id      = beatmap.get_beatmapset_id();
  std::optional<std::string> osu_file_path_opt;

  spdlog::debug("[PP] Processing beatmap_id={}, beatmapset_id={}, mode={}, api_pp={:.2f}",
                beatmap_id, beatmapset_id, score.get_mode(), score.get_pp());

  auto difficulty_cache_it = state.beatmap_difficulty_cache.find(beatmap_id);
  if (difficulty_cache_it != state.beatmap_difficulty_cache.end()) {
    approach_rate      = std::get<0>(difficulty_cache_it->second);
    overall_difficulty = std::get<1>(difficulty_cache_it->second);
    circle_size        = std::get<2>(difficulty_cache_it->second);
    hp_drain_rate      = std::get<3>(difficulty_cache_it->second);
    total_objects      = std::get<4>(difficulty_cache_it->second);
    spdlog::debug("[PP] Using cached difficulty for beatmap {}", beatmap_id);
    osu_file_path_opt = performance_service_.get_osu_file_path(beatmapset_id, beatmap_id);
  } else {
    osu_file_path_opt = performance_service_.get_osu_file_path(beatmapset_id, beatmap_id);
    if (osu_file_path_opt) {
      auto diff_opt = performance_service_.get_difficulty(beatmapset_id, beatmap_id, mods_str);
      if (diff_opt) {
        approach_rate                              = diff_opt->approach_rate;
        overall_difficulty                         = diff_opt->overall_difficulty;
        circle_size                                = diff_opt->circle_size;
        hp_drain_rate                              = diff_opt->hp_drain_rate;
        total_objects                              = diff_opt->total_objects;
        state.beatmap_difficulty_cache[beatmap_id] = std::make_tuple(
            approach_rate, overall_difficulty, circle_size, hp_drain_rate, total_objects);
      }
    }
  }

  int hits_made =
      score.get_count_300() + score.get_count_100() + score.get_count_50() + score.get_count_miss();
  float completion_percent =
      (total_objects > 0) ? (static_cast<float>(hits_made) / total_objects) * 100.0f : 100.0f;

  double current_pp     = score.get_pp();
  double current_aim_pp = 0.0, current_speed_pp = 0.0, current_accuracy_pp = 0.0;

  struct {
    double total_pp    = 0.0;
    double aim_pp      = 0.0;
    double speed_pp    = 0.0;
    double accuracy_pp = 0.0;
  } fc_perf;

  if (osu_file_path_opt.has_value() && score.get_mode() == "osu") {
    SimulateParams current_params;
    current_params.accuracy  = score.get_accuracy();
    current_params.mods      = mods_str;
    current_params.lazer     = score.get_set_on_lazer();
    current_params.combo     = score.get_max_combo();
    current_params.misses    = score.get_count_miss();
    current_params.count_300 = score.get_count_300();
    current_params.count_100 = score.get_count_100();
    current_params.count_50  = score.get_count_50();
    if (!score.get_passed()) {
      current_params.passed_objects = hits_made;
    }

    bool should_calc_fc = (score.get_count_miss() > 0 || !score.get_passed()) && total_objects > 0;
    SimulateParams fc_params;
    if (should_calc_fc) {
      int    passed_objects = hits_made;
      int    remaining      = std::max(0, total_objects - passed_objects);
      int    fc_n300        = score.get_count_300() + remaining;
      int    count_hits     = total_objects - score.get_count_miss();
      double ratio   = (count_hits > 0) ? 1.0 - (static_cast<double>(fc_n300) / count_hits) : 0.0;
      int    new100s = static_cast<int>(std::ceil(ratio * score.get_count_miss()));
      int    misses_as_300s = std::max(0, static_cast<int>(score.get_count_miss()) - new100s);
      fc_n300 += misses_as_300s;

      fc_params.mods      = mods_str;
      fc_params.lazer     = score.get_set_on_lazer();
      fc_params.combo     = 0;
      fc_params.misses    = 0;
      fc_params.count_300 = fc_n300;
      fc_params.count_100 = score.get_count_100() + new100s;
      fc_params.count_50  = score.get_count_50();
    }

    auto current_pp_future =
        std::async(std::launch::async, [this, beatmapset_id, beatmap_id, current_params]() {
          return performance_service_.calculate_pp(beatmapset_id, beatmap_id, "osu",
                                                   current_params);
        });

    std::future<std::optional<PerformanceAttrs>> fc_pp_future;
    if (should_calc_fc) {
      fc_pp_future = std::async(std::launch::async, [this, beatmapset_id, beatmap_id, fc_params]() {
        return performance_service_.calculate_pp(beatmapset_id, beatmap_id, "osu", fc_params);
      });
    }

    auto perf_opt = current_pp_future.get();
    if (perf_opt.has_value()) {
      current_aim_pp      = perf_opt->aim_pp;
      current_speed_pp    = perf_opt->speed_pp;
      current_accuracy_pp = perf_opt->accuracy_pp;
      if (current_pp <= 0.01) {
        current_pp = perf_opt->pp;
      }
    }

    if (should_calc_fc && fc_pp_future.valid()) {
      auto fc_perf_opt = fc_pp_future.get();
      if (fc_perf_opt.has_value()) {
        fc_perf.total_pp    = fc_perf_opt->pp;
        fc_perf.aim_pp      = fc_perf_opt->aim_pp;
        fc_perf.speed_pp    = fc_perf_opt->speed_pp;
        fc_perf.accuracy_pp = fc_perf_opt->accuracy_pp;
      }
    }
  }

  PPInfo pp_info{.current_pp  = current_pp,
                 .fc_pp       = fc_perf.total_pp,
                 .fc_accuracy = 0.0,
                 .has_fc_pp   = false,
                 .aim_pp      = current_aim_pp,
                 .speed_pp    = current_speed_pp,
                 .accuracy_pp = current_accuracy_pp};

  if (score.get_mode() == "osu" && (score.get_count_miss() > 0 || !score.get_passed()) &&
      fc_perf.total_pp > current_pp && total_objects > 0) {
    int    passed_objects = hits_made;
    int    remaining      = std::max(0, total_objects - passed_objects);
    int    fc_n300        = score.get_count_300() + remaining;
    int    count_hits     = total_objects - score.get_count_miss();
    double ratio   = (count_hits > 0) ? 1.0 - (static_cast<double>(fc_n300) / count_hits) : 0.0;
    int    new100s = static_cast<int>(std::ceil(ratio * score.get_count_miss()));
    int    misses_as_300s = std::max(0, static_cast<int>(score.get_count_miss()) - new100s);
    fc_n300 += misses_as_300s;
    int fc_n100 = score.get_count_100() + new100s;
    int fc_n50  = score.get_count_50();
    pp_info.fc_accuracy =
        (fc_n300 * 300.0 + fc_n100 * 100.0 + fc_n50 * 50.0) / (total_objects * 300.0) * 100.0;
    pp_info.has_fc_pp = true;
  }

  DifficultyInfo difficulty_info{.approach_rate      = approach_rate,
                                 .overall_difficulty = overall_difficulty,
                                 .circle_size        = circle_size,
                                 .hp_drain_rate      = hp_drain_rate};

  PaginationInfo pagination{.current       = state.current_index,
                            .total         = state.scores.size(),
                            .has_refresh   = true,
                            .refresh_count = state.refresh_count};

  float          modded_bpm = utils::apply_speed_mods_to_bpm(beatmap.get_bpm(), mods_str);
  uint32_t modded_length = utils::apply_speed_mods_to_length(beatmap.get_total_length(), mods_str);

  int      try_number = 0;
  if (!state.use_best_scores) {
    uint32_t current_beatmap_id = score.get_beatmap_id();
    int      count              = 1;
    for (size_t i = state.current_index + 1; i < state.scores.size(); ++i) {
      if (state.scores[i].get_beatmap_id() == current_beatmap_id)
        count++;
      else
        break;
    }
    try_number = count;
  }

  // === Resolve map position future ===
  MapPositionInfo map_position;
  if (score.get_passed() && map_position_future.valid()) {
    map_position = map_position_future.get();
  }

  std::string  score_type = state.use_best_scores ? "Recent Best" : "Recent";
  dpp::message msg        = message_presenter_.build_recent_score_page(
      score, beatmap, difficulty_info, pp_info, pagination, score_type, completion_percent,
      modded_bpm, modded_length, state.preset, try_number, map_position, state.caller_discord_id);

  try {
    auto cache_data = message_presenter_.build_recent_score_cache_data(
        score, beatmap, difficulty_info, pp_info, pagination, score_type, completion_percent,
        modded_bpm, modded_length, state.preset, try_number, map_position, state.caller_discord_id);

    json page_data;
    page_data["title"]        = cache_data.title;
    page_data["url"]          = cache_data.url;
    page_data["description"]  = cache_data.description;
    page_data["thumbnail"]    = cache_data.thumbnail;
    page_data["beatmap_info"] = cache_data.beatmap_info;
    page_data["footer"]       = cache_data.footer;
    page_data["footer_icon"]  = cache_data.footer_icon;
    page_data["timestamp"]    = cache_data.timestamp;
    page_data["username"]     = cache_data.username;
    page_data["user_id"]      = cache_data.user_id;
    page_data["preset"]       = services::embed_preset_to_string(cache_data.preset);
    page_data["content"]      = cache_data.content;
    page_data["color"]        = cache_data.color;

    state.page_content_cache[state.current_index] = page_data.dump();
    spdlog::debug("[RS] Cached page data for index {}", state.current_index);
  } catch (const std::exception& e) {
    spdlog::warn("[RS] Failed to cache page data: {}", e.what());
  }

  return msg;
}

void RecentScoreService::remove_message_components(dpp::snowflake channel_id,
                                                   dpp::snowflake message_id) {
  // Get the message first, then remove components
  bot_.message_get(
      message_id, channel_id, [this, message_id](const dpp::confirmation_callback_t& callback) {
        if (callback.is_error()) {
          spdlog::warn("Failed to get message {} for button removal", message_id.str());
          return;
        }

        auto msg = callback.get<dpp::message>();
        msg.components.clear();

        // Edit message to remove components only
        bot_.message_edit(msg, [message_id](const dpp::confirmation_callback_t& edit_callback) {
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
            spdlog::warn("Failed to edit message {} to remove buttons: {}", message_id.str(),
                         edit_callback.get_error().message);
          }
        });
      });
}

void RecentScoreService::schedule_button_removal(dpp::snowflake       channel_id,
                                                 dpp::snowflake       message_id,
                                                 std::chrono::minutes ttl) {
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
      spdlog::warn("Failed to clear pending button removal from database: {}", e.what());
    }
  }).detach();
}

} // namespace services
