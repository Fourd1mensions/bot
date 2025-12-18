#include "commands/lb_command.h"
#include "services/service_container.h"
#include "services/leaderboard_service.h"
#include <osu.h>
#include <algorithm>
#include <regex>
#include <sstream>
#include <spdlog/spdlog.h>

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
            params.sort_method = parse_sort_method(tokens[i + 1]);
            ++i;  // Skip next token
            continue;
        }

        // Check for mods: +HD, +HDDT, etc.
        if (t.length() > 1 && t[0] == '+') {
            std::string mods = t.substr(1);
            // Remove any extra plus signs and convert to uppercase
            mods.erase(std::remove(mods.begin(), mods.end(), '+'), mods.end());
            std::transform(mods.begin(), mods.end(), mods.begin(),
                [](unsigned char c) { return std::toupper(c); });
            params.mods_filter = mods;
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

LbSortMethod LbCommand::parse_sort_method(const std::string& method) const {
    std::string lower = method;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    if (lower == "score" || lower == "s") {
        return LbSortMethod::Score;
    }
    if (lower == "acc" || lower == "accuracy" || lower == "a") {
        return LbSortMethod::Acc;
    }
    if (lower == "combo" || lower == "c") {
        return LbSortMethod::Combo;
    }
    if (lower == "date" || lower == "recent" || lower == "d" || lower == "r") {
        return LbSortMethod::Date;
    }
    // Default to PP
    return LbSortMethod::PP;
}

void LbCommand::execute(const CommandContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[!lb] ServiceContainer is null");
        return;
    }

    auto params = parse_params(ctx.content);
    s->leaderboard_service.create_leaderboard(ctx.event, params.mods_filter, params.beatmap_id, params.sort_method);
}

} // namespace commands
