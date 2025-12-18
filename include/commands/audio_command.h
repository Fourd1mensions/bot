#pragma once

#include "commands/command.h"

namespace commands {

class AudioCommand : public ICommand {
public:
    AudioCommand() = default;

    std::vector<std::string> get_aliases() const override;
    void execute(const CommandContext& ctx) override;
};

} // namespace commands
