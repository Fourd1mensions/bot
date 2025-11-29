#include "commands/audio_command.h"
#include <bot.h>

namespace commands {

AudioCommand::AudioCommand(Bot& bot) : bot_(bot) {}

std::vector<std::string> AudioCommand::get_aliases() const {
    return {"!song", "!audio"};
}

void AudioCommand::execute(const CommandContext& ctx) {
    bot_.create_audio_message(ctx.event);
}

} // namespace commands
