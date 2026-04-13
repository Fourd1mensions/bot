#pragma once

#include "commands/command.h"

namespace commands {

class MapCommand : public ICommand {
public:
    MapCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "map"; }
    void execute_unified(const UnifiedContext& ctx) override;
};

} // namespace commands
