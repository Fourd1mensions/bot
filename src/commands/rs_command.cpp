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

RsCommand::ParsedParams RsCommand::parse(const CommandContext& ctx) const {
    ParsedParams result;
    std::string content = ctx.content;

    size_t cmd_end = 3; // Length of "!rs"
    if (content.find("!кы") == 0) {
        cmd_end = 7; // Length of "!кы" in bytes (UTF-8: 4 bytes for each Cyrillic char + 1 for !)
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

void RsCommand::execute(const CommandContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[!rs] ServiceContainer is null");
        return;
    }

    auto parsed = parse(ctx);
    const auto& event = ctx.event;

    if (!parsed.valid) {
        event.reply(parsed.error_message);
        return;
    }

    auto start = std::chrono::steady_clock::now();

    // Parse parameters using service
    auto cmd_params = s->command_params_service.parse_recent_params(parsed.params, parsed.mode);

    // Resolve osu user_id using service
    auto resolve_result = s->user_resolver_service.resolve(cmd_params.username, event.msg.author.id);
    if (!resolve_result) {
        event.reply(s->message_presenter.build_error_message(resolve_result.error_message));
        return;
    }
    int64_t osu_user_id = resolve_result.osu_user_id;

    // Show typing indicator
    s->bot.channel_typing(event.msg.channel_id);

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
            event.reply(dpp::message(
                fmt::format("request timeout: osu! api took too long to respond ({}s)", elapsed)));
        } else {
            event.reply(s->message_presenter.build_error_message(error_messages::FETCH_SCORES_FAILED));
        }
        return;
    }

    // Parse scores
    std::vector<Score> scores;
    try {
        json scores_json = json::parse(scores_response);
        if (!scores_json.is_array() || scores_json.empty()) {
            event.reply(s->message_presenter.build_error_message(error_messages::NO_RECENT_SCORES));
            return;
        }

        for (const auto& score_json : scores_json) {
            Score score;
            score.from_json(score_json);
            scores.push_back(score);
        }
    } catch (const json::exception& e) {
        event.reply(s->message_presenter.build_error_message(error_messages::PARSE_SCORES_FAILED));
        spdlog::error("Failed to parse scores: {}", e.what());
        return;
    }

    // Validate score index
    if (cmd_params.score_index >= scores.size()) {
        event.reply(s->message_presenter.build_error_message(
            fmt::format(error_messages::SCORE_INDEX_OUT_OF_RANGE_FORMAT, cmd_params.score_index + 1, scores.size())));
        return;
    }

    // Create state
    RecentScoreState rs_state(std::move(scores), cmd_params.score_index, cmd_params.mode,
        cmd_params.include_fails, cmd_params.use_best_scores, osu_user_id, event.msg.author.id);

    // Build first page (will parse .osu file for first score only and cache it)
    dpp::message msg = s->recent_score_service.build_page(rs_state);

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    if (elapsed > 8) {
        spdlog::warn("[CMD] !rs took {}s to complete (slow API response)", elapsed);
    }

    // Reply with message
    event.reply(msg, false, [s, rs_state = std::move(rs_state)](const dpp::confirmation_callback_t& callback) mutable {
        if (callback.is_error()) {
            spdlog::error("Failed to send recent score message");
            return;
        }
        auto reply_msg = callback.get<dpp::message>();

        // Store state in Memcached with 5-minute TTL
        bool cache_success = false;
        try {
            auto& cache = cache::MemcachedCache::instance();
            cache.cache_recent_scores(reply_msg.id.str(), rs_state);
            spdlog::debug("Stored recent score state for message {} in Memcached", reply_msg.id.str());
            cache_success = true;
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
