#include "commands/sim_command.h"
#include <bot.h>
#include <algorithm>
#include <cctype>

namespace commands {

SimCommand::SimCommand(Bot& bot) : bot_(bot) {}

std::vector<std::string> SimCommand::get_aliases() const {
    return {"!sim"};
}

int SimCommand::parse_int_param(const std::string& content, const std::string& param) const {
    size_t param_pos = content.find(param);
    if (param_pos != std::string::npos) {
        size_t value_start = param_pos + param.length();
        // Skip spaces
        while (value_start < content.length() && content[value_start] == ' ') {
            value_start++;
        }
        // Extract number
        size_t value_end = value_start;
        while (value_end < content.length() && std::isdigit(content[value_end])) {
            value_end++;
        }
        if (value_end > value_start) {
            try {
                return std::stoi(content.substr(value_start, value_end - value_start));
            } catch (...) {}
        }
    }
    return -1;
}

double SimCommand::parse_ratio(const std::string& content) const {
    size_t ratio_pos = content.find("-r");
    if (ratio_pos != std::string::npos) {
        size_t value_start = ratio_pos + 2;
        while (value_start < content.length() && content[value_start] == ' ') {
            value_start++;
        }
        size_t value_end = value_start;
        while (value_end < content.length() &&
               (std::isdigit(content[value_end]) || content[value_end] == '.')) {
            value_end++;
        }
        if (value_end > value_start) {
            try {
                return std::stod(content.substr(value_start, value_end - value_start));
            } catch (...) {}
        }
    }
    return -1.0;
}

SimCommand::ParsedParams SimCommand::parse(const std::string& content) const {
    ParsedParams result;

    // Parse mode (e.g., "!sim:taiko")
    size_t colon_pos = content.find(':');
    size_t space_pos = content.find(' ');
    if (colon_pos != std::string::npos && (space_pos == std::string::npos || colon_pos < space_pos)) {
        size_t mode_end = content.find(' ', colon_pos);
        result.mode = content.substr(colon_pos + 1, mode_end - colon_pos - 1);
        std::transform(result.mode.begin(), result.mode.end(), result.mode.begin(), ::tolower);

        if (result.mode != "osu" && result.mode != "taiko" && result.mode != "catch" && result.mode != "mania") {
            result.valid = false;
            result.error_message = "Invalid mode. Supported modes: `osu`, `taiko`, `catch`, `mania`";
            return result;
        }
    }

    // Find percentage
    size_t percent_pos = content.find('%');
    if (percent_pos == std::string::npos) {
        result.valid = false;
        result.error_message =
            "Usage: `!sim[:mode] <accuracy>% [+mods] [-c COMBO] [-n100 X] [-n50 X] [-n0 X] [-r RATIO]`\n"
            "Modes: `osu` (default), `taiko`, `catch`, `mania`\n"
            "Examples:\n"
            "• `!sim 99% +HDDT` - standard osu!\n"
            "• `!sim:taiko 100% +HR` - taiko mode\n"
            "• `!sim 100% -n100 5 -c 1500` - 5x100, 1500x combo\n"
            "• `!sim:mania 99% -r 0.95` - mania with 95% ratio";
        return result;
    }

    // Extract accuracy value
    size_t start_pos = content.find_first_of("0123456789");
    if (start_pos == std::string::npos || start_pos >= percent_pos) {
        result.valid = false;
        result.error_message = "Invalid accuracy format. Example: `!sim 99%`";
        return result;
    }

    std::string acc_str = content.substr(start_pos, percent_pos - start_pos);
    try {
        result.accuracy = std::stod(acc_str) / 100.0;
        if (result.accuracy < 0.0 || result.accuracy > 1.0) {
            result.valid = false;
            result.error_message = "Accuracy must be between 0% and 100%.";
            return result;
        }
    } catch (const std::exception&) {
        result.valid = false;
        result.error_message = "Invalid accuracy value. Example: `!sim 99%`";
        return result;
    }

    // Parse mods
    size_t plus_pos = content.find('+');
    if (plus_pos != std::string::npos) {
        size_t mods_end = content.find(" -", plus_pos);
        std::string mods_substr = (mods_end != std::string::npos)
            ? content.substr(plus_pos + 1, mods_end - plus_pos - 1)
            : content.substr(plus_pos + 1);

        result.mods_filter = mods_substr;
        result.mods_filter.erase(std::remove(result.mods_filter.begin(), result.mods_filter.end(), ' '), result.mods_filter.end());
        result.mods_filter.erase(std::remove(result.mods_filter.begin(), result.mods_filter.end(), '+'), result.mods_filter.end());
        std::transform(result.mods_filter.begin(), result.mods_filter.end(), result.mods_filter.begin(),
            [](unsigned char c) { return std::toupper(c); });
    }

    // Parse hit count parameters
    result.count_100 = parse_int_param(content, "-n100");
    result.count_50 = parse_int_param(content, "-n50");
    int n0 = parse_int_param(content, "-n0");
    if (n0 >= 0) {
        result.misses = n0;
    }

    // Parse combo
    int combo_param = parse_int_param(content, "-c");
    if (combo_param > 0) {
        result.combo = combo_param;
    }

    // Parse ratio (mania only)
    if (result.mode == "mania") {
        result.ratio = parse_ratio(content);
    }

    return result;
}

void SimCommand::execute(const CommandContext& ctx) {
    auto parsed = parse(ctx.content);

    if (!parsed.valid) {
        ctx.event.reply(parsed.error_message);
        return;
    }

    bot_.create_sim_message(ctx.event, parsed.accuracy, parsed.mode, parsed.mods_filter,
        parsed.combo, parsed.count_100, parsed.count_50, parsed.misses, parsed.ratio);
}

} // namespace commands
