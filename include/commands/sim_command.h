#pragma once

#include "commands/command.h"

namespace commands {

class SimCommand : public ICommand {
public:
    SimCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "sim"; }
    void execute_unified(const UnifiedContext& ctx) override;

private:
    struct ParsedParams {
        double accuracy = 0.0;
        std::string mode = "osu";
        std::string mods_filter;
        int combo = 0;
        int count_100 = -1;
        int count_50 = -1;
        int misses = 0;
        double ratio = -1.0;
        bool valid = true;
        std::string error_message;
    };

    ParsedParams parse(const std::string& content, const std::string& prefix = "!") const;
    int parse_int_param(const std::string& content, const std::string& param) const;
    double parse_ratio(const std::string& content) const;
};

} // namespace commands
