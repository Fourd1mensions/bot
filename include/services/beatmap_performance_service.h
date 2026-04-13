#pragma once

#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

// Forward declarations
class BeatmapDownloader;

namespace services {

class BeatmapCacheService;

#ifdef USE_ROSU_PP_SERVICE
class RosuPpClient;
#endif

/**
 * Difficulty attributes for a beatmap with mods applied.
 */
struct BeatmapDifficultyAttrs {
    float approach_rate = 9.0f;
    float overall_difficulty = 9.0f;
    float circle_size = 5.0f;
    float hp_drain_rate = 5.0f;
    int total_objects = 0;
    double star_rating = 0.0;
    double aim_difficulty = 0.0;
    double speed_difficulty = 0.0;
    int max_combo = 0;
};

/**
 * Performance calculation result.
 */
struct PerformanceAttrs {
    double pp = 0.0;
    double aim_pp = 0.0;
    double speed_pp = 0.0;
    double accuracy_pp = 0.0;
};

/**
 * Parameters for PP simulation.
 */
struct SimulateParams {
    double accuracy = 1.0;
    std::string mods;
    int combo = 0;          // 0 = use beatmap max
    int misses = 0;
    int count_300 = -1;     // -1 = auto
    int count_100 = -1;     // -1 = auto
    int count_50 = -1;      // -1 = auto
    int passed_objects = 0; // 0 = full map, >0 = partial (for failed scores)
    bool lazer = false;     // Use stable PP calculation by default (CL mod = stable-like)
};

/**
 * Service for beatmap performance calculations.
 * Encapsulates .osu file loading, parsing, and PP calculation.
 *
 * When USE_ROSU_PP_SERVICE is defined, uses rosu-pp-service via gRPC
 * for high-performance calculations. Falls back to osu-tools otherwise.
 */
class BeatmapPerformanceService {
public:
    /**
     * Construct service with beatmap downloader.
     * @param downloader Reference to beatmap downloader
     * @param cache_service Optional cache service for background .osz downloads
     * @param rosu_pp_address Optional rosu-pp-service address (default: localhost:50051)
     */
    explicit BeatmapPerformanceService(
        BeatmapDownloader& downloader,
        BeatmapCacheService* cache_service = nullptr,
        const std::string& rosu_pp_address = "localhost:50051"
    );
    ~BeatmapPerformanceService();

    // Disable copy
    BeatmapPerformanceService(const BeatmapPerformanceService&) = delete;
    BeatmapPerformanceService& operator=(const BeatmapPerformanceService&) = delete;

    /**
     * Check if rosu-pp-service is available.
     */
    [[nodiscard]] bool is_rosu_pp_available() const;

    /**
     * Set the cache service for background .osz downloads.
     */
    void set_cache_service(BeatmapCacheService* cache_service);

    /**
     * Get path to .osu file for a beatmap via .osz extraction.
     * Downloads and extracts the full beatmapset if necessary.
     * @param beatmapset_id The beatmapset ID
     * @param beatmap_id The specific beatmap ID
     * @return Path to .osu file or nullopt on failure
     */
    std::optional<std::string> get_osu_file_path(uint32_t beatmapset_id, uint32_t beatmap_id);

    /**
     * Get path to .osu file by downloading it directly.
     * Faster than extracting from .osz but requires beatmap_id.
     * @param beatmap_id The specific beatmap ID
     * @return Path to .osu file or nullopt on failure
     */
    std::optional<std::string> get_osu_file_direct(uint32_t beatmap_id);

    /**
     * Get difficulty attributes for a beatmap with mods.
     * @param beatmapset_id The beatmapset ID
     * @param beatmap_id The specific beatmap ID
     * @param mods Mod string (e.g., "HDDT")
     * @return Difficulty attributes or nullopt on failure
     */
    std::optional<BeatmapDifficultyAttrs> get_difficulty(
        uint32_t beatmapset_id,
        uint32_t beatmap_id,
        const std::string& mods = ""
    );

    /**
     * Calculate PP for a score.
     * @param beatmapset_id The beatmapset ID
     * @param beatmap_id The specific beatmap ID
     * @param mode Game mode ("osu", "taiko", "catch", "mania")
     * @param params Simulation parameters
     * @return Performance attributes or nullopt on failure
     */
    std::optional<PerformanceAttrs> calculate_pp(
        uint32_t beatmapset_id,
        uint32_t beatmap_id,
        const std::string& mode,
        const SimulateParams& params
    );

    /**
     * Calculate PP at multiple accuracy levels (for !map command).
     * Uses direct .osu download (faster).
     * @param beatmap_id The specific beatmap ID
     * @param mode Game mode
     * @param mods Mod string
     * @param accuracy_levels Vector of accuracy values (0.0-1.0)
     * @param out_difficulty Optional output for difficulty attrs from first calc
     * @return Vector of PP values, or empty on failure
     */
    std::vector<double> calculate_pp_at_accuracies(
        uint32_t beatmap_id,
        const std::string& mode,
        const std::string& mods,
        const std::vector<double>& accuracy_levels,
        BeatmapDifficultyAttrs* out_difficulty = nullptr
    );

    /**
     * Get difficulty attributes using direct .osu download.
     * @param beatmap_id The specific beatmap ID
     * @param mods Mod string (e.g., "HDDT")
     * @return Difficulty attributes or nullopt on failure
     */
    std::optional<BeatmapDifficultyAttrs> get_difficulty_direct(
        uint32_t beatmap_id,
        const std::string& mods = ""
    );

    /**
     * Generate a strain graph PNG for a beatmap.
     * @param beatmap_id The specific beatmap ID
     * @param mods Mod string (e.g., "HDDT")
     * @param width Graph width in pixels (0 = default)
     * @param height Graph height in pixels (0 = default)
     * @return PNG image bytes or nullopt on failure
     */
    std::optional<std::vector<uint8_t>> get_strain_graph(
        uint32_t beatmap_id,
        const std::string& mods = "",
        uint32_t width = 0,
        uint32_t height = 0
    );

private:
    BeatmapDownloader& downloader_;
    BeatmapCacheService* cache_service_;

#ifdef USE_ROSU_PP_SERVICE
    std::unique_ptr<RosuPpClient> rosu_pp_client_;

    // Internal helpers for rosu-pp-service
    std::optional<PerformanceAttrs> calculate_pp_rosu(
        const std::string& osu_path,
        const std::string& mode,
        const SimulateParams& params
    );

    std::vector<double> calculate_pp_at_accuracies_rosu(
        const std::string& osu_path,
        const std::string& mode,
        const std::string& mods,
        const std::vector<double>& accuracy_levels,
        BeatmapDifficultyAttrs* out_difficulty
    );
#endif
};

} // namespace services
