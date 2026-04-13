#pragma once

#include "commands/command.h"

namespace commands {

class LinkCommand : public ICommand {
public:
    LinkCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "link"; }
    bool matches(const CommandContext& ctx) const override;
    void execute_unified(const UnifiedContext& ctx) override;
};

} // namespace commands
