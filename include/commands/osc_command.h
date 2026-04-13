#pragma once

#include "commands/command.h"

namespace commands {

class OscCommand : public ICommand {
public:
    OscCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "osc"; }
    void execute_unified(const UnifiedContext& ctx) override;
};

} // namespace commands
