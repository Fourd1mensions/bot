#include "commands/map_command.h"
#include <bot.h>
#include <algorithm>

namespace commands {

MapCommand::MapCommand(Bot& bot) : bot_(bot) {}

std::vector<std::string> MapCommand::get_aliases() const {
    return {"!map", "!m"};
}

std::string MapCommand::parse_mods_filter(const std::string& content) const {
    std::string mods_filter;
    size_t plus_pos = content.find('+');
    if (plus_pos != std::string::npos) {
        mods_filter = content.substr(plus_pos + 1);
        mods_filter.erase(std::remove(mods_filter.begin(), mods_filter.end(), ' '), mods_filter.end());
        mods_filter.erase(std::remove(mods_filter.begin(), mods_filter.end(), '+'), mods_filter.end());
        std::transform(mods_filter.begin(), mods_filter.end(), mods_filter.begin(),
            [](unsigned char c) { return std::toupper(c); });
    }
    return mods_filter;
}

void MapCommand::execute(const CommandContext& ctx) {
    std::string mods_filter = parse_mods_filter(ctx.content);
    bot_.create_map_message(ctx.event, mods_filter);
}

} // namespace commands
