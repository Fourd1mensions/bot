#pragma once

#include <dpp/dpp.h>
#include <string>
#include <vector>
#include <functional>

namespace commands {

/**
 * Context passed to command handlers.
 * Contains everything needed to execute a command.
 * Note: event is stored by value to ensure thread safety when
 * commands execute asynchronously.
 */
struct CommandContext {
    dpp::message_create_t event;  // Stored by value for thread safety
    std::string content;          // Original message content
    std::string content_lower;    // Lowercase content for matching
    std::string args;             // Arguments after command name
};

/**
 * Base interface for text commands (!lb, !rs, etc.)
 */
class ICommand {
public:
    virtual ~ICommand() = default;

    /**
     * Get command aliases (e.g., {"!lb", "!ди"})
     */
    virtual std::vector<std::string> get_aliases() const = 0;

    /**
     * Check if this command matches the message.
     * Default implementation checks if content starts with any alias.
     */
    virtual bool matches(const CommandContext& ctx) const {
        for (const auto& alias : get_aliases()) {
            if (ctx.content_lower.find(alias) == 0) {
                return true;
            }
        }
        return false;
    }

    /**
     * Execute the command.
     * Called in a separate thread by the router.
     */
    virtual void execute(const CommandContext& ctx) = 0;
};

} // namespace commands
