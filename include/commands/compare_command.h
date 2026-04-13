#pragma once

#include "commands/command.h"

namespace commands {

class CompareCommand : public ICommand {
public:
    CompareCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "compare"; }
    void execute_unified(const UnifiedContext& ctx) override;

private:
    std::string parse_params(const std::string& content) const;
};

} // namespace commands
