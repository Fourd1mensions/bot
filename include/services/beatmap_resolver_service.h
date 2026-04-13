#pragma once

#include <string>
#include <cstdint>

// Forward declaration
class Request;

namespace services {

/**
 * Result of beatmap resolution with both IDs.
 */
struct BeatmapResolveResult {
    uint32_t beatmap_id = 0;
    uint32_t beatmapset_id = 0;
    std::string error_message;

    bool success() const { return beatmap_id != 0; }
    operator bool() const { return success(); }
};

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

    /**
     * Resolve stored value to both beatmap_id and beatmapset_id.
     * If stored_id is empty, returns error with "no beatmap" message.
     * @param stored_id The stored ID from chat context
     * @return Result with both IDs or error message
     */
    BeatmapResolveResult resolve(const std::string& stored_id);

private:
    Request& request_;
};

} // namespace services
