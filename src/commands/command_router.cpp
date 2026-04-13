#include "commands/command_router.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace commands {

void CommandRouter::register_command(std::unique_ptr<ICommand> command) {
    // Register for slash command routing if supported
    std::string slash_name = command->get_slash_name();
    if (!slash_name.empty()) {
        slash_commands_[slash_name] = command.get();
        spdlog::debug("[Router] Registered slash command: /{}", slash_name);
    }

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
        .services = services_,
        .prefix = prefix_
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

bool CommandRouter::route_slash(const dpp::slashcommand_t& event) {
    const std::string& cmd_name = event.command.get_command_name();

    auto it = slash_commands_.find(cmd_name);
    if (it == slash_commands_.end()) {
        return false;
    }

    ICommand* cmd = it->second;

    // Try to extract "params" option if present (unified parameter passing)
    std::string args;
    try {
        auto param = event.get_parameter("params");
        if (std::holds_alternative<std::string>(param)) {
            args = std::get<std::string>(param);
        }
    } catch (...) {
        // No params parameter, that's fine
    }

    // Build slash command context
    SlashCommandContext ctx{
        .event = event,
        .command_name = cmd_name,
        .args = args,
        .services = services_,
        .prefix = prefix_
    };

    // Execute in separate thread
    std::jthread([cmd, ctx_copy = ctx]() mutable {
        try {
            cmd->execute(ctx_copy);
        } catch (const std::exception& e) {
            spdlog::error("[CMD] Slash command execution failed: {}", e.what());
        }
    }).detach();

    return true;
}

ICommand* CommandRouter::get_slash_command(const std::string& name) const {
    auto it = slash_commands_.find(name);
    return it != slash_commands_.end() ? it->second : nullptr;
}

} // namespace commands
