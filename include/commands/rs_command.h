#pragma once

#include "commands/command.h"

namespace commands {

class RsCommand : public ICommand {
public:
    RsCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "rs"; }
    bool matches(const CommandContext& ctx) const override;
    void execute_unified(const UnifiedContext& ctx) override;

private:
    struct ParsedParams {
        std::string mode = "osu";
        std::string params;
        bool valid = true;
        std::string error_message;
    };

    ParsedParams parse(const std::string& content) const;
};

} // namespace commands
