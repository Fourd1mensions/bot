#include "commands/audio_command.h"
#include "services/service_container.h"
#include "services/beatmap_extract_service.h"
#include "services/message_presenter_service.h"
#include <beatmap_downloader.h>
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

void AudioCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[audio] ServiceContainer is null");
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

    // Find audio file in extract
    auto audio_filename = s->beatmap_downloader.find_audio_in_extract(result.extract_path);
    if (!audio_filename) {
        ctx.reply(s->message_presenter.build_error_message(error_messages::NO_AUDIO));
        return;
    }

    // Read audio file into memory
    std::string audio_path = (result.extract_path / *audio_filename).string();
    std::ifstream audio_file(audio_path, std::ios::binary);
    if (!audio_file) {
        spdlog::error("[audio] Failed to open audio file: {}", audio_path);
        ctx.reply(s->message_presenter.build_error_message(error_messages::NO_AUDIO));
        return;
    }

    std::string audio_data((std::istreambuf_iterator<char>(audio_file)),
                            std::istreambuf_iterator<char>());
    audio_file.close();

    // Determine file size limit based on server boost level
    size_t max_file_size = 10 * 1024 * 1024;  // Default: 10MB (tier 0-1)
    dpp::guild* guild = dpp::find_guild(ctx.guild_id());
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
        spdlog::warn("[audio] Audio file too large: {} bytes (limit: {} bytes), falling back to URL",
                     audio_data.size(), max_file_size);
        // Fall back to URL
        std::string audio_url = fmt::format("{}/osu/{}/{}",
            s->config.public_url, result.extract_id, utils::url_encode(*audio_filename));

        // Build footer with size info and required tier
        std::string footer_text = s->beatmap_downloader.build_download_footer(result.beatmapset_id);
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

        ctx.reply(s->message_presenter.build_audio(result.beatmap, audio_url, footer_text));
        return;
    }

    // Create friendly filename: "artist - title.ext"
    std::string extension = std::filesystem::path(*audio_filename).extension().string();
    std::string friendly_filename = utils::sanitize_filename(
        fmt::format("{} - {}{}", result.beatmap.get_artist(), result.beatmap.get_title(), extension));

    spdlog::info("[audio] Original audio file: {}, extension: '{}', friendly name: '{}'",
                 *audio_filename, extension, friendly_filename);

    // Create message with just the audio file attachment (no embed)
    dpp::message msg;
    msg.add_file(friendly_filename, audio_data);

    ctx.reply(msg);
}

} // namespace commands
