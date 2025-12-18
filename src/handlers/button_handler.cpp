#include "handlers/button_handler.h"
#include "services/leaderboard_service.h"
#include "services/recent_score_service.h"
#include <requests.h>
#include <cache.h>
#include <osu.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace handlers {

ButtonHandler::ButtonHandler(
    services::LeaderboardService& leaderboard_service,
    services::RecentScoreService& recent_score_service,
    Request& request
)
    : leaderboard_service_(leaderboard_service)
    , recent_score_service_(recent_score_service)
    , request_(request)
{}

void ButtonHandler::handle_button_click(const dpp::button_click_t& event) {
    const std::string& button_id = event.custom_id;

    spdlog::info("[BTN] user={} ({}) channel={} button={}",
        event.command.usr.id.str(), event.command.usr.username,
        event.command.channel_id.str(), button_id);

    // Handle page jump modal (center button)
    if (button_id == "lb_select") {
        handle_lb_select(event);
        return;
    }

    // Handle leaderboard pagination
    if (button_id == "lb_prev" || button_id == "lb_next" || button_id == "lb_first" || button_id == "lb_last") {
        handle_lb_pagination(event, button_id);
        return;
    }

    // Handle recent scores pagination
    if (button_id == "rs_prev" || button_id == "rs_next" || button_id == "rs_first" || button_id == "rs_last") {
        handle_rs_pagination(event, button_id);
        return;
    }

    // Handle refresh button
    if (button_id == "rs_refresh") {
        handle_rs_refresh(event);
        return;
    }
}

void ButtonHandler::handle_form_submit(const dpp::form_submit_t& event) {
    spdlog::info("[FORM] user={} ({}) channel={} form_id={}",
        event.command.usr.id.str(), event.command.usr.username,
        event.command.channel_id.str(), event.custom_id);

    if (event.custom_id == "lb_jump_modal") {
        handle_lb_jump_modal(event);
    }
}

void ButtonHandler::handle_lb_select(const dpp::button_click_t& event) {
    auto msg_id = event.command.message_id;

    // Fetch from Memcached
    std::optional<LeaderboardState> state_opt;
    try {
        auto& cache = cache::MemcachedCache::instance();
        state_opt = cache.get_leaderboard(msg_id.str());
    } catch (const std::exception& e) {
        spdlog::warn("Failed to fetch leaderboard from cache: {}", e.what());
    }

    if (!state_opt) {
        spdlog::debug("Ignoring button click for expired leaderboard {}", msg_id.str());
        return;
    }

    size_t total_pages = state_opt->total_pages;

    // Create modal for page jump
    dpp::interaction_modal_response modal("lb_jump_modal", "Jump to Page");

    dpp::component text_input;
    text_input.set_label("Page Number")
        .set_id("page_number")
        .set_text_style(dpp::text_short)
        .set_placeholder(fmt::format("Enter 1-{}", total_pages))
        .set_min_length(1)
        .set_max_length(3)
        .set_required(true);

    modal.add_component(text_input);

    event.dialog(modal);
}

void ButtonHandler::handle_lb_pagination(const dpp::button_click_t& event, const std::string& button_id) {
    auto msg_id = event.command.message_id;

    // Fetch from Memcached
    std::optional<LeaderboardState> state_opt;
    try {
        auto& cache = cache::MemcachedCache::instance();
        state_opt = cache.get_leaderboard(msg_id.str());
        spdlog::info("[BTN] Retrieved leaderboard state from cache: {}", state_opt.has_value() ? "success" : "not found");
    } catch (const std::exception& e) {
        spdlog::warn("Failed to fetch leaderboard from cache: {}", e.what());
    }

    if (!state_opt) {
        spdlog::info("[BTN] Ignoring button click for expired/missing leaderboard {}", msg_id.str());
        return;
    }

    auto state = *state_opt;

    // Update page number
    if (button_id == "lb_first") {
        state.current_page = 0;
    } else if (button_id == "lb_prev" && state.current_page > 0) {
        state.current_page--;
    } else if (button_id == "lb_next" && state.current_page < state.total_pages - 1) {
        state.current_page++;
    } else if (button_id == "lb_last") {
        state.current_page = state.total_pages - 1;
    } else {
        return;
    }

    // Save updated state back to Memcached
    try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_leaderboard(msg_id.str(), state);
        spdlog::info("[BTN] Saved updated state to cache, page={}/{}", state.current_page + 1, state.total_pages);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to save updated leaderboard to cache: {}", e.what());
    }

    // Build updated message with new page
    dpp::message updated_msg = leaderboard_service_.build_page(state);

    // Update the message
    spdlog::info("[BTN] Updating message with new page {}", state.current_page + 1);
    event.reply(dpp::ir_update_message, updated_msg);
}

void ButtonHandler::handle_rs_pagination(const dpp::button_click_t& event, const std::string& button_id) {
    auto msg_id = event.command.message_id;

    // Fetch from Memcached
    std::optional<RecentScoreState> state_opt;
    try {
        auto& cache = cache::MemcachedCache::instance();
        state_opt = cache.get_recent_scores(msg_id.str());
        spdlog::info("[BTN] Retrieved recent scores state from cache: {}", state_opt.has_value() ? "success" : "not found");
    } catch (const std::exception& e) {
        spdlog::warn("Failed to fetch recent scores from cache: {}", e.what());
    }

    if (!state_opt) {
        spdlog::info("[BTN] Ignoring button click for expired/missing recent scores {}", msg_id.str());
        return;
    }

    auto state = *state_opt;

    // Update index for navigation buttons
    if (button_id == "rs_first") {
        state.current_index = 0;
    } else if (button_id == "rs_prev" && state.current_index > 0) {
        state.current_index--;
    } else if (button_id == "rs_next" && state.current_index < state.scores.size() - 1) {
        state.current_index++;
    } else if (button_id == "rs_last") {
        state.current_index = state.scores.size() - 1;
    } else {
        return;
    }

    // Save updated state back to Memcached
    try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_recent_scores(msg_id.str(), state);
        spdlog::info("[BTN] Saved updated recent scores state to cache, index={}/{}", state.current_index + 1, state.scores.size());
    } catch (const std::exception& e) {
        spdlog::warn("Failed to save updated recent scores to cache: {}", e.what());
    }

    // Build updated message with new score
    dpp::message updated_msg = recent_score_service_.build_page(state);

    // Update the message
    spdlog::info("[BTN] Updating message with new score {}/{}", state.current_index + 1, state.scores.size());
    event.reply(dpp::ir_update_message, updated_msg);
}

void ButtonHandler::handle_rs_refresh(const dpp::button_click_t& event) {
    auto msg_id = event.command.message_id;

    // Fetch from Memcached
    std::optional<RecentScoreState> state_opt;
    try {
        auto& cache = cache::MemcachedCache::instance();
        state_opt = cache.get_recent_scores(msg_id.str());
        spdlog::info("[BTN] Retrieved recent scores state from cache: {}", state_opt.has_value() ? "success" : "not found");
    } catch (const std::exception& e) {
        spdlog::warn("Failed to fetch recent scores from cache: {}", e.what());
    }

    if (!state_opt) {
        spdlog::info("[BTN] Ignoring button click for expired/missing recent scores {}", msg_id.str());
        return;
    }

    auto state = *state_opt;
    state.refresh_count++;

    std::string scores_response;
    if (state.use_best_scores) {
        scores_response = request_.get_user_best_scores(std::to_string(state.osu_user_id), "osu", 100, 0);
    } else {
        scores_response = request_.get_user_recent_scores(
            std::to_string(state.osu_user_id), state.include_fails, "osu", 50, 0);
    }

    if (!scores_response.empty()) {
        try {
            json scores_json = json::parse(scores_response);
            std::vector<Score> new_scores;

            // Parse all scores (beatmap will be fetched when displaying)
            for (const auto& score_j : scores_json) {
                Score score;
                score.from_json(score_j);
                new_scores.push_back(std::move(score));
            }

            if (!new_scores.empty()) {
                // Check if there are new scores by comparing first score
                bool has_new_scores = false;
                if (state.scores.empty() ||
                    new_scores[0].get_created_at() != state.scores[0].get_created_at() ||
                    new_scores[0].get_total_score() != state.scores[0].get_total_score()) {
                    has_new_scores = true;
                }

                if (has_new_scores) {
                    state.scores = std::move(new_scores);
                    state.current_index = 0;
                    spdlog::info("[BTN] Refreshed scores, got {} new scores", state.scores.size());
                } else {
                    spdlog::info("[BTN] Refreshed scores, no new scores found (refresh count: {})", state.refresh_count);
                }
            }
        } catch (const json::exception& e) {
            spdlog::error("Failed to parse refreshed scores: {}", e.what());
        }
    }

    // Save updated state back to Memcached
    try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_recent_scores(msg_id.str(), state);
        spdlog::info("[BTN] Saved updated recent scores state to cache, index={}/{}", state.current_index + 1, state.scores.size());
    } catch (const std::exception& e) {
        spdlog::warn("Failed to save updated recent scores to cache: {}", e.what());
    }

    // Build updated message with new score
    dpp::message updated_msg = recent_score_service_.build_page(state);

    // Update the message
    spdlog::info("[BTN] Updating message with new score {}/{}", state.current_index + 1, state.scores.size());
    event.reply(dpp::ir_update_message, updated_msg);
}

void ButtonHandler::handle_lb_jump_modal(const dpp::form_submit_t& event) {
    auto msg_id = event.command.message_id;

    // Get the page number from the form
    std::string page_str = std::get<std::string>(event.components[0].components[0].value);

    try {
        int page_num = std::stoi(page_str);

        // Fetch from Memcached
        std::optional<LeaderboardState> state_opt;
        try {
            auto& cache = cache::MemcachedCache::instance();
            state_opt = cache.get_leaderboard(msg_id.str());
        } catch (const std::exception& e) {
            spdlog::warn("Failed to fetch leaderboard from cache: {}", e.what());
        }

        if (!state_opt) {
            spdlog::debug("Ignoring form submit for expired leaderboard {}", msg_id.str());
            return;
        }

        auto state = *state_opt;

        // Validate page number
        if (page_num < 1 || page_num > static_cast<int>(state.total_pages)) {
            event.reply(dpp::ir_channel_message_with_source,
                dpp::message(fmt::format("Invalid page number. Please enter a number between 1 and {}.", state.total_pages))
                    .set_flags(dpp::m_ephemeral));
            return;
        }

        // Update to the requested page (convert to 0-indexed)
        state.current_page = page_num - 1;

        // Save updated state back to Memcached
        try {
            auto& cache = cache::MemcachedCache::instance();
            cache.cache_leaderboard(msg_id.str(), state);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to save updated leaderboard to cache: {}", e.what());
        }

        // Build updated message with new page
        dpp::message updated_msg = leaderboard_service_.build_page(state);

        // Update the message
        event.reply(dpp::ir_update_message, updated_msg);

    } catch (const std::exception& e) {
        event.reply(dpp::ir_channel_message_with_source,
            dpp::message("Invalid input. Please enter a valid number.").set_flags(dpp::m_ephemeral));
    }
}

} // namespace handlers
