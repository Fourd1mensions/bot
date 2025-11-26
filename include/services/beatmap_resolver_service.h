#pragma once

#include <string>

// Forward declaration
class Request;

namespace services {

/**
 * Resolves beatmap IDs from various formats (set IDs, direct IDs, etc.).
 */
class BeatmapResolverService {
public:
    explicit BeatmapResolverService(Request& request);
    ~BeatmapResolverService() = default;

    // Disable copy and move
    BeatmapResolverService(const BeatmapResolverService&) = delete;
    BeatmapResolverService& operator=(const BeatmapResolverService&) = delete;

    /**
     * Resolve a stored beatmap ID (which might be a set: prefix or direct ID).
     * @param stored_id The stored ID from chat context (e.g., "set:123456" or "987654")
     * @return The actual beatmap ID, or empty string if resolution failed
     */
    std::string resolve_beatmap_id(const std::string& stored_id);

private:
    Request& request_;
};

} // namespace services
