#include "commands/settings_command.h"
#include "services/service_container.h"
#include "services/user_settings_service.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace commands {

void SettingsCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        ctx.reply(dpp::message(":x: Internal service error."));
        return;
    }

    auto preset_str = ctx.get_string_param("embed_preset");
    if (!preset_str) {
        auto current = s->user_settings_service.get_preset(ctx.author_id());
        auto embed = dpp::embed()
            .set_color(dpp::colors::viola_purple)
            .set_description(fmt::format("Current embed preset: **{}**\n\nUse `/settings embed_preset:<preset>` to change.\nAvailable: `compact`, `classic`, `extended`",
                services::embed_preset_to_string(current)));

        dpp::message msg;
        msg.add_embed(embed);
        ctx.reply(msg);
        return;
    }

    auto preset = services::embed_preset_from_string(*preset_str);
    s->user_settings_service.set_preset(ctx.author_id(), preset);

    auto embed = dpp::embed()
        .set_color(dpp::colors::viola_purple)
        .set_description(fmt::format("Embed preset set to **{}**", services::embed_preset_to_string(preset)));

    dpp::message msg;
    msg.add_embed(embed);
    ctx.reply(msg);

    spdlog::info("[Settings] User {} set embed preset to {}", ctx.author_id().str(), *preset_str);
}

} // namespace commands
