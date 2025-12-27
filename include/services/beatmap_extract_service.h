#pragma once

#include <optional>
#include <string>
#include <filesystem>
#include <dpp/dpp.h>
#include <osu.h>

class BeatmapDownloader;
class Request;

namespace services {

class ChatContextService;
class BeatmapResolverService;
class MessagePresenterService;

/**
 * Result of beatmap extraction process
 */
struct BeatmapExtractResult {
    bool success = false;
    std::string error_message;

    // Available on success:
    Beatmap beatmap;
    uint32_t beatmapset_id = 0;
    std::string extract_id;
    std::filesystem::path extract_path;
};

/**
 * Service for extracting beatmap files from .osz archives.
 * Used by commands that need access to beatmap files (bg, audio, etc.)
 */
class BeatmapExtractService {
public:
    BeatmapExtractService(
        BeatmapDownloader& downloader,
        Request& request,
        ChatContextService& chat_context,
        BeatmapResolverService& beatmap_resolver,
        MessagePresenterService& message_presenter
    );

    /**
     * Extract beatmap files for the current channel context.
     * Downloads .osz if needed, extracts to temp directory.
     *
     * @param channel_id Channel to get beatmap context from
     * @return Result with extract path and beatmap info, or error
     */
    BeatmapExtractResult extract_for_channel(dpp::snowflake channel_id);

private:
    BeatmapDownloader& downloader_;
    Request& request_;
    ChatContextService& chat_context_;
    BeatmapResolverService& beatmap_resolver_;
    MessagePresenterService& message_presenter_;
};

} // namespace services
