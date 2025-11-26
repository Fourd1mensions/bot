#pragma once

#include "types.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

namespace osupp {

namespace detail {

inline std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

inline std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(str);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

inline PathType parse_path_type(char c) {
    switch (c) {
        case 'L': return PathType::Linear;
        case 'P': return PathType::Perfect;
        case 'B': return PathType::Bezier;
        case 'C': return PathType::Catmull;
        default: return PathType::Linear;
    }
}

inline HitObject parse_hit_object(const std::string& line) {
    HitObject obj;
    auto parts = split(line, ',');

    if (parts.size() < 5) return obj;

    // Parse position and time
    obj.position.x = std::stod(parts[0]);
    obj.position.y = std::stod(parts[1]);
    obj.start_time = std::stod(parts[2]);

    // Parse type flags
    int type_flags = std::stoi(parts[3]);
    obj.new_combo = (type_flags & 4) != 0;
    obj.combo_offset = (type_flags >> 4) & 7;

    // Determine object type
    if (type_flags & 1) {  // Circle
        obj.type = HitObjectType::Circle;
    } else if (type_flags & 2) {  // Slider
        obj.type = HitObjectType::Slider;

        if (parts.size() >= 7) {
            // Parse curve type and points
            auto curve_parts = split(parts[5], '|');
            if (!curve_parts.empty()) {
                obj.path_type = parse_path_type(curve_parts[0][0]);

                // Parse control points (first point is the slider start)
                obj.control_points.push_back(obj.position);

                for (size_t i = 1; i < curve_parts.size(); i++) {
                    auto coords = split(curve_parts[i], ':');
                    if (coords.size() >= 2) {
                        Vector2 point;
                        point.x = std::stod(coords[0]);
                        point.y = std::stod(coords[1]);
                        obj.control_points.push_back(point);
                    }
                }
            }

            // Parse slides (repeat count)
            obj.repeat_count = std::stoi(parts[6]);

            // Parse pixel length
            if (parts.size() >= 8) {
                obj.pixel_length = std::stod(parts[7]);
            }
        }
    } else if (type_flags & 8) {  // Spinner
        obj.type = HitObjectType::Spinner;
        if (parts.size() >= 6) {
            obj.end_time = std::stod(parts[5]);
        }
    }

    return obj;
}

inline TimingPoint parse_timing_point(const std::string& line) {
    TimingPoint tp;
    auto parts = split(line, ',');

    if (parts.size() < 2) return tp;

    tp.time = std::stod(parts[0]);
    tp.beat_length = std::stod(parts[1]);

    if (parts.size() >= 3) {
        tp.time_signature = std::stoi(parts[2]);
    }

    if (parts.size() >= 7) {
        tp.uninherited = std::stoi(parts[6]) == 1;
    } else {
        // Assume uninherited if beat_length is positive
        tp.uninherited = tp.beat_length > 0;
    }

    return tp;
}

} // namespace detail

inline Beatmap parse_beatmap(const std::string& filepath) {
    Beatmap beatmap;
    std::ifstream file(filepath);

    if (!file.is_open()) {
        return beatmap;
    }

    std::string line;
    std::string current_section;

    while (std::getline(file, line)) {
        line = detail::trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '/' || line[0] == ' ') {
            continue;
        }

        // Check for section headers
        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.length() - 2);
            continue;
        }

        // Parse based on current section
        if (current_section == "General") {
            auto colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = detail::trim(line.substr(0, colon_pos));
                std::string value = detail::trim(line.substr(colon_pos + 1));

                if (key == "Mode") {
                    beatmap.mode = std::stoi(value);
                }
            }
        }
        else if (current_section == "Metadata") {
            auto colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = detail::trim(line.substr(0, colon_pos));
                std::string value = detail::trim(line.substr(colon_pos + 1));

                if (key == "Title") beatmap.title = value;
                else if (key == "Artist") beatmap.artist = value;
                else if (key == "Creator") beatmap.creator = value;
                else if (key == "Version") beatmap.version = value;
            }
        }
        else if (current_section == "Difficulty") {
            auto colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = detail::trim(line.substr(0, colon_pos));
                std::string value = detail::trim(line.substr(colon_pos + 1));

                if (key == "HPDrainRate") {
                    beatmap.difficulty.hp_drain_rate = std::stod(value);
                }
                else if (key == "CircleSize") {
                    beatmap.difficulty.circle_size = std::stod(value);
                }
                else if (key == "OverallDifficulty") {
                    beatmap.difficulty.overall_difficulty = std::stod(value);
                }
                else if (key == "ApproachRate") {
                    beatmap.difficulty.approach_rate = std::stod(value);
                }
                else if (key == "SliderMultiplier") {
                    beatmap.difficulty.slider_multiplier = std::stod(value);
                }
                else if (key == "SliderTickRate") {
                    beatmap.difficulty.slider_tick_rate = std::stod(value);
                }
            }
        }
        else if (current_section == "TimingPoints") {
            beatmap.timing_points.push_back(detail::parse_timing_point(line));
        }
        else if (current_section == "HitObjects") {
            beatmap.hit_objects.push_back(detail::parse_hit_object(line));
        }
    }

    // Handle old beatmap format (v6 and earlier): if AR was not specified, it should equal OD
    // We detect this by checking if AR is still at default value (5.0) while OD is different
    // This is a heuristic, but works for most old maps
    if (beatmap.difficulty.approach_rate == 5.0 &&
        beatmap.difficulty.overall_difficulty != 5.0) {
        beatmap.difficulty.approach_rate = beatmap.difficulty.overall_difficulty;
        spdlog::debug("[BEATMAP_PARSER] Old format detected: setting AR = OD = {}",
                      beatmap.difficulty.approach_rate);
    }

    // Calculate max combo
    beatmap.max_combo = 0;
    for (const auto& obj : beatmap.hit_objects) {
        if (obj.type == HitObjectType::Circle) {
            beatmap.max_combo++;
        } else if (obj.type == HitObjectType::Slider) {
            // Slider head + repeat points + slider end
            beatmap.max_combo += obj.repeat_count + 1;
        }
        // Spinners don't add to combo in standard mode
    }

    return beatmap;
}

// Parse beatmap from string content (useful for testing or direct API data)
inline Beatmap parse_beatmap_content(const std::string& content) {
    Beatmap beatmap;
    std::istringstream stream(content);
    std::string line;
    std::string current_section;

    while (std::getline(stream, line)) {
        line = detail::trim(line);

        if (line.empty() || line[0] == '/' || line[0] == ' ') {
            continue;
        }

        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.length() - 2);
            continue;
        }

        if (current_section == "General") {
            auto colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = detail::trim(line.substr(0, colon_pos));
                std::string value = detail::trim(line.substr(colon_pos + 1));

                if (key == "Mode") {
                    beatmap.mode = std::stoi(value);
                }
            }
        }
        else if (current_section == "Metadata") {
            auto colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = detail::trim(line.substr(0, colon_pos));
                std::string value = detail::trim(line.substr(colon_pos + 1));

                if (key == "Title") beatmap.title = value;
                else if (key == "Artist") beatmap.artist = value;
                else if (key == "Creator") beatmap.creator = value;
                else if (key == "Version") beatmap.version = value;
            }
        }
        else if (current_section == "Difficulty") {
            auto colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = detail::trim(line.substr(0, colon_pos));
                std::string value = detail::trim(line.substr(colon_pos + 1));

                if (key == "HPDrainRate") {
                    beatmap.difficulty.hp_drain_rate = std::stod(value);
                }
                else if (key == "CircleSize") {
                    beatmap.difficulty.circle_size = std::stod(value);
                }
                else if (key == "OverallDifficulty") {
                    beatmap.difficulty.overall_difficulty = std::stod(value);
                }
                else if (key == "ApproachRate") {
                    beatmap.difficulty.approach_rate = std::stod(value);
                }
                else if (key == "SliderMultiplier") {
                    beatmap.difficulty.slider_multiplier = std::stod(value);
                }
                else if (key == "SliderTickRate") {
                    beatmap.difficulty.slider_tick_rate = std::stod(value);
                }
            }
        }
        else if (current_section == "TimingPoints") {
            beatmap.timing_points.push_back(detail::parse_timing_point(line));
        }
        else if (current_section == "HitObjects") {
            beatmap.hit_objects.push_back(detail::parse_hit_object(line));
        }
    }

    // Handle old beatmap format (v6 and earlier): if AR was not specified, it should equal OD
    // We detect this by checking if AR is still at default value (5.0) while OD is different
    // This is a heuristic, but works for most old maps
    if (beatmap.difficulty.approach_rate == 5.0 &&
        beatmap.difficulty.overall_difficulty != 5.0) {
        beatmap.difficulty.approach_rate = beatmap.difficulty.overall_difficulty;
        spdlog::debug("[BEATMAP_PARSER] Old format detected: setting AR = OD = {}",
                      beatmap.difficulty.approach_rate);
    }

    // Calculate max combo
    beatmap.max_combo = 0;
    for (const auto& obj : beatmap.hit_objects) {
        if (obj.type == HitObjectType::Circle) {
            beatmap.max_combo++;
        } else if (obj.type == HitObjectType::Slider) {
            beatmap.max_combo += obj.repeat_count + 1;
        }
    }

    return beatmap;
}

} // namespace osupp
