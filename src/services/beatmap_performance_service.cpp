#include "services/beatmap_performance_service.h"
#include "beatmap_downloader.h"
#include "osu_parser.h"
#include "osu_tools.h"
#include "utils.h"

#include <spdlog/spdlog.h>

namespace services {

BeatmapPerformanceService::BeatmapPerformanceService(BeatmapDownloader& downloader)
    : downloader_(downloader) {}

std::optional<std::string> BeatmapPerformanceService::get_osu_file_path(
    uint32_t beatmapset_id,
    uint32_t beatmap_id
) {
    if (!downloader_.download_osz(beatmapset_id)) {
        spdlog::debug("[PerfService] Failed to download osz for beatmapset {}", beatmapset_id);
        return std::nullopt;
    }

    auto extract_id = downloader_.create_extract(beatmapset_id);
    if (!extract_id) {
        spdlog::debug("[PerfService] Failed to create extract for beatmapset {}", beatmapset_id);
        return std::nullopt;
    }

    auto extract_path = downloader_.get_extract_path(*extract_id);
    if (!extract_path) {
        spdlog::debug("[PerfService] Failed to get extract path for {}", *extract_id);
        return std::nullopt;
    }

    auto osu_file = osu_parser::find_osu_file(*extract_path, beatmap_id);
    if (!osu_file) {
        spdlog::debug("[PerfService] Failed to find .osu file for beatmap {} in {}",
            beatmap_id, extract_path->string());
        return std::nullopt;
    }

    return osu_file->string();
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
    auto osu_path = get_osu_file_path(beatmapset_id, beatmap_id);
    if (!osu_path) {
        return std::nullopt;
    }

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
        spdlog::debug("[PerfService] PP calculation failed for beatmap {}", beatmap_id);
        return std::nullopt;
    }

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
    auto osu_path = get_osu_file_direct(beatmap_id);
    if (!osu_path) {
        return {};
    }

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

} // namespace services
