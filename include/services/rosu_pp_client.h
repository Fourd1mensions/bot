#pragma once

#ifdef USE_ROSU_PP_SERVICE

#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Forward declarations for gRPC types
namespace grpc {
class Channel;
}
namespace pp::v1 {
class PerformanceService;
}

namespace services {

/**
 * Difficulty attributes from rosu-pp-service.
 */
struct RosuDifficultyAttrs {
    double stars = 0.0;
    uint32_t max_combo = 0;

    // osu! specific (post-mod values)
    double aim = 0.0;
    double speed = 0.0;
    double flashlight = 0.0;
    double ar = 0.0;   // Approach rate (post-mod)
    double od = 0.0;   // Overall difficulty (post-mod)
    double hp = 0.0;   // HP drain (post-mod)
    double cs = 0.0;   // Circle size (post-mod)
    uint32_t n_circles = 0;
    uint32_t n_sliders = 0;
    uint32_t n_spinners = 0;
};

/**
 * Performance attributes from rosu-pp-service.
 */
struct RosuPerformanceAttrs {
    double pp = 0.0;
    double stars = 0.0;
    uint32_t max_combo = 0;

    // osu! specific PP breakdown
    double pp_aim = 0.0;
    double pp_speed = 0.0;
    double pp_acc = 0.0;
    double pp_flashlight = 0.0;
    double effective_miss_count = 0.0;

    // Difficulty attributes
    RosuDifficultyAttrs difficulty;
};

/**
 * Score parameters for PP calculation.
 */
struct RosuScoreParams {
    std::optional<uint32_t> combo;
    std::optional<double> accuracy;    // 0-100
    std::optional<uint32_t> misses;
    std::optional<uint32_t> n300;
    std::optional<uint32_t> n100;
    std::optional<uint32_t> n50;
    std::optional<uint32_t> n_geki;
    std::optional<uint32_t> n_katu;
};

/**
 * Difficulty settings (mods, custom values).
 */
struct RosuDifficultySettings {
    uint32_t mods = 0;                 // Mod bitflags (legacy)
    std::string mods_str;              // Acronym-based mods (e.g., "HDDT", "NFCL") - preferred
    std::optional<double> clock_rate;
    std::optional<float> ar;
    std::optional<float> cs;
    std::optional<float> od;
    std::optional<float> hp;
    std::optional<uint32_t> passed_objects;  // For failed scores: number of objects played
    bool lazer = false;
};

/**
 * Game mode enum matching rosu-pp-service.
 */
enum class RosuGameMode {
    Osu = 1,
    Taiko = 2,
    Catch = 3,
    Mania = 4
};

/**
 * gRPC client for rosu-pp-service.
 * Provides high-performance PP calculation via rosu-pp Rust library.
 */
class RosuPpClient {
public:
    /**
     * Create client connected to specified address.
     * @param address Server address (e.g., "localhost:50051")
     */
    explicit RosuPpClient(const std::string& address = "localhost:50051");
    ~RosuPpClient();

    // Disable copy, allow move
    RosuPpClient(const RosuPpClient&) = delete;
    RosuPpClient& operator=(const RosuPpClient&) = delete;
    RosuPpClient(RosuPpClient&&) noexcept;
    RosuPpClient& operator=(RosuPpClient&&) noexcept;

    /**
     * Check if connected to the server.
     */
    [[nodiscard]] bool is_connected() const;

    /**
     * Calculate difficulty (star rating) for a beatmap.
     * @param osu_file_path Path to .osu file
     * @param mode Game mode (nullopt = use beatmap's mode)
     * @param settings Difficulty settings (mods, etc.)
     * @return Difficulty attributes or nullopt on failure
     */
    [[nodiscard]] std::optional<RosuDifficultyAttrs> calculate_difficulty(
        const std::string& osu_file_path,
        std::optional<RosuGameMode> mode = std::nullopt,
        const RosuDifficultySettings& settings = {}
    );

    /**
     * Calculate difficulty from beatmap bytes.
     */
    [[nodiscard]] std::optional<RosuDifficultyAttrs> calculate_difficulty_bytes(
        const std::vector<uint8_t>& content,
        std::optional<RosuGameMode> mode = std::nullopt,
        const RosuDifficultySettings& settings = {}
    );

    /**
     * Calculate PP for a score.
     * @param osu_file_path Path to .osu file
     * @param mode Game mode
     * @param settings Difficulty settings
     * @param score Score parameters
     * @return Performance attributes or nullopt on failure
     */
    [[nodiscard]] std::optional<RosuPerformanceAttrs> calculate_performance(
        const std::string& osu_file_path,
        std::optional<RosuGameMode> mode,
        const RosuDifficultySettings& settings,
        const RosuScoreParams& score
    );

    /**
     * Calculate PP from beatmap bytes.
     */
    [[nodiscard]] std::optional<RosuPerformanceAttrs> calculate_performance_bytes(
        const std::vector<uint8_t>& content,
        std::optional<RosuGameMode> mode,
        const RosuDifficultySettings& settings,
        const RosuScoreParams& score
    );

    /**
     * Batch calculate PP for multiple scores (same beatmap, different params).
     * More efficient than calling calculate_performance multiple times.
     */
    [[nodiscard]] std::vector<RosuPerformanceAttrs> calculate_batch(
        const std::string& osu_file_path,
        std::optional<RosuGameMode> mode,
        const RosuDifficultySettings& settings,
        const std::vector<RosuScoreParams>& scores
    );

    /**
     * Generate a strain graph PNG for a beatmap.
     * @param osu_file_path Path to .osu file
     * @param mode Game mode (nullopt = use beatmap's mode)
     * @param settings Difficulty settings (mods, etc.)
     * @param width Graph width in pixels (0 = default 900)
     * @param height Graph height in pixels (0 = default 250)
     * @return PNG image bytes or nullopt on failure
     */
    [[nodiscard]] std::optional<std::vector<uint8_t>> get_strain_graph(
        const std::string& osu_file_path,
        std::optional<RosuGameMode> mode = std::nullopt,
        const RosuDifficultySettings& settings = {},
        uint32_t width = 0,
        uint32_t height = 0
    );

    /**
     * Generate a strain graph PNG from beatmap bytes.
     */
    [[nodiscard]] std::optional<std::vector<uint8_t>> get_strain_graph_bytes(
        const std::vector<uint8_t>& content,
        std::optional<RosuGameMode> mode = std::nullopt,
        const RosuDifficultySettings& settings = {},
        uint32_t width = 0,
        uint32_t height = 0
    );

    /**
     * Parse mod string to bitflags.
     * @param mods Mod string (e.g., "HDDT", "HRFL")
     * @return Mod bitflags
     */
    [[nodiscard]] static uint32_t parse_mods(const std::string& mods);

    /**
     * Convert game mode string to enum.
     */
    [[nodiscard]] static std::optional<RosuGameMode> parse_mode(const std::string& mode);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace services

#endif // USE_ROSU_PP_SERVICE
