#include "services/recent_score_service.h"
#include "services/beatmap_performance_service.h"
#include "services/message_presenter_service.h"
#include <requests.h>
#include <osu.h>
#include <osu_tools.h>
#include <utils.h>
#include <database.h>
#include <cache.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace services {

RecentScoreService::RecentScoreService(
    Request& request,
    BeatmapPerformanceService& performance_service,
    MessagePresenterService& message_presenter,
    dpp::cluster& bot
)
    : request_(request)
    , performance_service_(performance_service)
    , message_presenter_(message_presenter)
    , bot_(bot)
{}

dpp::message RecentScoreService::build_page(RecentScoreState& state) {
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

            MessagePresenterService::RecentScoreCacheData cache_data{
                .title = cached_data["title"].get<std::string>(),
                .url = cached_data["url"].get<std::string>(),
                .description = cached_data["description"].get<std::string>(),
                .thumbnail = cached_data["thumbnail"].get<std::string>(),
                .beatmap_info = cached_data["beatmap_info"].get<std::string>(),
                .footer = cached_data["footer"].get<std::string>(),
                .timestamp = cached_data["timestamp"].get<time_t>()
            };

            PaginationInfo pagination{
                .current = state.current_index,
                .total = state.scores.size(),
                .has_refresh = true,
                .refresh_count = state.refresh_count
            };

            spdlog::debug("[RS] Using cached page data for index {}", state.current_index);
            return message_presenter_.build_from_cache_data(cache_data, pagination);
        } catch (const std::exception& e) {
            spdlog::warn("[RS] Failed to use cached page data: {}, rebuilding", e.what());
            // Fall through to rebuild
        }
    }

    // Get beatmap info from the score's beatmap data
    std::string beatmap_response = request_.get_beatmap(std::to_string(score.get_beatmap_id()));
    if (beatmap_response.empty()) {
        dpp::message err_msg;
        err_msg.set_content("failed to fetch beatmap data");
        return err_msg;
    }

    Beatmap beatmap(beatmap_response);

    // Get mod-adjusted difficulty if mods are present
    if (!score.get_mods().empty() && score.get_mods() != "NM") {
        uint32_t mods_bitset = utils::mods_string_to_bitset(score.get_mods());
        std::string attributes_response = request_.get_beatmap_attributes(
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
        osu_file_path_opt = performance_service_.get_osu_file_path(beatmapset_id, beatmap_id);
    } else {
        // Use performance service to get difficulty
        osu_file_path_opt = performance_service_.get_osu_file_path(beatmapset_id, beatmap_id);
        if (osu_file_path_opt) {
            auto diff_opt = performance_service_.get_difficulty(beatmapset_id, beatmap_id, score.get_mods());
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
            int fc_total_objects = score.get_count_300() + score.get_count_100() + score.get_count_50() + score.get_count_miss();
            double fc_accuracy = ((score.get_count_300() + score.get_count_miss()) * 300.0 + score.get_count_100() * 100.0 + score.get_count_50() * 50.0) / (fc_total_objects * 300.0);

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
    PPInfo pp_info{
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
    DifficultyInfo difficulty_info{
        .approach_rate = approach_rate,
        .overall_difficulty = overall_difficulty,
        .circle_size = circle_size,
        .hp_drain_rate = hp_drain_rate
    };

    // Prepare pagination info
    PaginationInfo pagination{
        .current = state.current_index,
        .total = state.scores.size(),
        .has_refresh = true,
        .refresh_count = state.refresh_count
    };

    // Calculate modded BPM and length
    float modded_bpm = utils::apply_speed_mods_to_bpm(beatmap.get_bpm(), score.get_mods());
    uint32_t modded_length = utils::apply_speed_mods_to_length(beatmap.get_total_length(), score.get_mods());

    // Build message using presenter service
    std::string score_type = state.use_best_scores ? "best" : "recent";
    dpp::message msg = message_presenter_.build_recent_score_page(
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
        auto cache_data = message_presenter_.build_recent_score_cache_data(
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

void RecentScoreService::remove_message_components(dpp::snowflake channel_id, dpp::snowflake message_id) {
    // Get the message first, then remove components
    bot_.message_get(message_id, channel_id, [this, message_id](const dpp::confirmation_callback_t& callback) {
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
                spdlog::warn("Failed to edit message {} to remove buttons: {}", message_id.str(), edit_callback.get_error().message);
            }
        });
    });
}

void RecentScoreService::schedule_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id, std::chrono::minutes ttl) {
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
