#pragma once

#include <dpp/dpp.h>

// Forward declarations
class Request;

namespace services {
class LeaderboardService;
class RecentScoreService;
}

namespace handlers {

/**
 * Handler for Discord button click and form submit events.
 * Routes events to appropriate services.
 */
class ButtonHandler {
public:
    ButtonHandler(
        services::LeaderboardService& leaderboard_service,
        services::RecentScoreService& recent_score_service,
        Request& request
    );

    /**
     * Handle button click event.
     */
    void handle_button_click(const dpp::button_click_t& event);

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

    // Form handlers
    void handle_lb_jump_modal(const dpp::form_submit_t& event);

    services::LeaderboardService& leaderboard_service_;
    services::RecentScoreService& recent_score_service_;
    Request& request_;
};

} // namespace handlers
