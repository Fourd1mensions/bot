#pragma once

#include "commands/command.h"

namespace commands {

class CompareCommand : public ICommand {
public:
    CompareCommand() = default;

    std::vector<std::string> get_aliases() const override;
    void execute(const CommandContext& ctx) override;

private:
    std::string parse_params(const std::string& content) const;
};

} // namespace commands
