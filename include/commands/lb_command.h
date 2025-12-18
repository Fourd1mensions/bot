#pragma once

#include "commands/command.h"
#include <osu.h>
#include <state/session_state.h>
#include <string>
#include <optional>

namespace commands {

// Use LbSortMethod from session_state.h to avoid duplication
using ::LbSortMethod;

/**
 * Parsed parameters for leaderboard command
 */
struct LbParams {
    std::string mods_filter;              // e.g., "HDDT"
    std::optional<std::string> beatmap_id; // If specified in command
    LbSortMethod sort_method = LbSortMethod::PP;
};

/**
 * Leaderboard command (!lb, !ди)
 * Shows scores of linked users on the current beatmap.
 *
 * Usage:
 *   !lb                    - leaderboard for last beatmap in channel
 *   !lb <beatmap_url>      - leaderboard for specific beatmap
 *   !lb +HDDT              - filter by mods
 *   !lb -s score           - sort by score instead of PP
 *   !lb <url> +HD -s acc   - combine all options
 *
 * Sort methods: pp (default), score, acc, combo, date
 */
class LbCommand : public ICommand {
public:
    LbCommand() = default;

    std::vector<std::string> get_aliases() const override;
    void execute(const CommandContext& ctx) override;

private:
    /**
     * Parse all parameters from command content.
     */
    LbParams parse_params(const std::string& content) const;

    /**
     * Extract beatmap ID from URL or raw ID.
     */
    std::optional<std::string> extract_beatmap_id(const std::string& token) const;

    /**
     * Parse sort method string to enum.
     */
    LbSortMethod parse_sort_method(const std::string& method) const;
};

} // namespace commands
