#include "commands/command_router.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace commands {

void CommandRouter::register_command(std::unique_ptr<ICommand> command) {
    commands_.push_back(std::move(command));
}

bool CommandRouter::route(const dpp::message_create_t& event) {
    // Build context
    std::string content = event.msg.content;
    std::string content_lower = content;
    std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    CommandContext ctx{
        .event = event,
        .content = content,
        .content_lower = content_lower,
        .args = "",
        .services = services_
    };

    // Find matching command
    for (auto& cmd : commands_) {
        if (cmd->matches(ctx)) {
            // Execute in separate thread
            std::jthread([cmd_ptr = cmd.get(), ctx_copy = ctx]() mutable {
                try {
                    cmd_ptr->execute(ctx_copy);
                } catch (const std::exception& e) {
                    spdlog::error("[CMD] Command execution failed: {}", e.what());
                }
            }).detach();
            return true;
        }
    }

    return false;
}

} // namespace commands
