#pragma once

#include "commands/command.h"

namespace commands {

class SettingsCommand : public ICommand {
public:
    SettingsCommand() = default;

    std::vector<std::string> get_aliases() const override { return {}; }
    std::string get_slash_name() const override { return "settings"; }
    bool matches(const CommandContext&) const override { return false; }
    void execute_unified(const UnifiedContext& ctx) override;
};

} // namespace commands
