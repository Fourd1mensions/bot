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

bool LbCommand::matches(const CommandContext& ctx) const {
    auto check_boundary = [](const std::string& str, size_t prefix_len) {
        if (str.length() == prefix_len) return true;  // Exact match
        char next = str[prefix_len];
        return next == ' ' || next == ':' || next == '\t';
    };

    // Check ASCII alias in lowercase content
    if (ctx.content_lower.find("!lb") == 0 && check_boundary(ctx.content_lower, 3)) return true;
    // Check Cyrillic aliases in original content (tolower doesn't work with UTF-8)
    // Support both lowercase and uppercase variants
    // Note: "!ди" is 5 bytes in UTF-8 (1 + 2 + 2)
    if ((ctx.content.find("!ди") == 0 || ctx.content.find("!ДИ") == 0) && check_boundary(ctx.content, 5)) return true;
    return false;
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
        if (t == "-s" || t == "--sort") {
            if (i + 1 >= tokens.size()) {
                params.warnings.push_back("Flag `-s` requires a sort method (pp, score, acc, combo, date)");
                continue;
            }
            auto [method, warning] = parse_sort_method(tokens[i + 1]);
            params.sort_method = method;
            if (!warning.empty()) {
                params.warnings.push_back(warning);
            }
            ++i;  // Skip next token
            continue;
        }

        // Check for unknown flags
        if (!t.empty() && t[0] == '-') {
            params.warnings.push_back(fmt::format("Unknown flag `{}`. Valid: `-s SORT`", t));
            continue;
        }

        // Check for mods: +HD, +HDDT, etc.
        if (t.length() > 1 && t[0] == '+') {
            std::string mods_input = t.substr(1);
            auto validation = utils::validate_mods(mods_input);

            params.mods_filter = validation.is_nomod ? "NM" : validation.normalized;

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

        // Unknown token - not a URL, ID, flag, or mods
        params.warnings.push_back(fmt::format("Unknown parameter `{}`. Expected beatmap URL/ID, +MODS, or -s SORT.", t));
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
            params.mods_filter = validation.is_nomod ? "NM" : validation.normalized;

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
            if (i > 0) warning_msg += "\n:warning: ";
            warning_msg += params.warnings[i];
        }
        // Send warnings as a separate message before proceeding
        // For text commands, we can send a quick warning
        if (!ctx.is_slash()) {
            std::visit([&warning_msg](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, dpp::message_create_t>) {
                    e.reply(warning_msg, true);  // ephemeral-style (won't actually be ephemeral for text)
                }
            }, ctx.event);
        }
        spdlog::info("[lb] Warnings: {}", warning_msg);
    }

    s->leaderboard_service.create_leaderboard(ctx, params.mods_filter, params.beatmap_id, params.sort_method);
}

} // namespace commands
