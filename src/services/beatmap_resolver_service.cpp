#include "services/beatmap_resolver_service.h"
#include "requests.h"
#include <spdlog/spdlog.h>

namespace services {

BeatmapResolverService::BeatmapResolverService(Request& request)
    : request_(request) {
}

std::string BeatmapResolverService::resolve_beatmap_id(const std::string& stored_id) {
    constexpr std::string_view set_prefix = "set:";
    if (stored_id.starts_with(set_prefix)) {
        std::string set_id = stored_id.substr(set_prefix.size());
        std::string beatmap_id = request_.get_beatmap_id_from_set(set_id);
        if (beatmap_id.empty()) {
            spdlog::error("Failed to resolve beatmap from set {}", set_id);
        }
        return beatmap_id;
    }
    return stored_id;
}

BeatmapResolveResult BeatmapResolverService::resolve(const std::string& stored_id) {
    BeatmapResolveResult result;

    if (stored_id.empty()) {
        result.error_message = "No beatmap found in this channel. Please send a beatmap link first.";
        return result;
    }

    constexpr std::string_view set_prefix = "set:";
    if (stored_id.starts_with(set_prefix)) {
        // Extract beatmapset_id from stored value
        result.beatmapset_id = std::stoul(stored_id.substr(set_prefix.size()));

        // Get beatmap_id from API
        std::string beatmap_id_str = request_.get_beatmap_id_from_set(
            std::to_string(result.beatmapset_id));

        if (beatmap_id_str.empty()) {
            result.error_message = "Failed to resolve beatmap ID.";
            result.beatmapset_id = 0;
            return result;
        }

        result.beatmap_id = std::stoul(beatmap_id_str);
        spdlog::debug("[BeatmapResolver] Resolved set:{} -> beatmap:{}",
            result.beatmapset_id, result.beatmap_id);
    } else {
        // Direct beatmap_id
        result.beatmap_id = std::stoul(stored_id);
        // beatmapset_id will be fetched later if needed
    }

    return result;
}

} // namespace services
