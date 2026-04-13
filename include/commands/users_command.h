#pragma once

#include "commands/command.h"

namespace commands {

/**
 * Command to display all tracked users (Discord <-> osu! mappings)
 * Usage: !users
 */
class UsersCommand : public ICommand {
public:
    UsersCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "users"; }
    void execute_unified(const UnifiedContext& ctx) override;
};

} // namespace commands
