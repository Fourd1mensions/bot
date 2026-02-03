#pragma once

#include "commands/command.h"

namespace commands {

/**
 * Admin command to link any Discord user to an osu! account.
 * Usage: !adminset <discord_id_or_mention> <osu_username>
 * Only admins (from config) can use this command.
 */
class AdminSetCommand : public ICommand {
public:
    AdminSetCommand() = default;

    std::vector<std::string> get_aliases() const override;
    void execute_unified(const UnifiedContext& ctx) override;
};

} // namespace commands
