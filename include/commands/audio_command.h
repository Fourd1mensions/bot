#pragma once

#include "commands/command.h"

class Bot;

namespace commands {

class AudioCommand : public ICommand {
public:
    explicit AudioCommand(Bot& bot);

    std::vector<std::string> get_aliases() const override;
    void execute(const CommandContext& ctx) override;

private:
    Bot& bot_;
};

} // namespace commands
