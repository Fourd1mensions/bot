#pragma once

#include <dpp/dpp.h>
#include <commands/command_router.h>
#include <mutex>

namespace services {
class ChatContextService;
}

namespace handlers {

/**
 * Handles Discord message events (create and update).
 */
class MessageHandler {
public:
    MessageHandler(
        commands::CommandRouter& command_router,
        services::ChatContextService& chat_context_service,
        dpp::cluster& bot
    );

    /**
     * Handle message create event.
     * Logs the message, updates chat context, and routes to command handlers.
     */
    void handle_create(const dpp::message_create_t& event);

    /**
     * Handle message update event.
     * Updates chat context if the edited message is being tracked.
     */
    void handle_update(const dpp::message_update_t& event);

private:
    commands::CommandRouter& command_router_;
    services::ChatContextService& chat_context_service_;
    dpp::cluster& bot_;
    std::mutex mutex_;
};

} // namespace handlers
