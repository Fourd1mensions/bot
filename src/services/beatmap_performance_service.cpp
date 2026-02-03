#include "services/beatmap_performance_service.h"
#include "services/beatmap_cache_service.h"
#include "beatmap_downloader.h"
#include "osu_parser.h"
#include "osu_tools.h"
#include "utils.h"

#ifdef USE_ROSU_PP_SERVICE
#include "services/rosu_pp_client.h"
#endif

#include <spdlog/spdlog.h>

namespace services {

BeatmapPerformanceService::BeatmapPerformanceService(
    BeatmapDownloader& downloader,
    BeatmapCacheService* cache_service,
    const std::string& rosu_pp_address
)
    : downloader_(downloader)
    , cache_service_(cache_service)
{
#ifdef USE_ROSU_PP_SERVICE
    try {
        rosu_pp_client_ = std::make_unique<RosuPpClient>(rosu_pp_address);
        spdlog::info("[PerfService] Using rosu-pp-service at {}", rosu_pp_address);
    } catch (const std::exception& e) {
        spdlog::warn("[PerfService] Failed to connect to rosu-pp-service: {}. Falling back to osu-tools.", e.what());
    }
#else
    (void)rosu_pp_address;  // Suppress unused warning
    spdlog::info("[PerfService] Using osu-tools (rosu-pp-service not compiled in)");
#endif
}

BeatmapPerformanceService::~BeatmapPerformanceService() = default;

bool BeatmapPerformanceService::is_rosu_pp_available() const {
#ifdef USE_ROSU_PP_SERVICE
    return rosu_pp_client_ && rosu_pp_client_->is_connected();
#else
    return false;
#endif
}

void BeatmapPerformanceService::set_cache_service(BeatmapCacheService* cache_service) {
    cache_service_ = cache_service;
}

std::optional<std::string> BeatmapPerformanceService::get_osu_file_path(
    uint32_t beatmapset_id,
    uint32_t beatmap_id
) {
    // Fast path: download just the .osu file directly from osu.ppy.sh
    auto osu_path = get_osu_file_direct(beatmap_id);

    if (osu_path) {
        // Queue full .osz download in background for bg/audio commands
        if (cache_service_ && !downloader_.beatmapset_exists(beatmapset_id)) {
            spdlog::debug("[PerfService] Queuing background .osz download for beatmapset {}", beatmapset_id);
            cache_service_->queue_download(beatmapset_id);
        }
        return osu_path;
    }

    // Fallback: try .osz extraction if direct download failed
    spdlog::info("[PerfService] Direct .osu download failed, trying .osz extraction for beatmap {}", beatmap_id);

    if (downloader_.download_osz(beatmapset_id)) {
        auto extract_id = downloader_.create_extract(beatmapset_id);
        if (extract_id) {
            auto extract_path = downloader_.get_extract_path(*extract_id);
            if (extract_path) {
                auto osu_file = osu_parser::find_osu_file(*extract_path, beatmap_id);
                if (osu_file) {
                    return osu_file->string();
                }
            }
        }
    }

    spdlog::warn("[PerfService] All download methods failed for beatmap {}", beatmap_id);
    return std::nullopt;
}

std::optional<std::string> BeatmapPerformanceService::get_osu_file_direct(uint32_t beatmap_id) {
    auto osu_path = downloader_.download_osu_file(beatmap_id);
    if (!osu_path) {
        spdlog::debug("[PerfService] Failed to download .osu file for beatmap {}", beatmap_id);
        return std::nullopt;
    }
    return osu_path->string();
}

std::optional<BeatmapDifficultyAttrs> BeatmapPerformanceService::get_difficulty(
    uint32_t beatmapset_id,
    uint32_t beatmap_id,
    const std::string& mods
) {
    auto osu_path = get_osu_file_path(beatmapset_id, beatmap_id);
    if (!osu_path) {
        return std::nullopt;
    }

    auto beatmap_opt = osu_parser::parse_osu_file(*osu_path);
    if (!beatmap_opt) {
        spdlog::warn("[PerfService] Failed to parse .osu file: {}", *osu_path);
        return std::nullopt;
    }

    // Apply mods to base difficulty
    auto mod_flags = utils::parse_mod_flags(mods);
    auto modded = osu_parser::apply_mods(
        *beatmap_opt,
        mod_flags.has_ez,
        mod_flags.has_hr,
        mod_flags.has_dt,
        mod_flags.has_ht
    );

    BeatmapDifficultyAttrs result;
    result.approach_rate = modded.approach_rate;
    result.overall_difficulty = modded.overall_difficulty;
    result.circle_size = modded.circle_size;
    result.hp_drain_rate = modded.hp_drain_rate;
    result.total_objects = modded.total_objects;

    // Get star rating from osu-tools
    auto diff_result = osu_tools::calculate_difficulty(*osu_path, mods);
    if (diff_result) {
        result.star_rating = diff_result->star_rating;
        result.aim_difficulty = diff_result->aim_difficulty;
        result.speed_difficulty = diff_result->speed_difficulty;
        result.max_combo = diff_result->max_combo;
    }

    return result;
}

std::optional<PerformanceAttrs> BeatmapPerformanceService::calculate_pp(
    uint32_t beatmapset_id,
    uint32_t beatmap_id,
    const std::string& mode,
    const SimulateParams& params
) {
    spdlog::info("[PerfService] calculate_pp: beatmap={} mode={} mods={} acc={:.2f}%",
        beatmap_id, mode, params.mods, params.accuracy * 100.0);

    auto osu_path = get_osu_file_path(beatmapset_id, beatmap_id);
    if (!osu_path) {
        spdlog::warn("[PerfService] Failed to get .osu file for beatmap {}", beatmap_id);
        return std::nullopt;
    }

#ifdef USE_ROSU_PP_SERVICE
    if (rosu_pp_client_ && rosu_pp_client_->is_connected()) {
        spdlog::info("[PerfService] -> rosu-pp for beatmap {}", beatmap_id);
        return calculate_pp_rosu(*osu_path, mode, params);
    } else {
        spdlog::warn("[PerfService] rosu-pp unavailable, fallback to osu-tools");
    }
#endif

    // Fallback to osu-tools
    spdlog::info("[PerfService] -> osu-tools for beatmap {}", beatmap_id);
    auto result = osu_tools::simulate_performance(
        *osu_path,
        params.accuracy,
        mode,
        params.mods,
        params.combo,
        params.misses,
        params.count_100,
        params.count_50
    );

    if (!result) {
        spdlog::warn("[PerfService] osu-tools PP calculation failed for beatmap {}", beatmap_id);
        return std::nullopt;
    }

    spdlog::info("[PerfService] osu-tools OK: pp={:.2f}", result->pp);
    return PerformanceAttrs{
        .pp = result->pp,
        .aim_pp = result->aim_pp,
        .speed_pp = result->speed_pp,
        .accuracy_pp = result->accuracy_pp
    };
}

std::vector<double> BeatmapPerformanceService::calculate_pp_at_accuracies(
    uint32_t beatmap_id,
    const std::string& mode,
    const std::string& mods,
    const std::vector<double>& accuracy_levels,
    BeatmapDifficultyAttrs* out_difficulty
) {
    spdlog::info("[PerfService] calculate_pp_at_accuracies: beatmap={} mode={} mods={} levels={}",
        beatmap_id, mode, mods, accuracy_levels.size());

    auto osu_path = get_osu_file_direct(beatmap_id);
    if (!osu_path) {
        spdlog::warn("[PerfService] Failed to get .osu file for beatmap {}", beatmap_id);
        return {};
    }

#ifdef USE_ROSU_PP_SERVICE
    if (rosu_pp_client_ && rosu_pp_client_->is_connected()) {
        spdlog::info("[PerfService] -> rosu-pp batch for {} levels", accuracy_levels.size());
        return calculate_pp_at_accuracies_rosu(*osu_path, mode, mods, accuracy_levels, out_difficulty);
    } else {
        spdlog::warn("[PerfService] rosu-pp unavailable for batch, using osu-tools");
    }
#endif

    // Fallback to osu-tools
    std::vector<double> pp_values;
    pp_values.reserve(accuracy_levels.size());

    bool first = true;
    for (double acc : accuracy_levels) {
        auto result = osu_tools::simulate_performance(
            *osu_path,
            acc,
            mode,
            mods.empty() ? "NM" : mods
        );

        if (result) {
            pp_values.push_back(result->pp);

            // Capture difficulty info from first calculation
            if (first && out_difficulty) {
                out_difficulty->star_rating = result->difficulty.star_rating;
                out_difficulty->aim_difficulty = result->difficulty.aim_difficulty;
                out_difficulty->speed_difficulty = result->difficulty.speed_difficulty;
                out_difficulty->max_combo = result->difficulty.max_combo;
                first = false;
            }
        } else {
            pp_values.push_back(0.0);
        }
    }

    // Also parse .osu file for AR/OD/CS/HP if difficulty output requested
    if (out_difficulty && !pp_values.empty()) {
        auto beatmap_opt = osu_parser::parse_osu_file(*osu_path);
        if (beatmap_opt) {
            auto mod_flags = utils::parse_mod_flags(mods);
            auto modded = osu_parser::apply_mods(
                *beatmap_opt,
                mod_flags.has_ez,
                mod_flags.has_hr,
                mod_flags.has_dt,
                mod_flags.has_ht
            );
            out_difficulty->approach_rate = modded.approach_rate;
            out_difficulty->overall_difficulty = modded.overall_difficulty;
            out_difficulty->circle_size = modded.circle_size;
            out_difficulty->hp_drain_rate = modded.hp_drain_rate;
            out_difficulty->total_objects = modded.total_objects;
        }
    }

    return pp_values;
}

std::optional<BeatmapDifficultyAttrs> BeatmapPerformanceService::get_difficulty_direct(
    uint32_t beatmap_id,
    const std::string& mods
) {
    auto osu_path = get_osu_file_direct(beatmap_id);
    if (!osu_path) {
        return std::nullopt;
    }

    auto beatmap_opt = osu_parser::parse_osu_file(*osu_path);
    if (!beatmap_opt) {
        spdlog::warn("[PerfService] Failed to parse .osu file: {}", *osu_path);
        return std::nullopt;
    }

    auto mod_flags = utils::parse_mod_flags(mods);
    auto modded = osu_parser::apply_mods(
        *beatmap_opt,
        mod_flags.has_ez,
        mod_flags.has_hr,
        mod_flags.has_dt,
        mod_flags.has_ht
    );

    BeatmapDifficultyAttrs result;
    result.approach_rate = modded.approach_rate;
    result.overall_difficulty = modded.overall_difficulty;
    result.circle_size = modded.circle_size;
    result.hp_drain_rate = modded.hp_drain_rate;
    result.total_objects = modded.total_objects;

#ifdef USE_ROSU_PP_SERVICE
    // Use rosu-pp-service for star rating if available
    if (rosu_pp_client_ && rosu_pp_client_->is_connected()) {
        RosuDifficultySettings settings;
        settings.mods = RosuPpClient::parse_mods(mods);

        auto diff_result = rosu_pp_client_->calculate_difficulty(*osu_path, std::nullopt, settings);
        if (diff_result) {
            result.star_rating = diff_result->stars;
            result.aim_difficulty = diff_result->aim;
            result.speed_difficulty = diff_result->speed;
            result.max_combo = diff_result->max_combo;
            return result;
        }
    }
#endif

    // Fallback to osu-tools
    auto diff_result = osu_tools::calculate_difficulty(*osu_path, mods);
    if (diff_result) {
        result.star_rating = diff_result->star_rating;
        result.aim_difficulty = diff_result->aim_difficulty;
        result.speed_difficulty = diff_result->speed_difficulty;
        result.max_combo = diff_result->max_combo;
    }

    return result;
}

#ifdef USE_ROSU_PP_SERVICE

std::optional<PerformanceAttrs> BeatmapPerformanceService::calculate_pp_rosu(
    const std::string& osu_path,
    const std::string& mode,
    const SimulateParams& params
) {
    RosuDifficultySettings settings;
    settings.mods = RosuPpClient::parse_mods(params.mods);

    RosuScoreParams score;
    score.accuracy = params.accuracy * 100.0;  // Convert 0-1 to 0-100
    if (params.combo > 0) {
        score.combo = params.combo;
    }
    if (params.misses > 0) {
        score.misses = params.misses;
    }
    if (params.count_100 >= 0) {
        score.n100 = params.count_100;
    }
    if (params.count_50 >= 0) {
        score.n50 = params.count_50;
    }

    auto game_mode = RosuPpClient::parse_mode(mode);

    auto result = rosu_pp_client_->calculate_performance(osu_path, game_mode, settings, score);
    if (!result) {
        spdlog::debug("[PerfService] rosu-pp calculation failed, falling back to osu-tools");
        return std::nullopt;
    }

    return PerformanceAttrs{
        .pp = result->pp,
        .aim_pp = result->pp_aim,
        .speed_pp = result->pp_speed,
        .accuracy_pp = result->pp_acc
    };
}

std::vector<double> BeatmapPerformanceService::calculate_pp_at_accuracies_rosu(
    const std::string& osu_path,
    const std::string& mode,
    const std::string& mods,
    const std::vector<double>& accuracy_levels,
    BeatmapDifficultyAttrs* out_difficulty
) {
    RosuDifficultySettings settings;
    settings.mods = RosuPpClient::parse_mods(mods);

    // Build score params for each accuracy level
    std::vector<RosuScoreParams> scores;
    scores.reserve(accuracy_levels.size());

    for (double acc : accuracy_levels) {
        RosuScoreParams score;
        score.accuracy = acc * 100.0;  // Convert 0-1 to 0-100
        scores.push_back(score);
    }

    auto game_mode = RosuPpClient::parse_mode(mode);

    // Batch calculate all accuracies at once
    auto results = rosu_pp_client_->calculate_batch(osu_path, game_mode, settings, scores);

    std::vector<double> pp_values;
    pp_values.reserve(results.size());

    for (size_t i = 0; i < results.size(); ++i) {
        pp_values.push_back(results[i].pp);

        // Capture difficulty info from first result
        if (i == 0 && out_difficulty && results[i].pp > 0) {
            out_difficulty->star_rating = results[i].stars;
            out_difficulty->aim_difficulty = results[i].difficulty.aim;
            out_difficulty->speed_difficulty = results[i].difficulty.speed;
            out_difficulty->max_combo = results[i].max_combo;

            // All difficulty attributes from rosu-pp (post-mod values)
            out_difficulty->approach_rate = static_cast<float>(results[i].difficulty.ar);
            out_difficulty->overall_difficulty = static_cast<float>(results[i].difficulty.od);
            out_difficulty->hp_drain_rate = static_cast<float>(results[i].difficulty.hp);
            out_difficulty->circle_size = static_cast<float>(results[i].difficulty.cs);
            out_difficulty->total_objects = results[i].difficulty.n_circles +
                                           results[i].difficulty.n_sliders +
                                           results[i].difficulty.n_spinners;
        }
    }

    return pp_values;
}

#endif // USE_ROSU_PP_SERVICE

} // namespace services
