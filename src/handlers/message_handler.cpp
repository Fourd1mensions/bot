#include "handlers/message_handler.h"
#include <services/chat_context_service.h>
#include <spdlog/spdlog.h>

namespace handlers {

MessageHandler::MessageHandler(
    commands::CommandRouter& command_router,
    services::ChatContextService& chat_context_service
)
    : command_router_(command_router)
    , chat_context_service_(chat_context_service)
{}

void MessageHandler::handle_create(const dpp::message_create_t& event) {
    spdlog::info("[MSG] user={} ({}) channel={} content=\"{}\"",
        event.msg.author.id.str(), event.msg.author.username,
        event.msg.channel_id.str(), event.msg.content);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        chat_context_service_.update_context(
            event.raw_event,
            event.msg.content,
            event.msg.channel_id.str(),
            event.msg.id.str()
        );
    }

    // Route to command handlers
    command_router_.route(event);
}

void MessageHandler::handle_update(const dpp::message_update_t& event) {
    dpp::snowflake channel_id = event.msg.channel_id;

    // Check if the updated message is the one tracked in chat context
    dpp::snowflake tracked_msg_id = chat_context_service_.get_message_id(channel_id);
    if (tracked_msg_id == event.msg.id) {
        chat_context_service_.update_context(
            event.raw_event,
            event.msg.content,
            channel_id,
            event.msg.id
        );
    }
}

} // namespace handlers
