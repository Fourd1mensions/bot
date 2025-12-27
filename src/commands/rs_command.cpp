#include "commands/rs_command.h"
#include "services/service_container.h"
#include "services/user_resolver_service.h"
#include "services/message_presenter_service.h"
#include "services/command_params_service.h"
#include "services/recent_score_service.h"
#include <requests.h>
#include <osu.h>
#include <cache.h>
#include <error_messages.h>
#include <state/session_state.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace commands {

std::vector<std::string> RsCommand::get_aliases() const {
    return {"!rs", "!кы"};
}

bool RsCommand::matches(const CommandContext& ctx) const {
    return ctx.content_lower.find("!rs") == 0 || ctx.content.find("!кы") == 0;
}

RsCommand::ParsedParams RsCommand::parse(const std::string& content) const {
    ParsedParams result;

    size_t cmd_end = 0;
    // Find command end - check for known prefixes
    if (content.find("!rs") == 0) {
        cmd_end = 3;
    } else if (content.find("!кы") == 0) {
        cmd_end = 7; // UTF-8 bytes
    } else if (content.find("/rs") == 0) {
        cmd_end = 3;
    } else {
        // For slash commands without prefix, params start from beginning
        result.params = content;
        return result;
    }

    // Check for mode specification (e.g., !rs:taiko)
    size_t colon_pos = content.find(':');
    if (colon_pos != std::string::npos && colon_pos < cmd_end + 10) {
        size_t mode_end = content.find(' ', colon_pos);
        if (mode_end == std::string::npos) {
            mode_end = content.length();
        }
        result.mode = content.substr(colon_pos + 1, mode_end - colon_pos - 1);
        std::transform(result.mode.begin(), result.mode.end(), result.mode.begin(), ::tolower);

        // Validate mode
        if (result.mode != "osu" && result.mode != "taiko" && result.mode != "catch" &&
            result.mode != "mania" && result.mode != "fruits" && result.mode != "ctb") {
            result.valid = false;
            result.error_message = "Invalid mode. Supported modes: `osu`, `taiko`, `catch`/`fruits`, `mania`";
            return result;
        }

        // Normalize mode names
        if (result.mode == "ctb") result.mode = "catch";
        if (result.mode == "fruits") result.mode = "catch";

        cmd_end = mode_end;
    }

    // Extract params after command
    result.params = content.length() > cmd_end ? content.substr(cmd_end) : "";

    // Trim leading spaces
    size_t start = result.params.find_first_not_of(" \t");
    if (start != std::string::npos) {
        result.params = result.params.substr(start);
    } else {
        result.params = "";
    }

    return result;
}

void RsCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[rs] ServiceContainer is null");
        return;
    }

    services::RecentScoreParams cmd_params;

    if (ctx.is_slash()) {
        // Get parameters directly from slash command
        if (auto username = ctx.get_string_param("username")) {
            cmd_params.username = *username;
        }
        if (auto index = ctx.get_int_param("index")) {
            cmd_params.score_index = static_cast<size_t>(*index - 1); // Convert to 0-based
        }
        if (auto mode = ctx.get_string_param("mode")) {
            cmd_params.mode = *mode;
        }
        if (auto best = ctx.get_bool_param("best"); best && *best) {
            cmd_params.use_best_scores = true;
        }
        if (auto pass = ctx.get_bool_param("pass"); pass && *pass) {
            cmd_params.include_fails = false;
        }
    } else {
        // Parse text command
        auto parsed = parse(ctx.content);
        if (!parsed.valid) {
            ctx.reply(parsed.error_message);
            return;
        }
        cmd_params = s->command_params_service.parse_recent_params(parsed.params, parsed.mode);
    }

    auto start = std::chrono::steady_clock::now();

    // Resolve osu user_id using service
    auto resolve_result = s->user_resolver_service.resolve(cmd_params.username, ctx.author_id());
    if (!resolve_result) {
        ctx.reply(s->message_presenter.build_error_message(resolve_result.error_message));
        return;
    }
    int64_t osu_user_id = resolve_result.osu_user_id;

    // Show typing indicator (only for text commands)
    if (!ctx.is_slash()) {
        s->bot.channel_typing(ctx.channel_id());
    }

    // Fetch scores (recent or best)
    std::string scores_response;
    if (cmd_params.use_best_scores) {
        scores_response = s->request.get_user_best_scores(std::to_string(osu_user_id), cmd_params.mode, 100, 0);
    } else {
        scores_response = s->request.get_user_recent_scores(
            std::to_string(osu_user_id), cmd_params.include_fails, cmd_params.mode, 50, 0);
    }

    if (scores_response.empty()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        if (elapsed > 8) {
            ctx.reply(fmt::format("request timeout: osu! api took too long to respond ({}s)", elapsed));
        } else {
            ctx.reply(s->message_presenter.build_error_message(error_messages::FETCH_SCORES_FAILED));
        }
        return;
    }

    // Parse scores
    std::vector<Score> scores;
    try {
        json scores_json = json::parse(scores_response);
        if (!scores_json.is_array() || scores_json.empty()) {
            ctx.reply(s->message_presenter.build_error_message(error_messages::NO_RECENT_SCORES));
            return;
        }

        for (const auto& score_json : scores_json) {
            Score score;
            score.from_json(score_json);
            scores.push_back(score);
        }
    } catch (const json::exception& e) {
        ctx.reply(s->message_presenter.build_error_message(error_messages::PARSE_SCORES_FAILED));
        spdlog::error("Failed to parse scores: {}", e.what());
        return;
    }

    // Validate score index
    if (cmd_params.score_index >= scores.size()) {
        ctx.reply(s->message_presenter.build_error_message(
            fmt::format(error_messages::SCORE_INDEX_OUT_OF_RANGE_FORMAT, cmd_params.score_index + 1, scores.size())));
        return;
    }

    // Create state
    RecentScoreState rs_state(std::move(scores), cmd_params.score_index, cmd_params.mode,
        cmd_params.include_fails, cmd_params.use_best_scores, osu_user_id, ctx.author_id());

    // Build first page (will parse .osu file for first score only and cache it)
    dpp::message msg = s->recent_score_service.build_page(rs_state);

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    if (elapsed > 8) {
        spdlog::warn("[CMD] rs took {}s to complete (slow API response)", elapsed);
    }

    // Reply with message
    ctx.reply(std::move(msg), false, [s, rs_state = std::move(rs_state)](const dpp::confirmation_callback_t& callback) mutable {
        if (callback.is_error()) {
            spdlog::error("Failed to send recent score message");
            return;
        }
        auto reply_msg = callback.get<dpp::message>();

        // Store state in Memcached with 5-minute TTL
        try {
            auto& cache = cache::MemcachedCache::instance();
            cache.cache_recent_scores(reply_msg.id.str(), rs_state);
            spdlog::debug("Stored recent score state for message {} in Memcached", reply_msg.id.str());
        } catch (const std::exception& e) {
            spdlog::error("Failed to cache recent score state: {}", e.what());
        }

        // Schedule button removal only if there's more than one score
        if (rs_state.scores.size() > 1) {
            dpp::snowflake msg_id = reply_msg.id;
            dpp::snowflake chan_id = reply_msg.channel_id;

            // Remove buttons after 2 minutes
            auto ttl = std::chrono::minutes(2);
            s->recent_score_service.schedule_button_removal(chan_id, msg_id, ttl);
        }
    });
}

} // namespace commands
