#include "handlers/message_handler.h"
#include <services/chat_context_service.h>
#include <spdlog/spdlog.h>

namespace handlers {

MessageHandler::MessageHandler(
    commands::CommandRouter& command_router,
    services::ChatContextService& chat_context_service,
    dpp::cluster& bot
)
    : command_router_(command_router)
    , chat_context_service_(chat_context_service)
    , bot_(bot)
{}

void MessageHandler::handle_create(const dpp::message_create_t& event) {
    const bool is_self = (event.msg.author.id == bot_.me.id);

    spdlog::info("[MSG] user={} ({}) channel={} content=\"{}\"",
        event.msg.author.id.str(), event.msg.author.username,
        event.msg.channel_id.str(), event.msg.content);

    // Update context for all messages, but skip download callbacks for own messages
    // (our embeds contain beatmap URLs that would re-trigger downloads)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chat_context_service_.update_context(
            event.raw_event,
            event.msg.content,
            event.msg.channel_id.str(),
            event.msg.id.str(),
            is_self  // skip_callbacks for bot's own messages
        );
    }

    if (event.msg.author.is_bot()) {
        return;
    }

    command_router_.route(event);
}

void MessageHandler::handle_update(const dpp::message_update_t& event) {
    dpp::snowflake channel_id = event.msg.channel_id;
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
