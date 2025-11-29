#pragma once

#include "commands/command.h"
#include <memory>
#include <vector>
#include <thread>

namespace commands {

/**
 * Routes incoming messages to appropriate command handlers.
 */
class CommandRouter {
public:
    CommandRouter() = default;
    ~CommandRouter() = default;

    // Disable copy
    CommandRouter(const CommandRouter&) = delete;
    CommandRouter& operator=(const CommandRouter&) = delete;

    /**
     * Register a command handler.
     */
    void register_command(std::unique_ptr<ICommand> command);

    /**
     * Try to route a message to a matching command.
     * Executes the command in a separate thread if matched.
     * @return true if a command was matched and executed
     */
    bool route(const dpp::message_create_t& event);

private:
    std::vector<std::unique_ptr<ICommand>> commands_;
};

} // namespace commands
