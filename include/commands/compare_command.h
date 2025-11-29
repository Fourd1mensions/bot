#pragma once

#include "commands/command.h"

class Bot;

namespace commands {

class CompareCommand : public ICommand {
public:
    explicit CompareCommand(Bot& bot);

    std::vector<std::string> get_aliases() const override;
    void execute(const CommandContext& ctx) override;

private:
    Bot& bot_;
    std::string parse_params(const std::string& content) const;
};

} // namespace commands
