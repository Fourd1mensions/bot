#pragma once

#include "commands/command.h"

namespace commands {

class AudioCommand : public ICommand {
public:
    AudioCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "audio"; }
    void execute_unified(const UnifiedContext& ctx) override;
};

} // namespace commands
