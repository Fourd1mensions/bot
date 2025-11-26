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

} // namespace services
