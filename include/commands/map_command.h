#pragma once

#include "commands/command.h"

namespace commands {

class MapCommand : public ICommand {
public:
    MapCommand() = default;

    std::vector<std::string> get_aliases() const override;
    void execute(const CommandContext& ctx) override;

private:
    std::string parse_mods_filter(const std::string& content) const;
};

} // namespace commands
