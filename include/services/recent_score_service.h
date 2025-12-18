#pragma once

#include <osu.h>  // For Score, Beatmap - required by session_state.h
#include <state/session_state.h>
#include <dpp/dpp.h>
#include <chrono>
#include <functional>

// Forward declarations
class Request;

namespace services {

class BeatmapPerformanceService;
class MessagePresenterService;

/**
 * Service for building recent score pages and managing score navigation.
 */
class RecentScoreService {
public:
    RecentScoreService(
        Request& request,
        BeatmapPerformanceService& performance_service,
        MessagePresenterService& message_presenter,
        dpp::cluster& bot
    );

    /**
     * Build a message page for the current score in state.
     */
    dpp::message build_page(RecentScoreState& state);

    /**
     * Schedule removal of message buttons after TTL.
     */
    void schedule_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id, std::chrono::minutes ttl);

    /**
     * Remove components from a message.
     */
    void remove_message_components(dpp::snowflake channel_id, dpp::snowflake message_id);

private:
    Request& request_;
    BeatmapPerformanceService& performance_service_;
    MessagePresenterService& message_presenter_;
    dpp::cluster& bot_;
};

} // namespace services
