#pragma once

#include "types.h"
#include <cmath>
#include <algorithm>

namespace osupp {

// Constants from official implementation
constexpr double NORMALISED_RADIUS = 50.0;  // Radius used for difficulty calculations
constexpr double OBJECT_RADIUS = 64.0;      // Base object radius

constexpr double PREEMPT_MIN = 450.0;   // AR 10
constexpr double PREEMPT_MID = 1200.0;  // AR 5
constexpr double PREEMPT_MAX = 1800.0;  // AR 0

constexpr double HIT_WINDOW_50_MIN = 20.0;   // OD 10
constexpr double HIT_WINDOW_50_MID = 50.0;   // OD 5
constexpr double HIT_WINDOW_50_MAX = 80.0;   // OD 0

constexpr double HIT_WINDOW_300_MIN = 80.0;  // OD 10
constexpr double HIT_WINDOW_300_MID = 50.0;  // OD 5
constexpr double HIT_WINDOW_300_MAX = 20.0;  // OD 0

// Calculate circle radius from CS
inline double calculate_radius(double circle_size) {
    // From official: OBJECT_RADIUS * (1.0f - 0.7f * (circleSize - 5) / 5)
    return OBJECT_RADIUS * (1.0 - 0.7 * (circle_size - 5.0) / 5.0);
}

// Calculate scale factor for normalization
inline double calculate_scale(double circle_size) {
    return (1.0 - 0.7 * (circle_size - 5.0) / 5.0);
}

// Convert AR to preempt time (milliseconds)
inline double ar_to_ms(double ar) {
    if (ar <= 5.0) {
        return PREEMPT_MID + (PREEMPT_MAX - PREEMPT_MID) * (5.0 - ar) / 5.0;
    }
    return PREEMPT_MID - (PREEMPT_MID - PREEMPT_MIN) * (ar - 5.0) / 5.0;
}

// Convert OD to hit window for 300 (milliseconds)
inline double od_to_ms_300(double od) {
    if (od <= 5.0) {
        return HIT_WINDOW_300_MID + (HIT_WINDOW_300_MAX - HIT_WINDOW_300_MID) * (5.0 - od) / 5.0;
    }
    return HIT_WINDOW_300_MID - (HIT_WINDOW_300_MID - HIT_WINDOW_300_MIN) * (od - 5.0) / 5.0;
}

// Convert OD to hit window for 50 (milliseconds)
inline double od_to_ms_50(double od) {
    if (od <= 5.0) {
        return HIT_WINDOW_50_MID + (HIT_WINDOW_50_MAX - HIT_WINDOW_50_MID) * (5.0 - od) / 5.0;
    }
    return HIT_WINDOW_50_MID - (HIT_WINDOW_50_MID - HIT_WINDOW_50_MIN) * (od - 5.0) / 5.0;
}

// Apply mod adjustments to difficulty
inline double apply_mod_adjustment(double value, double multiplier, bool increase) {
    if (multiplier == 1.0) return value;

    if (increase) {
        // HR increases, EZ decreases
        value *= multiplier;
    } else {
        // EZ decreases
        value *= multiplier;
    }

    return std::clamp(value, 0.0, 10.0);
}

// Apply rate adjustment to AR (for DT/HT)
inline double apply_rate_adjustment_ar(double ar, double clock_rate) {
    double preempt = ar_to_ms(ar);
    preempt /= clock_rate;

    // Convert back to AR
    if (preempt > PREEMPT_MID) {
        return 5.0 - (preempt - PREEMPT_MID) * 5.0 / (PREEMPT_MAX - PREEMPT_MID);
    }
    return 5.0 + (PREEMPT_MID - preempt) * 5.0 / (PREEMPT_MID - PREEMPT_MIN);
}

// Apply rate adjustment to OD (for DT/HT)
inline double apply_rate_adjustment_od(double od, double clock_rate) {
    double hit_window = od_to_ms_300(od);
    hit_window /= clock_rate;

    // Convert back to OD
    if (hit_window > HIT_WINDOW_300_MID) {
        return 5.0 - (hit_window - HIT_WINDOW_300_MID) * 5.0 / (HIT_WINDOW_300_MAX - HIT_WINDOW_300_MID);
    }
    return 5.0 + (HIT_WINDOW_300_MID - hit_window) * 5.0 / (HIT_WINDOW_300_MID - HIT_WINDOW_300_MIN);
}

// Apply all mod adjustments to beatmap difficulty
inline BeatmapDifficulty apply_mods_to_difficulty(const BeatmapDifficulty& diff, const Mods& mods) {
    BeatmapDifficulty result = diff;

    // Apply HR/EZ multipliers
    if (mods.hard_rock) {
        result.circle_size = apply_mod_adjustment(result.circle_size, mods.get_cs_multiplier(), true);
        result.approach_rate = apply_mod_adjustment(result.approach_rate, mods.get_ar_multiplier(), true);
        result.overall_difficulty = apply_mod_adjustment(result.overall_difficulty, mods.get_od_multiplier(), true);
        result.hp_drain_rate = apply_mod_adjustment(result.hp_drain_rate, 1.4, true);
    }

    if (mods.easy) {
        result.circle_size = apply_mod_adjustment(result.circle_size, mods.get_cs_multiplier(), false);
        result.approach_rate = apply_mod_adjustment(result.approach_rate, mods.get_ar_multiplier(), false);
        result.overall_difficulty = apply_mod_adjustment(result.overall_difficulty, mods.get_od_multiplier(), false);
        result.hp_drain_rate = apply_mod_adjustment(result.hp_drain_rate, 0.5, false);
    }

    // Apply rate adjustments (DT/HT)
    double clock_rate = mods.get_clock_rate();
    if (clock_rate != 1.0) {
        result.approach_rate = apply_rate_adjustment_ar(result.approach_rate, clock_rate);
        result.overall_difficulty = apply_rate_adjustment_od(result.overall_difficulty, clock_rate);
    }

    return result;
}

// Smoothstep function for bonuses
inline double smoothstep(double edge0, double edge1, double x) {
    x = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// Reverse lerp (used in wiggle bonus)
inline double reverse_lerp(double a, double b, double value) {
    return std::clamp((value - a) / (b - a), 0.0, 1.0);
}

// Smootherstep function (smoother than smoothstep)
inline double smootherstep(double edge0, double edge1, double x) {
    x = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

// Convert milliseconds to BPM
inline double ms_to_bpm(double ms, double divisor = 1.0) {
    return 60000.0 / (ms * divisor);
}

// Smoothstep bell curve (returns 1 at mean, smoothly reduces to 0 over width)
inline double smoothstep_bell_curve(double x, double mean = 0.5, double width = 0.5) {
    x -= mean;
    x = x > 0 ? (width - x) : (width + x);
    return smoothstep(0.0, width, x);
}

// Logistic function (S-shaped sigmoid curve)
inline double logistic(double x, double midpoint_offset, double multiplier, double max_value = 1.0) {
    return max_value / (1.0 + std::exp(multiplier * (midpoint_offset - x)));
}

// Convert degrees to radians
inline double deg_to_rad(double degrees) {
    return degrees * M_PI / 180.0;
}

// Calculate angle between three points (in radians)
inline std::optional<double> calculate_angle(const Vector2& p1, const Vector2& p2, const Vector2& p3) {
    Vector2 v1 = p1 - p2;
    Vector2 v2 = p3 - p2;

    double len1 = v1.length();
    double len2 = v2.length();

    if (len1 < 1e-10 || len2 < 1e-10) {
        return std::nullopt;
    }

    double dot_product = v1.dot(v2);
    double cos_angle = dot_product / (len1 * len2);
    cos_angle = std::clamp(cos_angle, -1.0, 1.0);

    return std::acos(cos_angle);
}

// Difficulty to performance conversion (used in performance calculation)
inline double difficulty_to_performance(double difficulty) {
    return std::pow(5.0 * std::max(1.0, difficulty / 0.0675) - 4.0, 3.0) / 100000.0;
}

// Get timing point at specific time
inline const TimingPoint* get_timing_point_at(const std::vector<TimingPoint>& timing_points, double time) {
    const TimingPoint* current = nullptr;

    for (const auto& tp : timing_points) {
        if (tp.time <= time && tp.uninherited) {
            current = &tp;
        } else if (tp.time > time) {
            break;
        }
    }

    return current;
}

// Get slider velocity multiplier at specific time
inline double get_slider_velocity_at(const std::vector<TimingPoint>& timing_points, double time) {
    double multiplier = 1.0;

    for (const auto& tp : timing_points) {
        if (tp.time <= time) {
            if (!tp.uninherited) {
                multiplier = tp.get_slider_velocity_multiplier();
            }
        } else {
            break;
        }
    }

    return multiplier;
}

} // namespace osupp
