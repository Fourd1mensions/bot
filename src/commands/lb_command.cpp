#include "commands/lb_command.h"
#include "services/service_container.h"
#include "services/leaderboard_service.h"
#include <osu.h>
#include <utils.h>
#include <algorithm>
#include <regex>
#include <sstream>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace commands {

std::vector<std::string> LbCommand::get_aliases() const {
    return {"!lb", "!ди"};
}

LbParams LbCommand::parse_params(const std::string& content) const {
    LbParams params;

    // Tokenize the content (skip command itself)
    std::istringstream iss(content);
    std::string token;
    std::vector<std::string> tokens;

    // Skip the command (!lb or !ди)
    iss >> token;

    while (iss >> token) {
        tokens.push_back(token);
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];

        // Check for sort flag: -s <method>
        if ((t == "-s" || t == "--sort") && i + 1 < tokens.size()) {
            auto [method, warning] = parse_sort_method(tokens[i + 1]);
            params.sort_method = method;
            if (!warning.empty()) {
                params.warnings.push_back(warning);
            }
            ++i;  // Skip next token
            continue;
        }

        // Check for mods: +HD, +HDDT, etc.
        if (t.length() > 1 && t[0] == '+') {
            std::string mods_input = t.substr(1);
            auto validation = utils::validate_mods(mods_input);

            params.mods_filter = validation.normalized;

            // Add warnings for invalid mods
            if (!validation.invalid.empty()) {
                std::string invalid_list;
                for (const auto& m : validation.invalid) {
                    if (!invalid_list.empty()) invalid_list += ", ";
                    invalid_list += m;
                }
                params.warnings.push_back(fmt::format("Unknown mod(s): {}. Ignored.", invalid_list));
            }

            // Add warning for incompatible mods
            if (validation.has_incompatible) {
                params.warnings.push_back(validation.incompatible_msg);
            }
            continue;
        }

        // Check if it's a beatmap URL or ID
        auto beatmap_id = extract_beatmap_id(t);
        if (beatmap_id) {
            params.beatmap_id = beatmap_id;
            continue;
        }
    }

    return params;
}

std::optional<std::string> LbCommand::extract_beatmap_id(const std::string& token) const {
    // Try to match osu.ppy.sh/beatmaps/<id> or osu.ppy.sh/b/<id>
    std::regex beatmap_regex(R"(https?://osu\.ppy\.sh/(?:beatmaps/|b/)(\d+))");
    std::smatch match;
    if (std::regex_search(token, match, beatmap_regex)) {
        return match[1].str();
    }

    // Try to match osu.ppy.sh/beatmapsets/<setid>#<mode>/<beatmap_id>
    std::regex beatmapset_regex(R"(https?://osu\.ppy\.sh/beatmapsets/\d+#(?:osu|taiko|fruits|mania)/(\d+))");
    if (std::regex_search(token, match, beatmapset_regex)) {
        return match[1].str();
    }

    // Check if it's a raw numeric ID
    if (std::all_of(token.begin(), token.end(), ::isdigit) && !token.empty()) {
        return token;
    }

    return std::nullopt;
}

std::pair<LbSortMethod, std::string> LbCommand::parse_sort_method(const std::string& method) const {
    std::string lower = method;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    if (lower == "pp" || lower == "p") {
        return {LbSortMethod::PP, ""};
    }
    if (lower == "score" || lower == "s") {
        return {LbSortMethod::Score, ""};
    }
    if (lower == "acc" || lower == "accuracy" || lower == "a") {
        return {LbSortMethod::Acc, ""};
    }
    if (lower == "combo" || lower == "c") {
        return {LbSortMethod::Combo, ""};
    }
    if (lower == "date" || lower == "recent" || lower == "d" || lower == "r") {
        return {LbSortMethod::Date, ""};
    }
    // Unknown method - return default with warning
    return {LbSortMethod::PP, fmt::format("Unknown sort '{}'. Using pp. Valid: pp, score, acc, combo, date", method)};
}

void LbCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[lb] ServiceContainer is null");
        return;
    }

    LbParams params;

    if (ctx.is_slash()) {
        // Get parameters directly from slash command with validation
        if (auto mods = ctx.get_string_param("mods")) {
            auto validation = utils::validate_mods(*mods);
            params.mods_filter = validation.normalized;

            if (!validation.invalid.empty()) {
                std::string invalid_list;
                for (const auto& m : validation.invalid) {
                    if (!invalid_list.empty()) invalid_list += ", ";
                    invalid_list += m;
                }
                params.warnings.push_back(fmt::format("Unknown mod(s): {}. Ignored.", invalid_list));
            }
            if (validation.has_incompatible) {
                params.warnings.push_back(validation.incompatible_msg);
            }
        }
        if (auto sort = ctx.get_string_param("sort")) {
            auto [method, warning] = parse_sort_method(*sort);
            params.sort_method = method;
            if (!warning.empty()) {
                params.warnings.push_back(warning);
            }
        }
        if (auto beatmap = ctx.get_string_param("beatmap")) {
            params.beatmap_id = extract_beatmap_id(*beatmap);
            if (!params.beatmap_id) {
                // Maybe it's just a raw ID
                params.beatmap_id = *beatmap;
            }
        }
    } else {
        // Parse text command
        params = parse_params(ctx.content);
    }

    // Show warnings to user if any
    if (!params.warnings.empty()) {
        std::string warning_msg = ":warning: ";
        for (size_t i = 0; i < params.warnings.size(); ++i) {
            if (i > 0) warning_msg += " • ";
            warning_msg += params.warnings[i];
        }
        // For now, just log warnings. Could also prepend to response.
        spdlog::info("[lb] Warnings: {}", warning_msg);
    }

    s->leaderboard_service.create_leaderboard(ctx, params.mods_filter, params.beatmap_id, params.sort_method);
}

} // namespace commands
