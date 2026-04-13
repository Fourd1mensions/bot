#include "services/command_params_service.h"

#include <algorithm>
#include <sstream>
#include <cctype>

#include <spdlog/spdlog.h>

namespace services {

std::vector<std::string> CommandParamsService::tokenize(const std::string& params) {
    std::vector<std::string> tokens;
    std::istringstream iss(params);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::optional<std::string> CommandParamsService::parse_discord_mention(const std::string& mention) {
    // Handle formats: <@123456>, <@!123456>
    if (mention.size() < 4) return std::nullopt;
    if (mention[0] != '<' || mention[1] != '@' || mention.back() != '>') {
        return std::nullopt;
    }

    std::string id_str = mention.substr(2, mention.size() - 3);

    // Remove '!' if present (some mentions have <@!ID> format)
    if (!id_str.empty() && id_str[0] == '!') {
        id_str = id_str.substr(1);
    }

    // Validate it's a number
    if (id_str.empty()) return std::nullopt;
    for (char c : id_str) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }

    return id_str;
}

std::optional<std::string> CommandParamsService::normalize_mode(const std::string& mode_input) {
    std::string mode = mode_input;
    std::transform(mode.begin(), mode.end(), mode.begin(),
        [](unsigned char c) { return std::tolower(c); });

    if (mode == "osu" || mode == "std" || mode == "standard") {
        return "osu";
    }
    if (mode == "taiko") {
        return "taiko";
    }
    if (mode == "catch" || mode == "ctb" || mode == "fruits") {
        return "fruits";
    }
    if (mode == "mania") {
        return "mania";
    }

    return std::nullopt;
}

std::string CommandParamsService::join_username_parts(const std::vector<std::string>& parts) {
    if (parts.empty()) return "";

    std::string result = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        result += " " + parts[i];
    }
    return result;
}

RecentScoreParams CommandParamsService::parse_recent_params(
    const std::string& params,
    const std::string& default_mode
) const {
    RecentScoreParams result;
    result.mode = default_mode;

    if (params.empty()) {
        return result;
    }

    auto tokens = tokenize(params);
    std::vector<std::string> username_parts;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];

        if (t == "-p") {
            // Passed only flag
            result.include_fails = false;
        } else if (t == "-b") {
            // Best scores flag (top 100)
            result.use_best_scores = true;
        } else if (t == "-i") {
            // Index flag with value
            if (i + 1 >= tokens.size()) {
                result.errors.push_back("Flag `-i` requires a number (e.g., `-i 5`)");
                continue;
            }
            const auto& value = tokens[i + 1];
            try {
                int idx = std::stoi(value);
                if (idx > 0) {
                    result.score_index = idx - 1; // Convert to 0-based
                } else {
                    result.errors.push_back("Index must be positive (e.g., `-i 1`)");
                }
            } catch (...) {
                result.errors.push_back("Invalid index `" + value + "`. Must be a number.");
            }
            ++i; // Skip next token (the index value)
        } else if (t == "-m") {
            // Mode flag with value
            if (i + 1 >= tokens.size()) {
                result.errors.push_back("Flag `-m` requires a mode (osu, taiko, catch, mania)");
                continue;
            }
            const auto& value = tokens[i + 1];
            auto normalized = normalize_mode(value);
            if (normalized) {
                result.mode = *normalized;
            } else {
                result.errors.push_back("Unknown mode `" + value + "`. Use: osu, taiko, catch, mania");
            }
            ++i; // Skip next token (the mode value)
        } else if (!t.empty() && t[0] == '-') {
            // Unknown flag
            result.warnings.push_back("Unknown flag `" + t + "`");
        } else if (!t.empty() && t[0] == '+') {
            // Mods are not supported in !rs, suggest using !lb or !c
            result.warnings.push_back("Mods filter is not supported in `!rs`. Use `!c` to filter by mods.");
        } else if (!t.empty()) {
            // Username part
            username_parts.push_back(t);
        }
    }

    result.username = join_username_parts(username_parts);
    return result;
}

CompareParams CommandParamsService::parse_compare_params(const std::string& params) const {
    CompareParams result;

    if (params.empty()) {
        return result;
    }

    auto tokens = tokenize(params);
    std::vector<std::string> username_parts;

    for (const auto& t : tokens) {
        if (!t.empty() && t[0] == '+') {
            // Mods filter
            result.mods_filter = t.substr(1);
            std::transform(result.mods_filter.begin(), result.mods_filter.end(),
                result.mods_filter.begin(), [](unsigned char c) { return std::toupper(c); });
        } else if (!t.empty() && t[0] == '-') {
            // Unknown flag - !c doesn't support flags
            result.warnings.push_back("Unknown flag `" + t + "`. `!c` only supports `+MODS` filter.");
        } else if (!t.empty()) {
            // Username part
            username_parts.push_back(t);
        }
    }

    result.username = join_username_parts(username_parts);
    return result;
}

} // namespace services
