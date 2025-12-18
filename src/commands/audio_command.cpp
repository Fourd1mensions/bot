#include "commands/audio_command.h"
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
#include <fstream>
#include <filesystem>

namespace commands {

std::vector<std::string> AudioCommand::get_aliases() const {
    return {"!song", "!audio"};
}

void AudioCommand::execute(const CommandContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[!song] ServiceContainer is null");
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
        spdlog::error("[!song] Unable to get beatmap from API");
        return;
    }

    Beatmap beatmap(response_beatmap);
    uint32_t beatmapset_id = beatmap.get_beatmapset_id();

    spdlog::info("[!song] Processing beatmapset_id: {}", beatmapset_id);

    // Download .osz file if needed
    if (!s->beatmap_downloader.download_osz(beatmapset_id)) {
        spdlog::error("[!song] download_osz failed for beatmapset {}", beatmapset_id);
        event.reply(s->message_presenter.build_error_message(error_messages::DOWNLOAD_FAILED));
        return;
    }

    spdlog::info("[!song] Download complete, creating extract...");

    // Create temporary extract
    auto extract_id = s->beatmap_downloader.create_extract(beatmapset_id);
    if (!extract_id) {
        spdlog::error("[!song] Failed to create extract for beatmapset {}", beatmapset_id);
        event.reply(s->message_presenter.build_error_message(error_messages::EXTRACT_FAILED));
        return;
    }

    // Find audio file in extract
    auto extract_path = s->beatmap_downloader.get_extract_path(*extract_id);
    if (!extract_path) {
        event.reply(s->message_presenter.build_error_message(error_messages::EXTRACT_NOT_FOUND));
        return;
    }

    auto audio_filename = s->beatmap_downloader.find_audio_in_extract(*extract_path);
    if (!audio_filename) {
        event.reply(s->message_presenter.build_error_message(error_messages::NO_AUDIO));
        return;
    }

    // Read audio file into memory
    std::string audio_path = (*extract_path / *audio_filename).string();
    std::ifstream audio_file(audio_path, std::ios::binary);
    if (!audio_file) {
        spdlog::error("[!song] Failed to open audio file: {}", audio_path);
        event.reply(s->message_presenter.build_error_message(error_messages::NO_AUDIO));
        return;
    }

    std::string audio_data((std::istreambuf_iterator<char>(audio_file)),
                            std::istreambuf_iterator<char>());
    audio_file.close();

    // Determine file size limit based on server boost level
    size_t max_file_size = 10 * 1024 * 1024;  // Default: 10MB (tier 0-1)
    dpp::guild* guild = dpp::find_guild(event.msg.guild_id);
    if (guild) {
        switch (guild->premium_tier) {
            case dpp::tier_2:
                max_file_size = 50 * 1024 * 1024;   // 50MB
                break;
            case dpp::tier_3:
                max_file_size = 100 * 1024 * 1024;  // 100MB
                break;
            default:
                break;
        }
    }

    // Check file size against server limit
    if (audio_data.size() > max_file_size) {
        spdlog::warn("[!song] Audio file too large: {} bytes (limit: {} bytes), falling back to URL",
                     audio_data.size(), max_file_size);
        // Fall back to URL
        std::string audio_url = fmt::format("{}/osu/{}/{}",
            s->config.public_url, *extract_id, utils::url_encode(*audio_filename));

        // Build footer with size info and required tier
        std::string footer_text = s->beatmap_downloader.build_download_footer(beatmapset_id);
        double file_mb = static_cast<double>(audio_data.size()) / (1024 * 1024);
        std::string required_tier;
        if (audio_data.size() <= 50 * 1024 * 1024) {
            required_tier = "Tier 2";
        } else if (audio_data.size() <= 100 * 1024 * 1024) {
            required_tier = "Tier 3";
        } else {
            required_tier = ">100MB";
        }
        footer_text += fmt::format(" | File: {:.1f}MB (needs {})", file_mb, required_tier);

        event.reply(s->message_presenter.build_audio(beatmap, audio_url, footer_text));
        return;
    }

    // Create friendly filename: "artist - title.ext"
    std::string extension = std::filesystem::path(*audio_filename).extension().string();
    std::string friendly_filename = fmt::format("{} - {}{}", beatmap.get_artist(), beatmap.get_title(), extension);

    // Create message with just the audio file attachment (no embed)
    dpp::message msg;
    msg.add_file(friendly_filename, audio_data);

    event.reply(msg);

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    if (elapsed > 8) {
        spdlog::warn("[CMD] !song took {}s to complete (slow download or API response)", elapsed);
    }
}

} // namespace commands
