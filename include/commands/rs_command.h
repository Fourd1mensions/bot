#pragma once

#include "commands/command.h"

class Bot;

namespace commands {

class RsCommand : public ICommand {
public:
    explicit RsCommand(Bot& bot);

    std::vector<std::string> get_aliases() const override;
    bool matches(const CommandContext& ctx) const override;
    void execute(const CommandContext& ctx) override;

private:
    Bot& bot_;

    struct ParsedParams {
        std::string mode = "osu";
        std::string params;
        bool valid = true;
        std::string error_message;
    };

    ParsedParams parse(const CommandContext& ctx) const;
};

} // namespace commands
