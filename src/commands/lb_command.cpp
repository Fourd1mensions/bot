#include "commands/lb_command.h"
#include <bot.h>
#include <algorithm>

namespace commands {

LbCommand::LbCommand(Bot& bot) : bot_(bot) {}

std::vector<std::string> LbCommand::get_aliases() const {
    return {"!lb", "!ди"};
}

std::string LbCommand::parse_mods_filter(const std::string& content) const {
    std::string mods_filter;
    size_t plus_pos = content.find('+');
    if (plus_pos != std::string::npos) {
        mods_filter = content.substr(plus_pos + 1);
        // Remove spaces and extra plus signs, convert to uppercase
        mods_filter.erase(std::remove(mods_filter.begin(), mods_filter.end(), ' '), mods_filter.end());
        mods_filter.erase(std::remove(mods_filter.begin(), mods_filter.end(), '+'), mods_filter.end());
        std::transform(mods_filter.begin(), mods_filter.end(), mods_filter.begin(),
            [](unsigned char c) { return std::toupper(c); });
    }
    return mods_filter;
}

void LbCommand::execute(const CommandContext& ctx) {
    std::string mods_filter = parse_mods_filter(ctx.content);
    bot_.create_lb_message(ctx.event, mods_filter);
}

} // namespace commands
