#include "commands/bg_command.h"
#include "services/service_container.h"
#include "services/chat_context_service.h"
#include "services/beatmap_resolver_service.h"
#include "services/message_presenter_service.h"
#include <beatmap_downloader.h>
#include <requests.h>
#include <osu.h>
#include <utils.h>
#include <error_messages.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace commands {

std::vector<std::string> BgCommand::get_aliases() const {
    return {"!bg"};
}

void BgCommand::execute(const CommandContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[!bg] ServiceContainer is null");
        return;
    }

    const auto& event = ctx.event;
    dpp::snowflake channel_id = event.msg.channel_id;

    std::string stored_id = s->chat_context_service.get_beatmap_id(channel_id);
    std::string beatmap_id = s->beatmap_resolver_service.resolve_beatmap_id(stored_id);

    if (beatmap_id.empty()) {
        event.reply(s->message_presenter.build_error_message(error_messages::NO_BEATMAP_IN_CHANNEL));
        return;
    }

    // Show typing indicator
    s->bot.channel_typing(event.msg.channel_id);

    auto start = std::chrono::steady_clock::now();

    std::string response_beatmap = s->request.get_beatmap(beatmap_id);
    if (response_beatmap.empty()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        if (elapsed > 8) {
            event.reply(s->message_presenter.build_error_message(
                fmt::format(error_messages::API_TIMEOUT_FORMAT, elapsed)));
        } else {
            event.reply(s->message_presenter.build_error_message(error_messages::API_NO_RESPONSE));
        }
        spdlog::error("[!bg] Unable to get beatmap from API");
        return;
    }

    Beatmap beatmap(response_beatmap);
    uint32_t beatmapset_id = beatmap.get_beatmapset_id();

    spdlog::info("[!bg] Processing beatmapset_id: {}", beatmapset_id);

    // Download .osz file if needed
    if (!s->beatmap_downloader.download_osz(beatmapset_id)) {
        spdlog::error("[!bg] download_osz failed for beatmapset {}", beatmapset_id);
        event.reply(s->message_presenter.build_error_message(error_messages::DOWNLOAD_FAILED));
        return;
    }

    spdlog::info("[!bg] Download complete, creating extract...");

    // Create temporary extract
    auto extract_id = s->beatmap_downloader.create_extract(beatmapset_id);
    if (!extract_id) {
        spdlog::error("[!bg] Failed to create extract for beatmapset {}", beatmapset_id);
        event.reply(s->message_presenter.build_error_message(error_messages::EXTRACT_FAILED));
        return;
    }

    // Find background file in extract
    auto extract_path = s->beatmap_downloader.get_extract_path(*extract_id);
    if (!extract_path) {
        event.reply(s->message_presenter.build_error_message(error_messages::EXTRACT_NOT_FOUND));
        return;
    }

    auto bg_filename = s->beatmap_downloader.find_background_in_extract(*extract_path);
    if (!bg_filename) {
        event.reply(s->message_presenter.build_error_message(error_messages::NO_BACKGROUND));
        return;
    }

    // Construct the background URL (with URL-encoded filename)
    std::string bg_url = fmt::format("{}/osu/{}/{}",
        s->config.public_url, *extract_id, utils::url_encode(*bg_filename));

    // Create embed with background image using presenter service
    std::string footer_text = s->beatmap_downloader.build_download_footer(beatmapset_id);
    dpp::message msg = s->message_presenter.build_background(beatmap, bg_url, footer_text);
    event.reply(msg);

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    if (elapsed > 8) {
        spdlog::warn("[CMD] !bg took {}s to complete (slow download or API response)", elapsed);
    }
}

} // namespace commands
