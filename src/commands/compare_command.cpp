#include "commands/compare_command.h"
#include <bot.h>

namespace commands {

CompareCommand::CompareCommand(Bot& bot) : bot_(bot) {}

std::vector<std::string> CompareCommand::get_aliases() const {
    return {"!compare", "!c"};
}

std::string CompareCommand::parse_params(const std::string& content) const {
    size_t cmd_start = content.find('!');
    size_t cmd_end = content.find(' ', cmd_start);
    if (cmd_end == std::string::npos) {
        cmd_end = content.length();
    }

    std::string params = content.length() > cmd_end ? content.substr(cmd_end) : "";

    // Trim leading spaces
    size_t start = params.find_first_not_of(" \t");
    if (start != std::string::npos) {
        params = params.substr(start);
    } else {
        params = "";
    }

    return params;
}

void CompareCommand::execute(const CommandContext& ctx) {
    std::string params = parse_params(ctx.content);
    bot_.create_compare_message(ctx.event, params);
}

} // namespace commands
