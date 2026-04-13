#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace services {

/**
 * Parsed parameters for recent scores command (!rs, !recent, etc.)
 */
struct RecentScoreParams {
    std::string username;           // Target username (empty = caller)
    std::string mode = "osu";       // Game mode
    size_t score_index = 0;         // Score index (0-based)
    bool include_fails = true;      // Include failed scores
    bool use_best_scores = false;   // Use top plays instead of recent

    // Validation
    std::vector<std::string> warnings;  // Non-fatal issues (unknown flags, etc.)
    std::vector<std::string> errors;    // Fatal issues (invalid required params)

    bool has_errors() const { return !errors.empty(); }
    bool has_warnings() const { return !warnings.empty(); }
};

/**
 * Parsed parameters for compare command (!c, !compare)
 */
struct CompareParams {
    std::string username;           // Target username (empty = caller)
    std::string mods_filter;        // Mods filter (e.g., "HDDT")

    // Validation
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    bool has_errors() const { return !errors.empty(); }
    bool has_warnings() const { return !warnings.empty(); }
};

/**
 * Service for parsing command parameters.
 */
class CommandParamsService {
public:
    CommandParamsService() = default;
    ~CommandParamsService() = default;

    /**
     * Tokenize a parameter string into words.
     */
    static std::vector<std::string> tokenize(const std::string& params);

    /**
     * Parse parameters for recent scores command.
     * Supports: [username] [-p] [-b] [-i index] [-m mode]
     * @param params Raw parameter string
     * @param default_mode Default game mode from command
     * @return Parsed parameters
     */
    RecentScoreParams parse_recent_params(
        const std::string& params,
        const std::string& default_mode = "osu"
    ) const;

    /**
     * Parse parameters for compare command.
     * Supports: [username] [+mods]
     * @param params Raw parameter string
     * @return Parsed parameters
     */
    CompareParams parse_compare_params(const std::string& params) const;

    /**
     * Parse a Discord mention and extract user ID.
     * Handles formats: <@123456>, <@!123456>
     * @param mention The mention string
     * @return User ID string or nullopt if not a valid mention
     */
    static std::optional<std::string> parse_discord_mention(const std::string& mention);

    /**
     * Normalize game mode string to API format.
     * Handles aliases like "std", "ctb", "fruits"
     * @param mode_input User-provided mode string
     * @return Normalized mode ("osu", "taiko", "fruits", "mania") or nullopt
     */
    static std::optional<std::string> normalize_mode(const std::string& mode_input);

    /**
     * Join username parts into a single string.
     */
    static std::string join_username_parts(const std::vector<std::string>& parts);
};

} // namespace services
