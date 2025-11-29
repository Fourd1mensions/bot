#include "commands/rs_command.h"
#include <bot.h>
#include <algorithm>

namespace commands {

RsCommand::RsCommand(Bot& bot) : bot_(bot) {}

std::vector<std::string> RsCommand::get_aliases() const {
    return {"!rs", "!кы"};
}

bool RsCommand::matches(const CommandContext& ctx) const {
    return ctx.content_lower.find("!rs") == 0 || ctx.content.find("!кы") == 0;
}

RsCommand::ParsedParams RsCommand::parse(const CommandContext& ctx) const {
    ParsedParams result;
    std::string content = ctx.content;

    size_t cmd_end = 3; // Length of "!rs"
    if (content.find("!кы") == 0) {
        cmd_end = 7; // Length of "!кы" in bytes (UTF-8: 4 bytes for each Cyrillic char + 1 for !)
    }

    // Check for mode specification (e.g., !rs:taiko)
    size_t colon_pos = content.find(':');
    if (colon_pos != std::string::npos && colon_pos < cmd_end + 10) {
        size_t mode_end = content.find(' ', colon_pos);
        if (mode_end == std::string::npos) {
            mode_end = content.length();
        }
        result.mode = content.substr(colon_pos + 1, mode_end - colon_pos - 1);
        std::transform(result.mode.begin(), result.mode.end(), result.mode.begin(), ::tolower);

        // Validate mode
        if (result.mode != "osu" && result.mode != "taiko" && result.mode != "catch" &&
            result.mode != "mania" && result.mode != "fruits" && result.mode != "ctb") {
            result.valid = false;
            result.error_message = "Invalid mode. Supported modes: `osu`, `taiko`, `catch`/`fruits`, `mania`";
            return result;
        }

        // Normalize mode names
        if (result.mode == "ctb") result.mode = "catch";
        if (result.mode == "fruits") result.mode = "catch";

        cmd_end = mode_end;
    }

    // Extract params after command
    result.params = content.length() > cmd_end ? content.substr(cmd_end) : "";

    // Trim leading spaces
    size_t start = result.params.find_first_not_of(" \t");
    if (start != std::string::npos) {
        result.params = result.params.substr(start);
    } else {
        result.params = "";
    }

    return result;
}

void RsCommand::execute(const CommandContext& ctx) {
    auto parsed = parse(ctx);

    if (!parsed.valid) {
        ctx.event.reply(parsed.error_message);
        return;
    }

    bot_.create_rs_message(ctx.event, parsed.mode, parsed.params);
}

} // namespace commands
