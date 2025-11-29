#pragma once

#include "commands/command.h"

class Bot;

namespace commands {

class MapCommand : public ICommand {
public:
    explicit MapCommand(Bot& bot);

    std::vector<std::string> get_aliases() const override;
    void execute(const CommandContext& ctx) override;

private:
    Bot& bot_;
    std::string parse_mods_filter(const std::string& content) const;
};

} // namespace commands
