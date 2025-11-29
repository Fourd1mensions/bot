#pragma once

#include <vector>
#include <string>
#include <optional>
#include <dpp/dpp.h>

// Forward declarations
class Score;
class Beatmap;
struct LeaderboardState;
struct RecentScoreState;

namespace services {

/**
 * Data structure for presenting a score in leaderboard/recent views.
 * Separates presentation data from Score domain object.
 */
struct ScorePresentation {
    size_t rank;              // Position in leaderboard (1-indexed)
    std::string header;       // "username `123pp` +HDDT"
    std::string body;         // Score details (accuracy, combo, etc.)
    double display_pp;        // PP value to show (may be calculated for Loved maps)
};

/**
 * Difficulty information for embed display.
 */
struct DifficultyInfo {
    float approach_rate = 9.0f;
    float overall_difficulty = 9.0f;
    float circle_size = 5.0f;
    float hp_drain_rate = 5.0f;
    double star_rating = 0.0;
    double aim_difficulty = 0.0;
    double speed_difficulty = 0.0;
    int max_combo = 0;
    int total_objects = 0;
};

/**
 * PP calculation results for embed display.
 */
struct PPInfo {
    double current_pp = 0.0;      // Current score PP
    double fc_pp = 0.0;           // FC PP (if applicable)
    double fc_accuracy = 0.0;     // FC accuracy percentage (if applicable)
    bool has_fc_pp = false;       // Whether FC PP should be shown
};

/**
 * Pagination state for building navigation buttons.
 */
struct PaginationInfo {
    size_t current = 0;
    size_t total = 1;
    bool has_refresh = false;
    size_t refresh_count = 0;
};

/**
 * Service responsible for building Discord messages and embeds.
 * Separates presentation logic from business logic in Bot class.
 */
class MessagePresenterService {
public:
    MessagePresenterService() = default;
    ~MessagePresenterService() = default;

    // Disable copy
    MessagePresenterService(const MessagePresenterService&) = delete;
    MessagePresenterService& operator=(const MessagePresenterService&) = delete;

    /**
     * Build a leaderboard page embed with pagination buttons.
     * @param beatmap The beatmap being displayed
     * @param scores_on_page Scores to display on current page
     * @param footer_text Footer text (page info, filters, etc.)
     * @param mods_filter Active mods filter
     * @param total_pages Total number of pages
     * @param current_page Current page (0-indexed)
     * @return Complete Discord message with embed and components
     */
    dpp::message build_leaderboard_page(
        const Beatmap& beatmap,
        const std::vector<ScorePresentation>& scores_on_page,
        const std::string& footer_text,
        const std::string& mods_filter,
        size_t total_pages,
        size_t current_page
    ) const;

    /**
     * Build a recent score page embed with pagination buttons.
     * @param score The score to display
     * @param beatmap The beatmap for this score
     * @param difficulty Difficulty attributes (AR, OD, etc.)
     * @param pp_info PP breakdown (current PP and optional FC PP)
     * @param pagination Pagination state
     * @param score_type "recent" or "best"
     * @param completion_percent Map completion percentage (100.0 if FC)
     * @param modded_bpm BPM adjusted for speed mods
     * @param modded_length Length in seconds adjusted for speed mods
     * @return Complete Discord message with embed and components
     */
    dpp::message build_recent_score_page(
        const Score& score,
        const Beatmap& beatmap,
        const DifficultyInfo& difficulty,
        const PPInfo& pp_info,
        const PaginationInfo& pagination,
        const std::string& score_type,
        float completion_percent,
        float modded_bpm,
        uint32_t modded_length
    ) const;

    /**
     * Build a beatmap info embed (!map command).
     * @param beatmap The beatmap to display
     * @param difficulty Difficulty attributes
     * @param pp_values PP values for 90%, 95%, 99%, 100% accuracy
     * @param mods Active mods
     * @param beatmapset_id Beatmapset ID for download links
     * @param modded_bpm BPM adjusted for speed mods (DT/HT)
     * @param modded_length Length in seconds adjusted for speed mods
     * @return Complete Discord message with embed
     */
    dpp::message build_map_info(
        const Beatmap& beatmap,
        const DifficultyInfo& difficulty,
        const std::vector<double>& pp_values,
        const std::string& mods,
        uint32_t beatmapset_id,
        float modded_bpm,
        uint32_t modded_length
    ) const;

    /**
     * Build a background image embed (!bg command).
     * @param beatmap The beatmap
     * @param bg_url URL of the background image
     * @param source Source indicator (mirror name or "cached")
     * @return Complete Discord message with embed
     */
    dpp::message build_background(
        const Beatmap& beatmap,
        const std::string& bg_url,
        const std::string& source
    ) const;

    /**
     * Build pagination button row.
     * @param prefix Button ID prefix ("lb_" or "rs_")
     * @param current Current page/index (0-indexed)
     * @param total Total pages/items
     * @param has_refresh Whether to show refresh button at index 0
     * @return Action row component with navigation buttons
     */
    dpp::component build_pagination_row(
        const std::string& prefix,
        size_t current,
        size_t total,
        bool has_refresh = false
    ) const;

    /**
     * Get rank emoji for a given rank string.
     * @param rank Rank string (F, D, C, B, A, S, SH, X, XH)
     * @return Discord emoji string
     */
    std::string get_rank_emoji(const std::string& rank) const;

    /**
     * Get embed color based on star rating.
     * @param star_rating The star rating
     * @return Color value
     */
    uint32_t get_star_rating_color(double star_rating) const;

    /**
     * Data for caching recent score page content.
     */
    struct RecentScoreCacheData {
        std::string title;
        std::string url;
        std::string description;
        std::string thumbnail;
        std::string beatmap_info;
        std::string footer;
        time_t timestamp;
    };

    /**
     * Build cache data for recent score page (avoids duplicating presenter logic).
     * @return Structured data suitable for JSON serialization
     */
    RecentScoreCacheData build_recent_score_cache_data(
        const Score& score,
        const Beatmap& beatmap,
        const DifficultyInfo& difficulty,
        const PPInfo& pp_info,
        const PaginationInfo& pagination,
        const std::string& score_type,
        float completion_percent,
        float modded_bpm,
        uint32_t modded_length
    ) const;
};

} // namespace services
