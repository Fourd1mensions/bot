#pragma once

#include "commands/command.h"

namespace commands {

class BgCommand : public ICommand {
public:
    BgCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "bg"; }
    void execute_unified(const UnifiedContext& ctx) override;
};

} // namespace commands
