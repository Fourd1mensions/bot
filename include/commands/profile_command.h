#pragma once

#include "commands/command.h"

namespace commands {

class ProfileCommand : public ICommand {
public:
    ProfileCommand() = default;

    std::vector<std::string> get_aliases() const override;
    std::string get_slash_name() const override { return "osu"; }
    void execute_unified(const UnifiedContext& ctx) override;

private:
    void show_profile(const UnifiedContext& ctx, const std::string& username, const std::string& mode);
};

} // namespace commands
