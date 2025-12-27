#include "commands/map_command.h"
#include "services/service_container.h"
#include "services/chat_context_service.h"
#include "services/beatmap_resolver_service.h"
#include "services/message_presenter_service.h"
#include "services/beatmap_performance_service.h"
#include <requests.h>
#include <osu.h>
#include <utils.h>
#include <database.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace commands {

std::vector<std::string> MapCommand::get_aliases() const {
    return {"!map", "!m"};
}

void MapCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[map] ServiceContainer is null");
        return;
    }

    std::string mods_filter;
    std::vector<std::string> warnings;

    if (ctx.is_slash()) {
        if (auto mods = ctx.get_string_param("mods")) {
            auto validation = utils::validate_mods(*mods);
            mods_filter = validation.normalized;

            if (!validation.invalid.empty()) {
                std::string invalid_list;
                for (const auto& m : validation.invalid) {
                    if (!invalid_list.empty()) invalid_list += ", ";
                    invalid_list += m;
                }
                warnings.push_back(fmt::format("Unknown mod(s): {}. Ignored.", invalid_list));
            }
            if (validation.has_incompatible) {
                warnings.push_back(validation.incompatible_msg);
            }
        }
    } else {
        std::string raw_mods = utils::extract_mods_from_content(ctx.content);
        if (!raw_mods.empty()) {
            auto validation = utils::validate_mods(raw_mods);
            mods_filter = validation.normalized;

            if (!validation.invalid.empty()) {
                std::string invalid_list;
                for (const auto& m : validation.invalid) {
                    if (!invalid_list.empty()) invalid_list += ", ";
                    invalid_list += m;
                }
                warnings.push_back(fmt::format("Unknown mod(s): {}. Ignored.", invalid_list));
            }
            if (validation.has_incompatible) {
                warnings.push_back(validation.incompatible_msg);
            }
        }
    }

    // Log warnings
    if (!warnings.empty()) {
        for (const auto& w : warnings) {
            spdlog::info("[map] Warning: {}", w);
        }
    }

    // Resolve beatmap from context
    std::string stored_value = s->chat_context_service.get_beatmap_id(ctx.channel_id());
    auto beatmap_result = s->beatmap_resolver_service.resolve(stored_value);
    if (!beatmap_result) {
        ctx.reply(s->message_presenter.build_error_message(beatmap_result.error_message));
        return;
    }
    uint32_t beatmap_id = beatmap_result.beatmap_id;
    uint32_t beatmapset_id = beatmap_result.beatmapset_id;

    // Get beatmap info from API
    std::string beatmap_json = s->request.get_beatmap(std::to_string(beatmap_id));

    if (beatmap_json.empty()) {
        ctx.reply(s->message_presenter.build_error_message("Failed to fetch beatmap information."));
        return;
    }

    auto beatmap_data = json::parse(beatmap_json);
    if (beatmap_id == 0 && beatmap_data.contains("beatmap_id")) {
        beatmap_id = beatmap_data["beatmap_id"].get<uint32_t>();
    }
    if (beatmapset_id == 0 && beatmap_data.contains("beatmapset_id")) {
        beatmapset_id = beatmap_data["beatmapset_id"].get<uint32_t>();
    }

    Beatmap beatmap(beatmap_data);
    std::string beatmap_mode = beatmap.get_mode();
    std::string title = beatmap.to_string();
    if (!mods_filter.empty()) {
        title += fmt::format(" +{}", mods_filter);
    }

    // Cache beatmap_id -> beatmapset_id mapping for faster lookups
    try {
        auto& db = db::Database::instance();
        db.cache_beatmap_id(beatmap_id, beatmapset_id, beatmap_mode);
    } catch (const std::exception& e) {
        spdlog::debug("[MAP] Failed to cache beatmap mapping: {}", e.what());
    }

    // Calculate PP at multiple accuracy levels using performance service
    std::vector<double> acc_levels = {0.90, 0.95, 0.99, 1.00};
    services::BeatmapDifficultyAttrs perf_difficulty;

    auto pp_values = s->performance_service.calculate_pp_at_accuracies(
        beatmap_id,
        beatmap_mode,
        mods_filter,
        acc_levels,
        &perf_difficulty
    );

    if (pp_values.empty()) {
        ctx.reply(s->message_presenter.build_error_message("Failed to calculate PP values."));
        return;
    }

    // Prepare difficulty info for presenter
    services::DifficultyInfo difficulty_info{
        .approach_rate = perf_difficulty.approach_rate,
        .overall_difficulty = perf_difficulty.overall_difficulty,
        .circle_size = perf_difficulty.circle_size,
        .hp_drain_rate = perf_difficulty.hp_drain_rate,
        .star_rating = perf_difficulty.star_rating,
        .aim_difficulty = perf_difficulty.aim_difficulty,
        .speed_difficulty = perf_difficulty.speed_difficulty,
        .max_combo = perf_difficulty.max_combo
    };

    // Calculate modded BPM and length for speed mods
    float modded_bpm = utils::apply_speed_mods_to_bpm(beatmap.get_bpm(), mods_filter);
    uint32_t modded_length = utils::apply_speed_mods_to_length(beatmap.get_total_length(), mods_filter);

    // Build message using presenter service
    dpp::message msg = s->message_presenter.build_map_info(
        beatmap,
        difficulty_info,
        pp_values,
        mods_filter,
        beatmapset_id,
        modded_bpm,
        modded_length
    );
    ctx.reply(msg);
}

} // namespace commands
