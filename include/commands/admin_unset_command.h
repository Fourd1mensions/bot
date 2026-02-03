#pragma once

#include "commands/command.h"

namespace commands {

/**
 * Admin command to remove a user from tracking (unlink Discord <-> osu!).
 * Usage: !adminunset <discord_id_or_mention>
 * Only admins (from config) can use this command.
 */
class AdminUnsetCommand : public ICommand {
public:
    AdminUnsetCommand() = default;

    std::vector<std::string> get_aliases() const override;
    void execute_unified(const UnifiedContext& ctx) override;
};

} // namespace commands
