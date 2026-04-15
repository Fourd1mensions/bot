#pragma once

#include <dpp/dpp.h>

// Forward declarations
class Request;

namespace services {
class LeaderboardService;
class RecentScoreService;
class MessagePresenterService;
class BeatmapPerformanceService;
class ChatContextService;
class UserSettingsService;
class EmbedTemplateService;
}

namespace handlers {

/**
 * Handler for Discord button click, select menu, and form submit events.
 * Routes events to appropriate services.
 */
class ButtonHandler {
public:
    ButtonHandler(
        services::LeaderboardService& leaderboard_service,
        services::RecentScoreService& recent_score_service,
        services::MessagePresenterService& message_presenter,
        Request& request,
        services::BeatmapPerformanceService* performance_service = nullptr,
        services::ChatContextService* chat_context_service = nullptr,
        services::UserSettingsService* user_settings_service = nullptr,
        services::EmbedTemplateService* template_service = nullptr
    );

    /**
     * Handle button click event.
     */
    void handle_button_click(const dpp::button_click_t& event);

    /**
     * Handle select menu click event.
     */
    void handle_select_click(const dpp::select_click_t& event);

    /**
     * Handle form submit event (modal responses).
     */
    void handle_form_submit(const dpp::form_submit_t& event);

private:
    // Leaderboard button handlers
    void handle_lb_select(const dpp::button_click_t& event);
    void handle_lb_pagination(const dpp::button_click_t& event, const std::string& button_id);

    // Recent scores button handlers
    void handle_rs_pagination(const dpp::button_click_t& event, const std::string& button_id);
    void handle_rs_refresh(const dpp::button_click_t& event);

    // Compare button handlers
    void handle_cmp_pagination(const dpp::button_click_t& event, const std::string& button_id);

    // Users button handlers
    void handle_users_pagination(const dpp::button_click_t& event, const std::string& button_id);

    // Top button handlers
    void handle_top_pagination(const dpp::button_click_t& event, const std::string& button_id);

    // osu! link handler
    void handle_osu_link_dm(const dpp::button_click_t& event, const std::string& token);

    // Map search select handlers
    void handle_map_search_set(const dpp::select_click_t& event, const std::string& search_key);
    void handle_map_search_diff(const dpp::select_click_t& event);
    void show_beatmap_result(const dpp::select_click_t& event, uint32_t beatmap_id);

    // Form handlers
    void handle_lb_jump_modal(const dpp::form_submit_t& event);

    services::LeaderboardService& leaderboard_service_;
    services::RecentScoreService& recent_score_service_;
    services::MessagePresenterService& message_presenter_;
    Request& request_;
    services::BeatmapPerformanceService* performance_service_;
    services::ChatContextService* chat_context_service_;
    services::UserSettingsService* user_settings_service_;
    services::EmbedTemplateService* template_service_;
};

} // namespace handlers
