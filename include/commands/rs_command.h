#pragma once

#include "commands/command.h"

namespace commands {

class RsCommand : public ICommand {
public:
    RsCommand() = default;

    std::vector<std::string> get_aliases() const override;
    bool matches(const CommandContext& ctx) const override;
    void execute(const CommandContext& ctx) override;

private:
    struct ParsedParams {
        std::string mode = "osu";
        std::string params;
        bool valid = true;
        std::string error_message;
    };

    ParsedParams parse(const CommandContext& ctx) const;
};

} // namespace commands
