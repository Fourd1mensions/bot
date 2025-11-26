#pragma once

#include <cmath>
#include <vector>
#include <string>
#include <optional>

namespace osupp {

// 2D vector for positions
struct Vector2 {
    double x = 0.0;
    double y = 0.0;

    Vector2() = default;
    Vector2(double x, double y) : x(x), y(y) {}

    double length() const {
        return std::sqrt(x * x + y * y);
    }

    double distance(const Vector2& other) const {
        double dx = x - other.x;
        double dy = y - other.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    Vector2 operator-(const Vector2& other) const {
        return Vector2(x - other.x, y - other.y);
    }

    Vector2 operator+(const Vector2& other) const {
        return Vector2(x + other.x, y + other.y);
    }

    Vector2 operator*(double scalar) const {
        return Vector2(x * scalar, y * scalar);
    }

    Vector2 operator/(double scalar) const {
        return Vector2(x / scalar, y / scalar);
    }

    double dot(const Vector2& other) const {
        return x * other.x + y * other.y;
    }

    Vector2 normalize() const {
        double len = length();
        return len > 0 ? Vector2(x / len, y / len) : Vector2(0, 0);
    }
};

// Mod flags
struct Mods {
    bool no_fail = false;
    bool easy = false;
    bool touch_device = false;
    bool hidden = false;
    bool hard_rock = false;
    bool sudden_death = false;
    bool double_time = false;
    bool relax = false;
    bool half_time = false;
    bool nightcore = false;  // Implies DT
    bool flashlight = false;
    bool auto_play = false;
    bool spun_out = false;
    bool autopilot = false;
    bool perfect = false;     // Implies SD
    bool blinds = false;
    bool traceable = false;

    double get_clock_rate() const {
        if (double_time || nightcore) return 1.5;
        if (half_time) return 0.75;
        return 1.0;
    }

    double get_od_multiplier() const {
        if (hard_rock) return 1.4;
        if (easy) return 0.5;
        return 1.0;
    }

    double get_ar_multiplier() const {
        if (hard_rock) return 1.4;
        if (easy) return 0.5;
        return 1.0;
    }

    double get_cs_multiplier() const {
        if (hard_rock) return 1.3;
        if (easy) return 0.5;
        return 1.0;
    }
};

// Hit object types
enum class HitObjectType {
    Circle,
    Slider,
    Spinner
};

// Slider path types
enum class PathType {
    Linear,
    Perfect,  // Circular arc
    Bezier,
    Catmull
};

// Base hit object
struct HitObject {
    HitObjectType type;
    Vector2 position;
    double start_time = 0.0;
    bool new_combo = false;
    int combo_offset = 0;

    // For sliders
    PathType path_type = PathType::Linear;
    std::vector<Vector2> control_points;
    int repeat_count = 0;
    double pixel_length = 0.0;

    // For spinners
    double end_time = 0.0;

    HitObject() = default;
};

// Timing point
struct TimingPoint {
    double time = 0.0;
    double beat_length = 0.0;  // Milliseconds per beat
    int time_signature = 4;
    bool uninherited = true;    // Red line vs green line

    double get_bpm() const {
        return uninherited && beat_length > 0 ? 60000.0 / beat_length : 0.0;
    }

    double get_slider_velocity_multiplier() const {
        return uninherited ? 1.0 : std::abs(100.0 / beat_length);
    }
};

// Beatmap difficulty settings
struct BeatmapDifficulty {
    double hp_drain_rate = 5.0;
    double circle_size = 5.0;
    double overall_difficulty = 5.0;
    double approach_rate = 5.0;
    double slider_multiplier = 1.4;
    double slider_tick_rate = 1.0;
};

// Complete beatmap data
struct Beatmap {
    // Metadata
    std::string title;
    std::string artist;
    std::string creator;
    std::string version;
    int mode = 0;  // 0=std, 1=taiko, 2=catch, 3=mania

    // Difficulty
    BeatmapDifficulty difficulty;

    // Objects
    std::vector<HitObject> hit_objects;
    std::vector<TimingPoint> timing_points;

    // Calculated values
    int max_combo = 0;
};

// Score information
struct ScoreInfo {
    double accuracy = 1.0;  // 0.0 to 1.0
    int max_combo = 0;
    int count_300 = 0;
    int count_100 = 0;
    int count_50 = 0;
    int count_miss = 0;
    int count_geki = 0;  // 300g
    int count_katu = 0;  // 200/100k

    Mods mods;

    int get_total_hits() const {
        return count_300 + count_100 + count_50 + count_miss;
    }

    double calculate_accuracy() const {
        int total = get_total_hits();
        if (total == 0) return 1.0;

        double weighted_sum = count_300 * 300.0 + count_100 * 100.0 + count_50 * 50.0;
        double max_sum = total * 300.0;
        return weighted_sum / max_sum;
    }
};

// Difficulty attributes
struct DifficultyAttributes {
    double star_rating = 0.0;
    double aim_difficulty = 0.0;
    double speed_difficulty = 0.0;
    double flashlight_difficulty = 0.0;

    double approach_rate = 5.0;
    double overall_difficulty = 5.0;
    double circle_size = 5.0;
    double hp_drain = 5.0;

    int max_combo = 0;
    int slider_count = 0;
    int spinner_count = 0;
    int circle_count = 0;

    // Advanced attributes
    double aim_difficult_strain_count = 0.0;
    double speed_difficult_strain_count = 0.0;
    double speed_note_count = 0.0;

    // Slider factors
    double aim_slider_factor = 0.0;
    double aim_difficult_slider_count = 0.0;
    double speed_difficult_slider_count = 0.0;
};

// Performance attributes
struct PerformanceAttributes {
    double total_pp = 0.0;
    double aim_pp = 0.0;
    double speed_pp = 0.0;
    double accuracy_pp = 0.0;
    double flashlight_pp = 0.0;

    DifficultyAttributes difficulty;
};

} // namespace osupp
