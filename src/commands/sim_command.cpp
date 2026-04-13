#include "commands/sim_command.h"
#include "services/service_container.h"
#include "services/chat_context_service.h"
#include "services/beatmap_resolver_service.h"
#include "services/message_presenter_service.h"
#include "services/beatmap_performance_service.h"
#include "services/embed_template_service.h"
#include "services/user_settings_service.h"
#include <requests.h>
#include <osu.h>
#include <database.h>
#include <utils.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace commands {

std::vector<std::string> SimCommand::get_aliases() const {
    return {"sim"};
}

int SimCommand::parse_int_param(const std::string& content, const std::string& param) const {
    size_t param_pos = content.find(param);
    if (param_pos != std::string::npos) {
        size_t value_start = param_pos + param.length();
        // Skip spaces
        while (value_start < content.length() && content[value_start] == ' ') {
            value_start++;
        }
        // Extract number
        size_t value_end = value_start;
        while (value_end < content.length() && std::isdigit(content[value_end])) {
            value_end++;
        }
        if (value_end > value_start) {
            try {
                return std::stoi(content.substr(value_start, value_end - value_start));
            } catch (...) {}
        }
    }
    return -1;
}

double SimCommand::parse_ratio(const std::string& content) const {
    size_t ratio_pos = content.find("-r");
    if (ratio_pos != std::string::npos) {
        size_t value_start = ratio_pos + 2;
        while (value_start < content.length() && content[value_start] == ' ') {
            value_start++;
        }
        size_t value_end = value_start;
        while (value_end < content.length() &&
               (std::isdigit(content[value_end]) || content[value_end] == '.')) {
            value_end++;
        }
        if (value_end > value_start) {
            try {
                return std::stod(content.substr(value_start, value_end - value_start));
            } catch (...) {}
        }
    }
    return -1.0;
}

SimCommand::ParsedParams SimCommand::parse(const std::string& content, const std::string& prefix) const {
    ParsedParams result;

    // Parse mode (e.g., "!sim:taiko")
    size_t colon_pos = content.find(':');
    size_t space_pos = content.find(' ');
    if (colon_pos != std::string::npos && (space_pos == std::string::npos || colon_pos < space_pos)) {
        size_t mode_end = content.find(' ', colon_pos);
        result.mode = content.substr(colon_pos + 1, mode_end - colon_pos - 1);
        std::transform(result.mode.begin(), result.mode.end(), result.mode.begin(), ::tolower);

        if (result.mode != "osu" && result.mode != "taiko" && result.mode != "catch" && result.mode != "mania") {
            result.valid = false;
            result.error_message = "Invalid mode. Supported modes: `osu`, `taiko`, `catch`, `mania`";
            return result;
        }
    }

    // Find percentage
    size_t percent_pos = content.find('%');
    if (percent_pos == std::string::npos) {
        result.valid = false;
        result.error_message = fmt::format(
            "Usage: `{}sim[:mode] <accuracy>% [+mods] [-c COMBO] [-n100 X] [-n50 X] [-n0 X] [-r RATIO]`\n"
            "Modes: `osu` (default), `taiko`, `catch`, `mania`\n"
            "Examples:\n"
            "• `{}sim 99% +HDDT` - standard osu!\n"
            "• `{}sim:taiko 100% +HR` - taiko mode\n"
            "• `{}sim 100% -n100 5 -c 1500` - 5x100, 1500x combo\n"
            "• `{}sim:mania 99% -r 0.95` - mania with 95% ratio",
            prefix, prefix, prefix, prefix, prefix);
        return result;
    }

    // Extract accuracy value
    size_t start_pos = content.find_first_of("0123456789");
    if (start_pos == std::string::npos || start_pos >= percent_pos) {
        result.valid = false;
        result.error_message = fmt::format("Invalid accuracy format. Example: `{}sim 99%`", prefix);
        return result;
    }

    std::string acc_str = content.substr(start_pos, percent_pos - start_pos);
    try {
        result.accuracy = std::stod(acc_str) / 100.0;
        if (result.accuracy < 0.0 || result.accuracy > 1.0) {
            result.valid = false;
            result.error_message = "Accuracy must be between 0% and 100%.";
            return result;
        }
    } catch (const std::exception&) {
        result.valid = false;
        result.error_message = fmt::format("Invalid accuracy value. Example: `{}sim 99%`", prefix);
        return result;
    }

    // Parse mods
    size_t plus_pos = content.find('+');
    if (plus_pos != std::string::npos) {
        size_t mods_end = content.find(" -", plus_pos);
        std::string mods_substr = (mods_end != std::string::npos)
            ? content.substr(plus_pos + 1, mods_end - plus_pos - 1)
            : content.substr(plus_pos + 1);

        result.mods_filter = mods_substr;
        result.mods_filter.erase(std::remove(result.mods_filter.begin(), result.mods_filter.end(), ' '), result.mods_filter.end());
        result.mods_filter.erase(std::remove(result.mods_filter.begin(), result.mods_filter.end(), '+'), result.mods_filter.end());
        std::transform(result.mods_filter.begin(), result.mods_filter.end(), result.mods_filter.begin(),
            [](unsigned char c) { return std::toupper(c); });
    }

    // Parse hit count parameters
    result.count_100 = parse_int_param(content, "-n100");
    result.count_50 = parse_int_param(content, "-n50");
    int n0 = parse_int_param(content, "-n0");
    if (n0 >= 0) {
        result.misses = n0;
    }

    // Parse combo
    int combo_param = parse_int_param(content, "-c");
    if (combo_param > 0) {
        result.combo = combo_param;
    }

    // Parse ratio (mania only)
    if (result.mode == "mania") {
        result.ratio = parse_ratio(content);
    }

    return result;
}

void SimCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[sim] ServiceContainer is null");
        return;
    }

    // Show typing indicator (only for text commands)
    if (!ctx.is_slash()) {
        s->bot.channel_typing(ctx.channel_id());
    }

    ParsedParams parsed;
    std::vector<std::string> warnings;

    if (ctx.is_slash()) {
        // Get parameters directly from slash command
        if (auto accuracy = ctx.get_double_param("accuracy")) {
            parsed.accuracy = *accuracy / 100.0; // Convert from % to decimal
        } else {
            ctx.reply("Accuracy is required.");
            return;
        }
        if (auto mods = ctx.get_string_param("mods")) {
            auto validation = utils::validate_mods(*mods);
            parsed.mods_filter = validation.normalized;

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
        if (auto mode = ctx.get_string_param("mode")) {
            parsed.mode = *mode;
        }
        if (auto combo = ctx.get_int_param("combo")) {
            parsed.combo = static_cast<int>(*combo);
        }
        if (auto n100 = ctx.get_int_param("n100")) {
            parsed.count_100 = static_cast<int>(*n100);
        }
        if (auto n50 = ctx.get_int_param("n50")) {
            parsed.count_50 = static_cast<int>(*n50);
        }
        if (auto misses = ctx.get_int_param("misses")) {
            parsed.misses = static_cast<int>(*misses);
        }
    } else {
        // Parse text command
        parsed = parse(ctx.content, ctx.prefix);
        if (!parsed.valid) {
            ctx.reply(parsed.error_message);
            return;
        }

        // Validate mods from text command
        if (!parsed.mods_filter.empty()) {
            auto validation = utils::validate_mods(parsed.mods_filter);
            parsed.mods_filter = validation.normalized;

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
    for (const auto& w : warnings) {
        spdlog::info("[sim] Warning: {}", w);
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
    // Get beatmapset_id from API response if not already set
    if (beatmapset_id == 0 && beatmap_data.contains("beatmapset_id")) {
        beatmapset_id = beatmap_data["beatmapset_id"].get<uint32_t>();
    }

    Beatmap beatmap(beatmap_data);
    std::string beatmap_mode = beatmap.get_mode();
    std::string title = beatmap.to_string();
    if (!parsed.mods_filter.empty()) {
        title += fmt::format(" +{}", parsed.mods_filter);
    }

    // Cache beatmap_id -> beatmapset_id mapping for faster lookups
    try {
        auto& db = db::Database::instance();
        db.cache_beatmap_id(beatmap_id, beatmapset_id, beatmap_mode);
    } catch (const std::exception& e) {
        spdlog::debug("[SIM] Failed to cache beatmap mapping: {}", e.what());
    }

    // Get .osu file path using performance service
    auto osu_file_path = s->performance_service.get_osu_file_direct(beatmap_id);
    if (!osu_file_path) {
        ctx.reply(s->message_presenter.build_error_message("Failed to download .osu file."));
        return;
    }

    // Calculate PP using performance service
    std::string mods = parsed.mods_filter.empty() ? "NM" : parsed.mods_filter;

    // Get difficulty attributes first
    auto diff_opt = s->performance_service.get_difficulty_direct(beatmap_id, mods);
    if (!diff_opt) {
        ctx.reply(s->message_presenter.build_error_message("Failed to get beatmap difficulty."));
        return;
    }

    services::SimulateParams params;
    params.accuracy = parsed.accuracy;
    params.mods = mods;
    params.combo = parsed.combo;
    params.misses = parsed.misses;
    params.count_100 = parsed.count_100;
    params.count_50 = parsed.count_50;

    auto result = s->performance_service.calculate_pp(beatmapset_id, beatmap_id, parsed.mode, params);

    if (!result.has_value()) {
        ctx.reply(s->message_presenter.build_error_message("Failed to simulate score. Please try again."));
        spdlog::error("[SIM] Failed to simulate score for beatmap {} with {}% accuracy and mods {}",
            beatmap_id, parsed.accuracy * 100, parsed.mods_filter);
        return;
    }

    // Use difficulty from separate call
    double star_rating = diff_opt->star_rating;
    int max_combo = diff_opt->max_combo;

    // Build values map for template rendering
    std::unordered_map<std::string, std::string> values;
    values["title"] = title;
    values["mode"] = parsed.mode;

    {
        std::string mode_display = parsed.mode;
        std::transform(mode_display.begin(), mode_display.end(), mode_display.begin(), ::toupper);
        values["mode_line"] = (!parsed.mode.empty() && parsed.mode != "osu") ? fmt::format(" [{}]", mode_display) : "";
    }

    values["gamemode_string"] = utils::gamemode_to_string(parsed.mode);

    values["sr"] = fmt::format("{:.2f}", star_rating);
    values["acc"] = fmt::format("{:.2f}", parsed.accuracy * 100);
    values["mods"] = mods;
    values["combo"] = std::to_string(parsed.combo);
    values["max_combo"] = std::to_string(max_combo);
    values["pp"] = fmt::format("{:.0f}", result->pp);
    values["aim_pp"] = fmt::format("{:.0f}", result->aim_pp);
    values["speed_pp"] = fmt::format("{:.0f}", result->speed_pp);
    values["accuracy_pp"] = fmt::format("{:.0f}", result->accuracy_pp);
    values["aim_diff"] = fmt::format("{:.2f}", diff_opt->aim_difficulty);
    values["speed_diff"] = fmt::format("{:.2f}", diff_opt->speed_difficulty);

    // Pre-formatted lines
    std::string hit_counts_line;
    if (parsed.count_100 >= 0 || parsed.count_50 >= 0 || parsed.misses > 0) {
        hit_counts_line = "\xe2\x80\xa2 Hit counts:";
        if (parsed.count_100 >= 0) { hit_counts_line += fmt::format(" **{}**x100", parsed.count_100); values["count_100"] = std::to_string(parsed.count_100); }
        if (parsed.count_50 >= 0)  { hit_counts_line += fmt::format(" **{}**x50", parsed.count_50);   values["count_50"] = std::to_string(parsed.count_50); }
        if (parsed.misses > 0)     { hit_counts_line += fmt::format(" **{}**xMiss", parsed.misses);   values["misses"] = std::to_string(parsed.misses); }
        hit_counts_line += "\n";
    }
    values["hit_counts_line"] = hit_counts_line;

    if (parsed.combo > 0) {
        values["combo_line"] = fmt::format("\xe2\x80\xa2 Combo: **{}x** (max: **{}x**)\n", parsed.combo, max_combo);
    } else {
        values["combo_line"] = fmt::format("\xe2\x80\xa2 Max Combo: **{}x**\n", max_combo);
    }

    if (parsed.mode == "mania" && parsed.ratio > 0.0) {
        values["ratio"] = fmt::format("{:.2f}", parsed.ratio);
        values["ratio_line"] = fmt::format("\xe2\x80\xa2 Ratio: **{:.2f}**\n", parsed.ratio);
    } else {
        values["ratio_line"] = "";
    }

    // Get template and render (supports custom preset)
    services::TemplateFields tmpl_fields;
    if (s->template_service) {
        auto preset = s->user_settings_service.get_preset(ctx.author_id());
        if (preset == services::EmbedPreset::Custom) {
            tmpl_fields = s->template_service->get_user_fields(ctx.author_id(), "sim", "custom");
        } else {
            tmpl_fields = s->template_service->get_fields("sim");
        }
    } else {
        tmpl_fields = services::EmbedTemplateService::get_default_fields("sim");
    }

    std::string body_tmpl = tmpl_fields.count("body") ? tmpl_fields.at("body") : "";
    std::string content = services::render_template(body_tmpl, values);

    ctx.reply(content);
}

} // namespace commands
