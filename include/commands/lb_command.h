#pragma once

#include "commands/command.h"
#include <string>

// Forward declaration
class Bot;

namespace commands {

/**
 * Leaderboard command (!lb, !ди)
 * Shows scores of linked users on the current beatmap.
 */
class LbCommand : public ICommand {
public:
    explicit LbCommand(Bot& bot);

    std::vector<std::string> get_aliases() const override;
    void execute(const CommandContext& ctx) override;

private:
    Bot& bot_;

    /**
     * Parse mods filter from command arguments.
     * E.g., "!lb +hddt" -> "HDDT"
     */
    std::string parse_mods_filter(const std::string& content) const;
};

} // namespace commands
