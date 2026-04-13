#pragma once

#include "commands/command.h"
#include <osu.h>
#include "state/session_state.h"

namespace commands {

/**
 * TopCommand - Show user's top plays with filters and sorting
 * Similar to bathbot's !top command
 *
 * Usage:
 *   !top [username] [options]
 *
 * Options:
 *   -s, --sort <method>   Sort by: pp, acc, score, combo, date, misses
 *   -m, --mods <mods>     Filter by mods (e.g., +HDDT, -HR for exclude)
 *   -g, --grade <grade>   Filter by grade (SS, S, A, B, C, D)
 *   -r, --reverse         Reverse sort order
 *   --index <N>           Show specific score (1-100)
 */
class TopCommand : public ICommand {
public:
    TopCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "top"; }
    bool matches(const CommandContext& ctx) const override;
    void execute_unified(const UnifiedContext& ctx) override;

    struct ParsedParams {
        std::string username;
        std::string mode = "osu";
        std::string mods_filter;      // +HDDT or -HR for exclude
        std::string grade_filter;     // SS, S, A, B, C, D
        TopSortMethod sort_method = TopSortMethod::PP;
        bool reverse = false;
        size_t index = 0;             // 0 = show all, 1-100 = show specific
        bool valid = true;
        std::string error_message;
    };

private:
    ParsedParams parse(const std::string& content) const;
};

} // namespace commands
