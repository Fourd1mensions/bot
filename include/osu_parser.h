#pragma once

#include <string>
#include <filesystem>
#include <optional>
#include <fstream>
#include <sstream>
#include <regex>

namespace fs = std::filesystem;

namespace osu_parser {

struct BeatmapDifficulty {
    float hp_drain_rate = 5.0f;
    float circle_size = 5.0f;
    float overall_difficulty = 5.0f;
    float approach_rate = 5.0f;
    float slider_multiplier = 1.4f;
    float slider_tick_rate = 1.0f;
    int beatmap_id = 0;
    int beatmapset_id = 0;
    int total_objects = 0;  // Total number of hit objects (circles + sliders + spinners)
};

// Find .osu file for a specific beatmap_id in an extract directory
inline std::optional<fs::path> find_osu_file(const fs::path& extract_path, int beatmap_id) {
    std::regex beatmap_id_regex(R"(BeatmapID:\s*(\d+))");

    for (const auto& entry : fs::directory_iterator(extract_path)) {
        if (entry.path().extension() != ".osu") continue;

        std::ifstream file(entry.path());
        std::string line;

        while (std::getline(file, line)) {
            std::smatch match;
            if (std::regex_search(line, match, beatmap_id_regex)) {
                int file_beatmap_id = std::stoi(match[1].str());
                if (file_beatmap_id == beatmap_id) {
                    return entry.path();
                }
                break; // Found BeatmapID line, no need to continue in this file
            }
        }
    }

    return std::nullopt;
}

// Parse difficulty attributes from .osu file
inline std::optional<BeatmapDifficulty> parse_osu_file(const fs::path& osu_file_path) {
    std::ifstream file(osu_file_path);
    if (!file.is_open()) {
        return std::nullopt;
    }

    BeatmapDifficulty diff;
    std::string line;
    bool in_difficulty = false;
    bool in_metadata = false;
    bool in_hitobjects = false;

    while (std::getline(file, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        // Check for section headers
        if (line == "[Difficulty]") {
            in_difficulty = true;
            in_metadata = false;
            in_hitobjects = false;
            continue;
        } else if (line == "[Metadata]") {
            in_metadata = true;
            in_difficulty = false;
            in_hitobjects = false;
            continue;
        } else if (line == "[HitObjects]") {
            in_hitobjects = true;
            in_metadata = false;
            in_difficulty = false;
            continue;
        } else if (!line.empty() && line[0] == '[') {
            in_difficulty = false;
            in_metadata = false;
            in_hitobjects = false;
            continue;
        }

        // Parse metadata
        if (in_metadata) {
            if (line.find("BeatmapID:") == 0) {
                diff.beatmap_id = std::stoi(line.substr(line.find(':') + 1));
            } else if (line.find("BeatmapSetID:") == 0) {
                diff.beatmapset_id = std::stoi(line.substr(line.find(':') + 1));
            }
        }

        // Parse difficulty attributes
        if (in_difficulty) {
            size_t colon_pos = line.find(':');
            if (colon_pos == std::string::npos) continue;

            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            try {
                if (key == "HPDrainRate") {
                    diff.hp_drain_rate = std::stof(value);
                } else if (key == "CircleSize") {
                    diff.circle_size = std::stof(value);
                } else if (key == "OverallDifficulty") {
                    diff.overall_difficulty = std::stof(value);
                } else if (key == "ApproachRate") {
                    diff.approach_rate = std::stof(value);
                } else if (key == "SliderMultiplier") {
                    diff.slider_multiplier = std::stof(value);
                } else if (key == "SliderTickRate") {
                    diff.slider_tick_rate = std::stof(value);
                }
            } catch (...) {
                // Ignore parse errors for individual values
            }
        }

        // Count hit objects
        if (in_hitobjects && !line.empty() && line[0] != '[') {
            diff.total_objects++;
        }
    }

    return diff;
}

// Convert AR to milliseconds (preempt time)
inline float ar_to_ms(float ar) {
    if (ar > 5.0f) {
        return 1200.0f - 150.0f * (ar - 5.0f);
    }
    return 1200.0f + 600.0f * (5.0f - ar);
}

// Convert milliseconds back to AR
inline float ms_to_ar(float ms) {
    if (ms < 1200.0f) {
        return (1200.0f - ms) / 150.0f + 5.0f;
    }
    return 5.0f - (ms - 1200.0f) / 600.0f;
}

// Convert OD to milliseconds (hit window for 300)
inline float od_to_ms(float od) {
    return 80.0f - 6.0f * od;
}

// Convert milliseconds back to OD
inline float ms_to_od(float ms) {
    return (80.0f - ms) / 6.0f;
}

// Apply mod adjustments to difficulty values
inline BeatmapDifficulty apply_mods(const BeatmapDifficulty& base_diff,
                                    bool has_easy, bool has_hard_rock,
                                    bool has_double_time, bool has_half_time) {
    BeatmapDifficulty modified = base_diff;

    // Easy mod: reduces AR, OD, CS, HP by 50%
    if (has_easy) {
        modified.approach_rate *= 0.5f;
        modified.overall_difficulty *= 0.5f;
        modified.circle_size *= 0.5f;
        modified.hp_drain_rate *= 0.5f;
    }

    // Hard Rock: increases AR, OD, CS, HP by 40%
    if (has_hard_rock) {
        modified.approach_rate = std::min(10.0f, modified.approach_rate * 1.4f);
        modified.overall_difficulty = std::min(10.0f, modified.overall_difficulty * 1.4f);
        modified.circle_size = std::min(10.0f, modified.circle_size * 1.4f);
        modified.hp_drain_rate = std::min(10.0f, modified.hp_drain_rate * 1.4f);
    }

    // DT/HT affect AR and OD through timing changes
    float speed_multiplier = 1.0f;
    if (has_double_time) {
        speed_multiplier = 1.5f;
    } else if (has_half_time) {
        speed_multiplier = 0.75f;
    }

    if (speed_multiplier != 1.0f) {
        // Apply speed changes to AR
        float ar_ms = ar_to_ms(modified.approach_rate);
        ar_ms /= speed_multiplier;
        modified.approach_rate = std::clamp(ms_to_ar(ar_ms), 0.0f, 11.0f);

        // Apply speed changes to OD
        float od_ms = od_to_ms(modified.overall_difficulty);
        od_ms /= speed_multiplier;
        modified.overall_difficulty = std::clamp(ms_to_od(od_ms), 0.0f, 11.0f);
    }

    return modified;
}

} // namespace osu_parser
