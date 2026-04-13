#include "services/beatmap_extract_service.h"
#include "services/chat_context_service.h"
#include "services/beatmap_resolver_service.h"
#include "services/message_presenter_service.h"
#include <beatmap_downloader.h>
#include <requests.h>
#include <error_messages.h>
#include <spdlog/spdlog.h>

namespace services {

BeatmapExtractService::BeatmapExtractService(
    BeatmapDownloader& downloader,
    Request& request,
    ChatContextService& chat_context,
    BeatmapResolverService& beatmap_resolver,
    MessagePresenterService& message_presenter
)
    : downloader_(downloader)
    , request_(request)
    , chat_context_(chat_context)
    , beatmap_resolver_(beatmap_resolver)
    , message_presenter_(message_presenter)
{}

BeatmapExtractResult BeatmapExtractService::extract_for_channel(dpp::snowflake channel_id) {
    BeatmapExtractResult result;

    // Resolve beatmap from channel context
    std::string stored_id = chat_context_.get_beatmap_id(channel_id);
    std::string beatmap_id = beatmap_resolver_.resolve_beatmap_id(stored_id);

    if (beatmap_id.empty()) {
        result.error_message = std::string(error_messages::NO_BEATMAP_IN_CHANNEL);
        return result;
    }

    // Fetch beatmap from API
    std::string response_beatmap = request_.get_beatmap(beatmap_id);
    if (response_beatmap.empty()) {
        result.error_message = std::string(error_messages::API_NO_RESPONSE);
        return result;
    }

    result.beatmap = Beatmap(response_beatmap);
    result.beatmapset_id = result.beatmap.get_beatmapset_id();

    spdlog::debug("[BeatmapExtract] Processing beatmapset_id: {}", result.beatmapset_id);

    // Download .osz file if needed
    if (!downloader_.download_osz(result.beatmapset_id)) {
        spdlog::error("[BeatmapExtract] download_osz failed for beatmapset {}", result.beatmapset_id);
        result.error_message = std::string(error_messages::DOWNLOAD_FAILED);
        return result;
    }

    // Create temporary extract
    auto extract_id_opt = downloader_.create_extract(result.beatmapset_id);
    if (!extract_id_opt) {
        spdlog::error("[BeatmapExtract] Failed to create extract for beatmapset {}", result.beatmapset_id);
        result.error_message = std::string(error_messages::EXTRACT_FAILED);
        return result;
    }
    result.extract_id = *extract_id_opt;

    // Get extract path
    auto extract_path_opt = downloader_.get_extract_path(result.extract_id);
    if (!extract_path_opt) {
        result.error_message = std::string(error_messages::EXTRACT_NOT_FOUND);
        return result;
    }
    result.extract_path = *extract_path_opt;

    result.success = true;
    return result;
}

} // namespace services
