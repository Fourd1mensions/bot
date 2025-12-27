#include "commands/compare_command.h"
#include "services/service_container.h"
#include "services/chat_context_service.h"
#include "services/beatmap_resolver_service.h"
#include "services/user_resolver_service.h"
#include "services/message_presenter_service.h"
#include "services/command_params_service.h"
#include <requests.h>
#include <osu.h>
#include <utils.h>
#include <cache.h>
#include <database.h>
#include <error_messages.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace commands {

std::vector<std::string> CompareCommand::get_aliases() const {
    return {"!compare", "!c"};
}

std::string CompareCommand::parse_params(const std::string& content) const {
    size_t cmd_start = content.find('!');
    size_t cmd_end = content.find(' ', cmd_start);
    if (cmd_end == std::string::npos) {
        cmd_end = content.length();
    }

    std::string params = content.length() > cmd_end ? content.substr(cmd_end) : "";

    // Trim leading spaces
    size_t start = params.find_first_not_of(" \t");
    if (start != std::string::npos) {
        params = params.substr(start);
    } else {
        params = "";
    }

    return params;
}

void CompareCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[compare] ServiceContainer is null");
        return;
    }

    auto start = std::chrono::steady_clock::now();

    // Resolve beatmap from context
    std::string stored_value = s->chat_context_service.get_beatmap_id(ctx.channel_id());
    auto beatmap_result = s->beatmap_resolver_service.resolve(stored_value);
    if (!beatmap_result) {
        ctx.reply(s->message_presenter.build_error_message(beatmap_result.error_message));
        return;
    }
    uint32_t beatmap_id = beatmap_result.beatmap_id;

    // Parse parameters
    services::CompareParams parsed;
    std::vector<std::string> warnings;

    if (ctx.is_slash()) {
        if (auto username = ctx.get_string_param("username")) {
            parsed.username = *username;
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
    } else {
        std::string params = parse_params(ctx.content);
        parsed = s->command_params_service.parse_compare_params(params);

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
        spdlog::info("[compare] Warning: {}", w);
    }

    // Resolve osu user_id using service
    auto resolve_result = s->user_resolver_service.resolve(parsed.username, ctx.author_id());
    if (!resolve_result) {
        ctx.reply(s->message_presenter.build_error_message(resolve_result.error_message));
        return;
    }
    int64_t osu_user_id = resolve_result.osu_user_id;

    // Show typing indicator
    if (!ctx.is_slash()) {
        s->bot.channel_typing(ctx.channel_id());
    }

    // Get beatmap info
    std::string beatmap_json = s->request.get_beatmap(std::to_string(beatmap_id));
    if (beatmap_json.empty()) {
        ctx.reply(s->message_presenter.build_error_message("Failed to fetch beatmap information."));
        return;
    }

    json beatmap_data = json::parse(beatmap_json);
    Beatmap beatmap(beatmap_data);

    // Fetch all scores for this user on this beatmap
    std::string scores_json = s->request.get_user_beatmap_score(std::to_string(beatmap_id), std::to_string(osu_user_id), true);
    if (scores_json.empty()) {
        ctx.reply(s->message_presenter.build_error_message("No scores found for this beatmap."));
        return;
    }

    json scores_data = json::parse(scores_json);
    if (!scores_data.contains("scores") || !scores_data["scores"].is_array()) {
        ctx.reply(s->message_presenter.build_error_message(error_messages::PARSE_SCORES_FAILED));
        return;
    }

    json scores_array = scores_data["scores"];

    // Filter by mods if specified
    if (!parsed.mods_filter.empty()) {
        json filtered_scores = json::array();
        for (const auto& score_json : scores_array) {
            std::string score_mods_str;
            if (score_json.contains("mods") && score_json["mods"].is_array()) {
                for (const auto& mod : score_json["mods"]) {
                    score_mods_str += mod.get<std::string>();
                }
            }
            if (score_mods_str.empty()) score_mods_str = "NM";

            // Check if score has the required mods
            bool has_mods = true;
            for (size_t i = 0; i + 1 < parsed.mods_filter.length(); i += 2) {
                std::string required_mod = parsed.mods_filter.substr(i, 2);
                if (score_mods_str.find(required_mod) == std::string::npos) {
                    has_mods = false;
                    break;
                }
            }

            if (has_mods) {
                filtered_scores.push_back(score_json);
            }
        }
        scores_array = filtered_scores;
    }

    if (scores_array.empty()) {
        std::string error_msg = parsed.mods_filter.empty()
            ? std::string(error_messages::NO_SCORES_ON_BEATMAP)
            : fmt::format(error_messages::NO_SCORES_WITH_MODS_FORMAT, parsed.mods_filter);
        ctx.reply(s->message_presenter.build_error_message(error_msg));
        return;
    }

    // Sort by PP (descending), then by score
    std::sort(scores_array.begin(), scores_array.end(), [](const json& a, const json& b) {
        double pp_a = a.value("pp", 0.0);
        double pp_b = b.value("pp", 0.0);
        if (pp_a != pp_b) return pp_a > pp_b;
        return a.value("score", 0) > b.value("score", 0);
    });

    // Get username
    std::string username = s->user_resolver_service.get_username_cached(osu_user_id);

    // Build scores vector
    std::vector<Score> scores;
    for (const auto& score_json : scores_array) {
        scores.emplace_back(score_json);
    }

    // Create CompareState
    CompareState state(std::move(scores), beatmap, username, parsed.mods_filter, ctx.author_id());

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("[COMPARE] Fetched {} scores for user {} on beatmap {} in {}ms",
        state.scores.size(), osu_user_id, beatmap_id, elapsed);

    // Build and send embed
    dpp::message msg = s->message_presenter.build_compare_page(state);

    // Send and cache state for pagination (only if more than 1 page)
    if (state.total_pages > 1) {
        ctx.reply(std::move(msg), false, [state](const dpp::confirmation_callback_t& callback) {
            if (callback.is_error()) {
                return;
            }

            // Get message ID from callback and cache state
            auto msg_result = callback.get<dpp::message>();
            try {
                auto& cache_inst = cache::MemcachedCache::instance();
                cache_inst.cache_compare(msg_result.id.str(), state);
                spdlog::info("[COMPARE] Cached compare state for message {}", msg_result.id.str());
            } catch (const std::exception& e) {
                spdlog::warn("[COMPARE] Failed to cache compare state: {}", e.what());
            }
        });
    } else {
        ctx.reply(msg);
    }
}

} // namespace commands
