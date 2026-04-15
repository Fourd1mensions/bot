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
#include <cache.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <random>

using json = nlohmann::json;

namespace commands {

std::vector<std::string> MapCommand::get_aliases() const {
    return {"map", "m"};
}

static std::string generate_search_key() {
    static std::mt19937 rng(std::random_device{}());
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string key(8, ' ');
    for (auto& c : key) {
        c = chars[rng() % (sizeof(chars) - 1)];
    }
    return key;
}

void MapCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[map] ServiceContainer is null");
        return;
    }

    // Check for -? search flag
    std::string content = ctx.content;
    size_t search_flag_pos = content.find("-?");
    if (search_flag_pos != std::string::npos) {
        // Extract search query after -?
        std::string query = content.substr(search_flag_pos + 2);

        // Trim leading spaces
        size_t start = query.find_first_not_of(' ');
        if (start == std::string::npos || query.empty()) {
            ctx.reply(s->message_presenter.build_error_message(
                fmt::format("Usage: `{}map -? <search query>`", ctx.prefix)));
            return;
        }
        query = query.substr(start);

        // Trim trailing spaces
        size_t end = query.find_last_not_of(' ');
        if (end != std::string::npos) {
            query = query.substr(0, end + 1);
        }

        if (!ctx.is_slash()) {
            s->bot.channel_typing(ctx.channel_id());
        }

        spdlog::info("[map] Search query: '{}'", query);

        // Search beatmapsets via API
        std::string result = s->request.search_beatmapsets(query);
        if (result.empty()) {
            ctx.reply(s->message_presenter.build_error_message("Search failed. Please try again."));
            return;
        }

        auto data = json::parse(result);
        auto beatmapsets = data.value("beatmapsets", json::array());

        if (beatmapsets.empty()) {
            ctx.reply(s->message_presenter.build_error_message(
                fmt::format("No results for \"{}\".", query)));
            return;
        }

        // Limit to 25 (Discord select menu max)
        if (beatmapsets.size() > 25) {
            beatmapsets = json::array();
            for (size_t i = 0; i < 25; ++i) {
                beatmapsets.push_back(data["beatmapsets"][i]);
            }
        }

        // Cache search results in memcached
        std::string search_key = generate_search_key();
        auto& cache = cache::MemcachedCache::instance();
        cache.set("map_search:" + search_key, beatmapsets.dump(), std::chrono::seconds(300));

        // Build select menu
        dpp::component select_menu;
        select_menu.set_type(dpp::cot_selectmenu);
        select_menu.set_placeholder("Select beatmapset...");
        select_menu.set_id("map_search_set:" + search_key);

        for (const auto& set : beatmapsets) {
            uint32_t set_id = set.value("id", 0);
            std::string artist = set.value("artist", "Unknown");
            std::string title = set.value("title", "Unknown");
            std::string creator = set.value("creator", "Unknown");

            std::string label = fmt::format("{} - {} by {}", artist, title, creator);
            if (label.size() > 100) label = label.substr(0, 97) + "...";

            // Build difficulty names list
            auto beatmaps = set.value("beatmaps", json::array());
            std::vector<std::string> diff_names;
            for (const auto& bm : beatmaps) {
                diff_names.push_back(bm.value("version", "?"));
            }
            std::string desc;
            for (const auto& d : diff_names) {
                if (!desc.empty()) desc += " | ";
                desc += d;
            }
            if (desc.size() > 100) desc = desc.substr(0, 97) + "...";

            select_menu.add_select_option(
                dpp::select_option(label, std::to_string(set_id), desc)
            );
        }

        dpp::message msg;
        msg.set_content(fmt::format("Search results for **\"{}\"** ({} found):",
            query, beatmapsets.size()));
        msg.add_component(dpp::component().add_component(select_menu));

        ctx.reply(msg);
        return;
    }

    // --- Original map command logic below ---

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

    // Show typing indicator (only for text commands)
    if (!ctx.is_slash()) {
        s->bot.channel_typing(ctx.channel_id());
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

    // Generate strain graph
    auto strain_graph = s->performance_service.get_strain_graph(
        beatmap_id,
        mods_filter,
        900,  // width
        250   // height
    );

    // Build message using presenter service
    auto user_preset = s->user_settings_service.get_preset(ctx.author_id());
    dpp::message msg = s->message_presenter.build_map_info(
        beatmap,
        difficulty_info,
        pp_values,
        mods_filter,
        beatmapset_id,
        modded_bpm,
        modded_length,
        user_preset,
        ctx.author_id()
    );

    // Attach strain graph if available
    if (strain_graph && !strain_graph->empty()) {
        std::string filename = fmt::format("strains_{}.png", beatmap_id);
        msg.add_file(filename, std::string(strain_graph->begin(), strain_graph->end()));

        // Update embed to use the attached image
        if (!msg.embeds.empty()) {
            msg.embeds[0].set_image("attachment://" + filename);
        }
    }

    ctx.reply(msg);
}

} // namespace commands
