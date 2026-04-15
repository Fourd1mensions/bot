#include "handlers/button_handler.h"
#include "services/leaderboard_service.h"
#include "services/recent_score_service.h"
#include "services/pagination_service.h"
#include "services/message_presenter_service.h"
#include "services/beatmap_performance_service.h"
#include "services/chat_context_service.h"
#include "services/user_settings_service.h"
#include "services/embed_template_service.h"
#include <requests.h>
#include <cache.h>
#include <osu.h>
#include <utils.h>
#include <database.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <algorithm>

using json = nlohmann::json;

namespace handlers {

ButtonHandler::ButtonHandler(
    services::LeaderboardService& leaderboard_service,
    services::RecentScoreService& recent_score_service,
    services::MessagePresenterService& message_presenter,
    Request& request,
    services::BeatmapPerformanceService* performance_service,
    services::ChatContextService* chat_context_service,
    services::UserSettingsService* user_settings_service,
    services::EmbedTemplateService* template_service
)
    : leaderboard_service_(leaderboard_service)
    , recent_score_service_(recent_score_service)
    , message_presenter_(message_presenter)
    , request_(request)
    , performance_service_(performance_service)
    , chat_context_service_(chat_context_service)
    , user_settings_service_(user_settings_service)
    , template_service_(template_service)
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

    // Handle compare pagination
    if (button_id == "cmp_prev" || button_id == "cmp_next" || button_id == "cmp_first" || button_id == "cmp_last") {
        handle_cmp_pagination(event, button_id);
        return;
    }

    // Handle users pagination
    if (button_id == "users_prev" || button_id == "users_next") {
        handle_users_pagination(event, button_id);
        return;
    }

    // Handle top pagination
    if (button_id == "top_prev" || button_id == "top_next" || button_id == "top_first" || button_id == "top_last") {
        handle_top_pagination(event, button_id);
        return;
    }

    // Handle osu! link DM button
    if (button_id.starts_with("osu_link_dm:")) {
        handle_osu_link_dm(event, button_id.substr(12));
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
        event.reply();
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
        event.reply();
        return;
    }

    auto state = *state_opt;

    // Navigate using PaginationService
    if (!services::PaginationService::navigate_by_button(state, button_id)) {
        event.reply();
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
        event.reply();
        return;
    }

    auto state = *state_opt;

    // Navigate using PaginationService
    if (!services::PaginationService::navigate_by_button(state, button_id)) {
        event.reply();
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
        event.reply();
        return;
    }

    auto state = *state_opt;
    state.refresh_count++;

    std::string scores_response;
    std::string mode = state.mode.empty() ? "osu" : state.mode;
    if (state.use_best_scores) {
        scores_response = request_.get_user_best_scores(std::to_string(state.osu_user_id), mode, 100, 0);
    } else {
        scores_response = request_.get_user_recent_scores(
            std::to_string(state.osu_user_id), state.include_fails, mode, 50, 0);
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
                    // Sort best scores by date (newest first), matching initial fetch order
                    if (state.use_best_scores) {
                        std::sort(new_scores.begin(), new_scores.end(), [](const Score& a, const Score& b) {
                            return a.get_created_at() > b.get_created_at();
                        });
                    }
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

        // Use PaginationService for jump navigation
        if (!services::PaginationService::navigate(state, services::NavigationAction::JumpTo, page_num)) {
            event.reply(dpp::ir_channel_message_with_source,
                dpp::message(fmt::format("Invalid page number. Please enter a number between 1 and {}.", state.total_pages))
                    .set_flags(dpp::m_ephemeral));
            return;
        }

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

void ButtonHandler::handle_cmp_pagination(const dpp::button_click_t& event, const std::string& button_id) {
    auto msg_id = event.command.message_id;

    // Fetch from Memcached
    std::optional<CompareState> state_opt;
    try {
        auto& cache = cache::MemcachedCache::instance();
        state_opt = cache.get_compare(msg_id.str());
        spdlog::info("[BTN] Retrieved compare state from cache: {}", state_opt.has_value() ? "success" : "not found");
    } catch (const std::exception& e) {
        spdlog::warn("Failed to fetch compare from cache: {}", e.what());
    }

    if (!state_opt) {
        spdlog::info("[BTN] Ignoring button click for expired/missing compare {}", msg_id.str());
        event.reply();
        return;
    }

    auto state = *state_opt;

    // Navigate using PaginationService
    if (!services::PaginationService::navigate_by_button(state, button_id)) {
        event.reply();
        return;
    }

    // Save updated state back to Memcached
    try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_compare(msg_id.str(), state);
        spdlog::info("[BTN] Saved updated compare state to cache, page={}/{}", state.current_page + 1, state.total_pages);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to save updated compare to cache: {}", e.what());
    }

    // Build updated message with new page
    dpp::message updated_msg = message_presenter_.build_compare_page(state);

    // Update the message
    spdlog::info("[BTN] Updating compare message with new page {}", state.current_page + 1);
    event.reply(dpp::ir_update_message, updated_msg);
}

namespace {
// Helper to build users page embed (must match users_command.cpp)
dpp::message build_users_page(const UsersState& state) {
    dpp::embed embed;
    embed.set_color(0xff66ab);  // osu! pink
    embed.set_title(fmt::format("Tracked Users ({})", state.users.size()));

    std::string description;
    size_t start_idx = state.current_page * UsersState::USERS_PER_PAGE;
    size_t end_idx = std::min(start_idx + UsersState::USERS_PER_PAGE, state.users.size());

    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& user = state.users[i];
        description += fmt::format("[{}](https://osu.ppy.sh/users/{}) • <@{}>\n",
            user.osu_username, user.osu_user_id, user.discord_id.str());
    }

    embed.set_description(description);

    // Footer with page info
    if (state.total_pages > 1) {
        embed.set_footer(dpp::embed_footer()
            .set_text(fmt::format("Page {}/{}", state.current_page + 1, state.total_pages)));
    }

    dpp::message msg;
    msg.add_embed(embed);

    // Add pagination buttons if needed
    if (state.total_pages > 1) {
        dpp::component row;
        row.set_type(dpp::cot_action_row);

        dpp::component prev_btn;
        prev_btn.set_type(dpp::cot_button)
            .set_style(dpp::cos_secondary)
            .set_label("◀")
            .set_id("users_prev")
            .set_disabled(state.current_page == 0);

        dpp::component next_btn;
        next_btn.set_type(dpp::cot_button)
            .set_style(dpp::cos_secondary)
            .set_label("▶")
            .set_id("users_next")
            .set_disabled(state.current_page >= state.total_pages - 1);

        row.add_component(prev_btn);
        row.add_component(next_btn);
        msg.add_component(row);
    }

    return msg;
}
} // anonymous namespace

void ButtonHandler::handle_users_pagination(const dpp::button_click_t& event, const std::string& button_id) {
    auto msg_id = event.command.message_id;
    spdlog::info("[BTN] users pagination: msg_id={}, button={}", msg_id.str(), button_id);

    // Fetch from Memcached
    std::optional<UsersState> state_opt;
    try {
        auto& cache = cache::MemcachedCache::instance();
        state_opt = cache.get_users(msg_id.str());
        spdlog::info("[BTN] users cache lookup: found={}", state_opt.has_value());
    } catch (const std::exception& e) {
        spdlog::warn("[BTN] Failed to fetch users from cache: {}", e.what());
    }

    if (!state_opt) {
        spdlog::warn("[BTN] No users state found for message {}", msg_id.str());
        event.reply();
        return;
    }

    auto state = *state_opt;

    // Navigate
    if (button_id == "users_prev" && state.current_page > 0) {
        state.current_page--;
    } else if (button_id == "users_next" && state.current_page < state.total_pages - 1) {
        state.current_page++;
    } else {
        return;
    }

    // Save updated state back to Memcached
    try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_users(msg_id.str(), state);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to save users to cache: {}", e.what());
    }

    // Build and update message
    dpp::message updated_msg = build_users_page(state);
    event.reply(dpp::ir_update_message, updated_msg);
}

void ButtonHandler::handle_osu_link_dm(const dpp::button_click_t& event, const std::string& token) {
    // Get link URL from cached token
    std::string link_url;
    try {
        auto& mc = cache::MemcachedCache::instance();
        auto token_data = mc.get("osu_link_token:" + token);
        if (!token_data) {
            event.reply(dpp::message("Link expired. Please use `/set` again.").set_flags(dpp::m_ephemeral));
            return;
        }

        auto j = json::parse(*token_data);
        link_url = j.value("link_url", "");
        if (link_url.empty()) {
            event.reply(dpp::message("Invalid link token.").set_flags(dpp::m_ephemeral));
            return;
        }
    } catch (const std::exception& e) {
        spdlog::error("[BTN] Failed to get link token: {}", e.what());
        event.reply(dpp::message("Failed to get link. Please try again.").set_flags(dpp::m_ephemeral));
        return;
    }

    // Send DM to user
    dpp::snowflake user_id = event.command.usr.id;

    auto embed = dpp::embed()
        .set_color(0xff66aa)
        .set_title("Link your osu! Account")
        .set_description(fmt::format("Click the link below to connect your osu! account:\n\n**[Click here to link]({})**\n\nThis link expires in 5 minutes.", link_url))
        .set_footer(dpp::embed_footer().set_text("Patchouli Bot"));

    dpp::message dm_msg;
    dm_msg.add_embed(embed);

    event.from->creator->direct_message_create(user_id, dm_msg, [&event](const dpp::confirmation_callback_t& callback) {
        if (callback.is_error()) {
            spdlog::warn("[BTN] Failed to send DM: {}", callback.get_error().message);
        }
    });

    event.reply(dpp::message("Check your DMs for the link!").set_flags(dpp::m_ephemeral));
}

void ButtonHandler::handle_top_pagination(const dpp::button_click_t& event, const std::string& button_id) {
    auto msg_id = event.command.message_id;

    // Fetch from Memcached
    std::optional<TopState> state_opt;
    try {
        auto& cache = cache::MemcachedCache::instance();
        state_opt = cache.get_top(msg_id.str());
        spdlog::info("[BTN] Retrieved top state from cache: {}", state_opt.has_value() ? "success" : "not found");
    } catch (const std::exception& e) {
        spdlog::warn("Failed to fetch top from cache: {}", e.what());
    }

    if (!state_opt) {
        spdlog::info("[BTN] Ignoring button click for expired/missing top {}", msg_id.str());
        event.reply();
        return;
    }

    auto state = *state_opt;

    // Navigate using PaginationService
    if (!services::PaginationService::navigate_by_button(state, button_id)) {
        event.reply();
        return;
    }

    // Save updated state back to Memcached
    try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_top(msg_id.str(), state);
        spdlog::info("[BTN] Saved updated top state to cache, page={}/{}", state.current_page + 1, state.total_pages);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to save updated top to cache: {}", e.what());
    }

    // Build updated message with new page
    dpp::message updated_msg = message_presenter_.build_top_page(state);

    // Update the message
    spdlog::info("[BTN] Updating message with new page {}", state.current_page + 1);
    event.reply(dpp::ir_update_message, updated_msg);
}

void ButtonHandler::handle_select_click(const dpp::select_click_t& event) {
    const std::string& menu_id = event.custom_id;

    spdlog::info("[SELECT] user={} ({}) channel={} menu={} values={}",
        event.command.usr.id.str(), event.command.usr.username,
        event.command.channel_id.str(), menu_id,
        event.values.empty() ? "(none)" : event.values[0]);

    if (menu_id.starts_with("map_search_set:")) {
        handle_map_search_set(event, menu_id.substr(15));
        return;
    }

    if (menu_id.starts_with("map_search_diff:")) {
        handle_map_search_diff(event);
        return;
    }
}

void ButtonHandler::show_beatmap_result(const dpp::select_click_t& event, uint32_t beatmap_id) {
    std::string beatmap_id_str = std::to_string(beatmap_id);

    if (!performance_service_) {
        event.reply(dpp::ir_update_message,
            dpp::message().set_content("Performance service unavailable."));
        return;
    }

    // Acknowledge with deferred update
    event.reply(dpp::ir_deferred_update_message, dpp::message());

    // Get beatmap info from API
    std::string beatmap_json = request_.get_beatmap(beatmap_id_str);
    if (beatmap_json.empty()) {
        event.edit_original_response(dpp::message().set_content("Failed to fetch beatmap information."));
        return;
    }

    auto beatmap_data = json::parse(beatmap_json);
    Beatmap beatmap(beatmap_data);
    uint32_t beatmapset_id = beatmap.get_beatmapset_id();
    std::string beatmap_mode = beatmap.get_mode();

    // Cache beatmap mapping
    try {
        auto& db = db::Database::instance();
        db.cache_beatmap_id(beatmap_id, beatmapset_id, beatmap_mode);
    } catch (const std::exception& e) {
        spdlog::debug("[MAP-SEARCH] Failed to cache beatmap mapping: {}", e.what());
    }

    // Calculate PP at multiple accuracy levels
    std::vector<double> acc_levels = {0.90, 0.95, 0.99, 1.00};
    services::BeatmapDifficultyAttrs perf_difficulty;

    auto pp_values = performance_service_->calculate_pp_at_accuracies(
        beatmap_id, beatmap_mode, "", acc_levels, &perf_difficulty
    );

    if (pp_values.empty()) {
        event.edit_original_response(dpp::message().set_content("Failed to calculate PP values."));
        return;
    }

    services::DifficultyInfo difficulty_info{
        .approach_rate = perf_difficulty.approach_rate,
        .overall_difficulty = perf_difficulty.overall_difficulty,
        .circle_size = perf_difficulty.circle_size,
        .hp_drain_rate = perf_difficulty.hp_drain_rate,
        .star_rating = perf_difficulty.star_rating,
        .aim_difficulty = perf_difficulty.aim_difficulty,
        .speed_difficulty = perf_difficulty.speed_difficulty,
        .max_combo = perf_difficulty.max_combo
    };

    float modded_bpm = beatmap.get_bpm();
    uint32_t modded_length = beatmap.get_total_length();

    auto user_preset = services::EmbedPreset::Classic;
    if (user_settings_service_) {
        user_preset = user_settings_service_->get_preset(event.command.usr.id);
    }

    auto strain_graph = performance_service_->get_strain_graph(beatmap_id, "", 900, 250);

    dpp::message msg = message_presenter_.build_map_info(
        beatmap, difficulty_info, pp_values, "",
        beatmapset_id, modded_bpm, modded_length,
        user_preset, event.command.usr.id
    );

    if (strain_graph && !strain_graph->empty()) {
        std::string filename = fmt::format("strains_{}.png", beatmap_id);
        msg.add_file(filename, std::string(strain_graph->begin(), strain_graph->end()));
        if (!msg.embeds.empty()) {
            msg.embeds[0].set_image("attachment://" + filename);
        }
    }

    event.edit_original_response(msg);
}

void ButtonHandler::handle_map_search_set(const dpp::select_click_t& event, const std::string& search_key) {
    if (event.values.empty()) return;

    std::string beatmapset_id_str = event.values[0];

    auto& cache = cache::MemcachedCache::instance();
    auto cached = cache.get("map_search:" + search_key);
    if (!cached) {
        event.reply(dpp::ir_update_message,
            dpp::message().set_content("Search results expired. Please search again."));
        return;
    }

    try {
        auto search_data = json::parse(*cached);

        // Find selected beatmapset
        json selected_set;
        for (const auto& set : search_data) {
            if (std::to_string(set.value("id", 0)) == beatmapset_id_str) {
                selected_set = set;
                break;
            }
        }

        if (selected_set.empty()) {
            event.reply(dpp::ir_update_message,
                dpp::message().set_content("Beatmapset not found."));
            return;
        }

        std::string artist = selected_set.value("artist", "");
        std::string title = selected_set.value("title", "");
        std::string creator = selected_set.value("creator", "");

        auto beatmaps = selected_set.value("beatmaps", json::array());

        // Sort by star rating
        std::vector<json> sorted_beatmaps(beatmaps.begin(), beatmaps.end());
        std::sort(sorted_beatmaps.begin(), sorted_beatmaps.end(), [](const json& a, const json& b) {
            return a.value("difficulty_rating", 0.0) < b.value("difficulty_rating", 0.0);
        });

        if (sorted_beatmaps.size() > 25) {
            sorted_beatmaps.resize(25);
        }

        // Single difficulty — skip second select, show map directly
        if (sorted_beatmaps.size() == 1) {
            uint32_t bm_id = sorted_beatmaps[0].value("id", 0);
            show_beatmap_result(event, bm_id);
            return;
        }

        // Build select menu
        dpp::component select_menu;
        select_menu.set_type(dpp::cot_selectmenu);
        select_menu.set_placeholder("Select difficulty...");
        select_menu.set_id("map_search_diff:" + beatmapset_id_str);

        for (const auto& bm : sorted_beatmaps) {
            uint32_t bm_id = bm.value("id", 0);
            std::string diff_name = bm.value("version", "Unknown");
            double sr = bm.value("difficulty_rating", 0.0);
            std::string mode = bm.value("mode", "osu");

            std::string label = fmt::format("[{:.2f}*] {}", sr, diff_name);
            if (label.size() > 100) label = label.substr(0, 97) + "...";

            std::string desc = fmt::format("Mode: {} | ID: {}", mode, bm_id);

            select_menu.add_select_option(
                dpp::select_option(label, std::to_string(bm_id), desc)
            );
        }

        dpp::message msg;
        msg.set_content(fmt::format("**{} - {}** by **{}**", artist, title, creator));
        msg.add_component(dpp::component().add_component(select_menu));

        event.reply(dpp::ir_update_message, msg);

    } catch (const json::exception& e) {
        spdlog::error("[SELECT] Failed to parse search cache: {}", e.what());
        event.reply(dpp::ir_update_message,
            dpp::message().set_content("Internal error processing search results."));
    }
}

void ButtonHandler::handle_map_search_diff(const dpp::select_click_t& event) {
    if (event.values.empty()) return;

    uint32_t beatmap_id = 0;
    try {
        beatmap_id = std::stoul(event.values[0]);
    } catch (...) {
        event.reply(dpp::ir_update_message,
            dpp::message().set_content("Invalid beatmap ID."));
        return;
    }

    show_beatmap_result(event, beatmap_id);
}

} // namespace handlers
