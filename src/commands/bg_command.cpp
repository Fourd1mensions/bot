#include "commands/bg_command.h"
#include "services/service_container.h"
#include "services/beatmap_extract_service.h"
#include "services/message_presenter_service.h"
#include <beatmap_downloader.h>
#include <utils.h>
#include <error_messages.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace commands {

std::vector<std::string> BgCommand::get_aliases() const {
    return {"!bg"};
}

void BgCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[bg] ServiceContainer is null");
        return;
    }

    // Show typing indicator (only for text commands)
    if (!ctx.is_slash()) {
        s->bot.channel_typing(ctx.channel_id());
    }

    // Extract beatmap files using shared service
    auto result = s->beatmap_extract_service.extract_for_channel(ctx.channel_id());
    if (!result.success) {
        ctx.reply(s->message_presenter.build_error_message(result.error_message));
        return;
    }

    // Find background file in extract
    auto bg_filename = s->beatmap_downloader.find_background_in_extract(result.extract_path);
    if (!bg_filename) {
        ctx.reply(s->message_presenter.build_error_message(error_messages::NO_BACKGROUND));
        return;
    }

    // Construct the background URL (with URL-encoded filename)
    std::string bg_url = fmt::format("{}/osu/{}/{}",
        s->config.public_url, result.extract_id, utils::url_encode(*bg_filename));

    // Create embed with background image using presenter service
    std::string footer_text = s->beatmap_downloader.build_download_footer(result.beatmapset_id);
    dpp::message msg = s->message_presenter.build_background(result.beatmap, bg_url, footer_text);
    ctx.reply(msg);
}

} // namespace commands
