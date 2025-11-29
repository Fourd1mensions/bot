#include "commands/bg_command.h"
#include <bot.h>

namespace commands {

BgCommand::BgCommand(Bot& bot) : bot_(bot) {}

std::vector<std::string> BgCommand::get_aliases() const {
    return {"!bg"};
}

void BgCommand::execute(const CommandContext& ctx) {
    bot_.create_bg_message(ctx.event);
}

} // namespace commands
