#pragma once

#include "commands/command.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <thread>

struct ServiceContainer;

namespace commands {

/**
 * Routes incoming messages and slash commands to appropriate command handlers.
 */
class CommandRouter {
public:
    CommandRouter() = default;
    ~CommandRouter() = default;

    // Disable copy
    CommandRouter(const CommandRouter&) = delete;
    CommandRouter& operator=(const CommandRouter&) = delete;

    /**
     * Set the service container for commands.
     */
    void set_services(ServiceContainer* services) { services_ = services; }

    /**
     * Register a command handler.
     * If the command has a slash name, it will also be registered for slash command routing.
     */
    void register_command(std::unique_ptr<ICommand> command);

    /**
     * Try to route a text message to a matching command.
     * Executes the command in a separate thread if matched.
     * @return true if a command was matched and executed
     */
    bool route(const dpp::message_create_t& event);

    /**
     * Try to route a slash command to a matching command.
     * Executes the command in a separate thread if matched.
     * @return true if a command was matched and executed
     */
    bool route_slash(const dpp::slashcommand_t& event);

    /**
     * Get a command by its slash name.
     * @return pointer to the command or nullptr if not found
     */
    ICommand* get_slash_command(const std::string& name) const;

private:
    std::vector<std::unique_ptr<ICommand>> commands_;
    std::unordered_map<std::string, ICommand*> slash_commands_;  // Maps slash name to command
    ServiceContainer* services_ = nullptr;
};

} // namespace commands
