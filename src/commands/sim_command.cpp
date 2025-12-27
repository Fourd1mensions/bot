#include "commands/sim_command.h"
#include "services/service_container.h"
#include "services/chat_context_service.h"
#include "services/beatmap_resolver_service.h"
#include "services/message_presenter_service.h"
#include "services/beatmap_performance_service.h"
#include <requests.h>
#include <osu.h>
#include <osu_tools.h>
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
    return {"!sim"};
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

SimCommand::ParsedParams SimCommand::parse(const std::string& content) const {
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
        result.error_message =
            "Usage: `!sim[:mode] <accuracy>% [+mods] [-c COMBO] [-n100 X] [-n50 X] [-n0 X] [-r RATIO]`\n"
            "Modes: `osu` (default), `taiko`, `catch`, `mania`\n"
            "Examples:\n"
            "• `!sim 99% +HDDT` - standard osu!\n"
            "• `!sim:taiko 100% +HR` - taiko mode\n"
            "• `!sim 100% -n100 5 -c 1500` - 5x100, 1500x combo\n"
            "• `!sim:mania 99% -r 0.95` - mania with 95% ratio";
        return result;
    }

    // Extract accuracy value
    size_t start_pos = content.find_first_of("0123456789");
    if (start_pos == std::string::npos || start_pos >= percent_pos) {
        result.valid = false;
        result.error_message = "Invalid accuracy format. Example: `!sim 99%`";
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
        result.error_message = "Invalid accuracy value. Example: `!sim 99%`";
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
        parsed = parse(ctx.content);
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

    // Calculate PP using osu-tools
    std::string mods = parsed.mods_filter.empty() ? "NM" : parsed.mods_filter;

    auto result = osu_tools::simulate_performance(
        *osu_file_path,
        parsed.accuracy,
        parsed.mode,
        mods,
        parsed.combo,
        parsed.misses,
        parsed.count_100,
        parsed.count_50
    );

    if (!result.has_value()) {
        ctx.reply(s->message_presenter.build_error_message("Failed to simulate score. Please try again."));
        spdlog::error("[SIM] Failed to simulate score for beatmap {} with {}% accuracy and mods {}",
            beatmap_id, parsed.accuracy * 100, parsed.mods_filter);
        return;
    }

    // Build response message
    std::string mode_display = parsed.mode;
    std::transform(mode_display.begin(), mode_display.end(), mode_display.begin(), ::toupper);

    std::string content = fmt::format("**Simulated Play on {}**", title);
    if (parsed.mode != "osu") {
        content += fmt::format(" [{}]", mode_display);
    }
    content += "\n";
    content += fmt::format(":star: **{:.2f}★**\n\n", result->difficulty.star_rating);

    content += "**Score Parameters:**\n";
    content += fmt::format("• Accuracy: **{:.2f}%**\n", parsed.accuracy * 100);
    if (parsed.count_100 >= 0 || parsed.count_50 >= 0 || parsed.misses > 0) {
        content += "• Hit counts:";
        if (parsed.count_100 >= 0) content += fmt::format(" **{}**x100", parsed.count_100);
        if (parsed.count_50 >= 0) content += fmt::format(" **{}**x50", parsed.count_50);
        if (parsed.misses > 0) content += fmt::format(" **{}**xMiss", parsed.misses);
        content += "\n";
    }
    content += fmt::format("• Mods: **{}**\n", mods);
    if (parsed.combo > 0) {
        content += fmt::format("• Combo: **{}x** (max: **{}x**)\n", parsed.combo, result->difficulty.max_combo);
    } else {
        content += fmt::format("• Max Combo: **{}x**\n", result->difficulty.max_combo);
    }
    if (parsed.mode == "mania" && parsed.ratio > 0.0) {
        content += fmt::format("• Ratio: **{:.2f}**\n", parsed.ratio);
    }
    content += "\n";

    content += "**Performance:**\n";
    content += fmt::format("• **{:.0f}pp** total\n", result->pp);
    content += fmt::format("• Aim: **{:.0f}pp**\n", result->aim_pp);
    content += fmt::format("• Speed: **{:.0f}pp**\n", result->speed_pp);
    content += fmt::format("• Accuracy: **{:.0f}pp**\n\n", result->accuracy_pp);

    content += "**Difficulty:**\n";
    content += fmt::format("• Aim: **{:.2f}★**\n", result->difficulty.aim_difficulty);
    content += fmt::format("• Speed: **{:.2f}★**\n", result->difficulty.speed_difficulty);

    ctx.reply(content);
}

} // namespace commands
