#pragma once

#include "types.h"
#include "difficulty_calculator.h"
#include <cmath>
#include <algorithm>

namespace osupp {

constexpr double PERFORMANCE_BASE_MULTIPLIER = 1.14;

// Calculate effective miss count (accounts for slider breaks)
inline double calculate_effective_miss_count(const ScoreInfo& score, const DifficultyAttributes& attrs) {
    double combo_based_miss_count = 0.0;

    if (attrs.slider_count > 0) {
        double full_combo_threshold = attrs.max_combo - 0.1 * attrs.slider_count;

        if (score.max_combo < full_combo_threshold) {
            combo_based_miss_count = full_combo_threshold - score.max_combo;
        }
    }

    return std::max(static_cast<double>(score.count_miss), combo_based_miss_count);
}

// Compute aim PP value
inline double compute_aim_value(const ScoreInfo& score, const DifficultyAttributes& attrs) {
    double aim_difficulty = attrs.aim_difficulty;

    // Convert difficulty to performance
    double aim_value = std::pow(5.0 * std::max(1.0, aim_difficulty / 0.0675) - 4.0, 3.0) / 100000.0;

    // Length bonus
    double length_bonus = 0.95 + 0.4 * std::min(1.0, static_cast<double>(score.get_total_hits()) / 2000.0);
    if (score.get_total_hits() > 2000) {
        length_bonus += std::log10(score.get_total_hits() / 2000.0) * 0.5;
    }
    aim_value *= length_bonus;

    // Miss penalty
    double effective_miss_count = calculate_effective_miss_count(score, attrs);
    if (effective_miss_count > 0) {
        aim_value *= 0.97 * std::pow(
            1.0 - std::pow(effective_miss_count / score.get_total_hits(), 0.775),
            effective_miss_count
        );
    }

    // Combo scaling
    if (attrs.max_combo > 0) {
        aim_value *= std::min(
            std::pow(static_cast<double>(score.max_combo), 0.8) / std::pow(static_cast<double>(attrs.max_combo), 0.8),
            1.0
        );
    }

    // AR bonus (for HD)
    double approach_rate_factor = 0.0;
    if (attrs.approach_rate > 10.33) {
        approach_rate_factor = 0.3 * (attrs.approach_rate - 10.33);
    } else if (attrs.approach_rate < 8.0) {
        approach_rate_factor = 0.05 * (8.0 - attrs.approach_rate);
    }

    if (score.mods.hidden) {
        aim_value *= 1.0 + approach_rate_factor;
    }

    if (score.mods.blinds) {
        aim_value *= 1.0 + approach_rate_factor * 0.8;
    }

    // Flashlight bonus
    if (score.mods.flashlight) {
        aim_value *= 1.0 + 0.35 * std::min(1.0, static_cast<double>(score.get_total_hits()) / 200.0) +
                     (score.get_total_hits() > 200 ? 0.3 * std::min(1.0, (score.get_total_hits() - 200.0) / 300.0) : 0.0) +
                     (score.get_total_hits() > 500 ? (score.get_total_hits() - 500.0) / 1200.0 : 0.0);
    }

    // Accuracy scaling
    double better_accuracy = score.accuracy;
    if (attrs.slider_count > 0 && attrs.circle_count > 0) {
        // Account for sliders in accuracy - calculate accuracy based on circles only
        int amount_hit_objects_with_accuracy = attrs.circle_count;
        double circles_hit = std::max(0.0, static_cast<double>(score.count_300) - (score.get_total_hits() - amount_hit_objects_with_accuracy));
        better_accuracy = circles_hit / amount_hit_objects_with_accuracy;
    }
    aim_value *= 0.5 + better_accuracy / 2.0;

    // AR scaling
    aim_value *= 0.98 + std::pow(attrs.overall_difficulty, 2.0) / 2500.0;

    return aim_value;
}

// Compute speed PP value
inline double compute_speed_value(const ScoreInfo& score, const DifficultyAttributes& attrs) {
    double speed_difficulty = attrs.speed_difficulty;

    // Convert difficulty to performance
    double speed_value = std::pow(5.0 * std::max(1.0, speed_difficulty / 0.0675) - 4.0, 3.0) / 100000.0;

    // Length bonus
    double length_bonus = 0.95 + 0.4 * std::min(1.0, static_cast<double>(score.get_total_hits()) / 2000.0);
    if (score.get_total_hits() > 2000) {
        length_bonus += std::log10(score.get_total_hits() / 2000.0) * 0.5;
    }
    speed_value *= length_bonus;

    // Miss penalty
    double effective_miss_count = calculate_effective_miss_count(score, attrs);
    if (effective_miss_count > 0) {
        speed_value *= 0.97 * std::pow(
            1.0 - std::pow(effective_miss_count / score.get_total_hits(), 0.775),
            std::pow(effective_miss_count, 0.875)
        );
    }

    // Combo scaling (less important for speed than aim)
    if (attrs.max_combo > 0) {
        speed_value *= std::min(
            std::pow(static_cast<double>(score.max_combo), 0.8) / std::pow(static_cast<double>(attrs.max_combo), 0.8),
            1.0
        );
    }

    // AR bonus
    double approach_rate_factor = 0.0;
    if (attrs.approach_rate > 10.33) {
        approach_rate_factor = 0.3 * (attrs.approach_rate - 10.33);
    }

    if (score.mods.hidden) {
        speed_value *= 1.0 + approach_rate_factor;
    }

    // Accuracy scaling based on OD
    double od_scaled_accuracy = (14.5 - attrs.overall_difficulty) / 2.0;
    speed_value *= 0.95 + std::pow(attrs.overall_difficulty, 2.0) / 750.0 *
                   std::pow(score.accuracy, od_scaled_accuracy);

    // NOTE: The 50s/100s specific penalty was removed in favor of the relevantAccuracy calculation
    // which the official implementation uses (line 260-267 in OsuPerformanceCalculator.cs)
    // We are using a simplified version that just uses the overall accuracy

    return speed_value;
}

// Compute accuracy PP value
inline double compute_accuracy_value(const ScoreInfo& score, const DifficultyAttributes& attrs) {
    // Better accuracy calculation
    double better_accuracy = score.accuracy;

    if (attrs.slider_count > 0) {
        int amount_hit_objects_with_accuracy = attrs.circle_count;
        if (amount_hit_objects_with_accuracy > 0) {
            better_accuracy = static_cast<double>(
                score.count_300 - (score.get_total_hits() - amount_hit_objects_with_accuracy)
            );
            better_accuracy = std::max(0.0, better_accuracy) / amount_hit_objects_with_accuracy;
        }
    }

    // Base accuracy value
    double accuracy_value = std::pow(1.52163, attrs.overall_difficulty) *
                           std::pow(better_accuracy, 24.0) * 2.83;

    // Length bonus
    accuracy_value *= std::min(1.15, std::pow(static_cast<double>(attrs.circle_count) / 1000.0, 0.3));

    // HD bonus
    if (score.mods.hidden) {
        accuracy_value *= 1.08;
    }

    // FL bonus
    if (score.mods.flashlight) {
        accuracy_value *= 1.02;
    }

    return accuracy_value;
}

// Compute flashlight PP value
inline double compute_flashlight_value(const ScoreInfo& score, const DifficultyAttributes& attrs) {
    if (!score.mods.flashlight) {
        return 0.0;
    }

    double flashlight_difficulty = attrs.flashlight_difficulty;

    // Convert difficulty to performance
    double flashlight_value = std::pow(flashlight_difficulty, 2.0) * 25.0;

    // Length bonus
    double length_bonus = 0.95 + 0.4 * std::min(1.0, static_cast<double>(score.get_total_hits()) / 2000.0);
    if (score.get_total_hits() > 2000) {
        length_bonus += std::log10(score.get_total_hits() / 2000.0) * 0.5;
    }
    flashlight_value *= length_bonus;

    // Miss penalty
    double effective_miss_count = calculate_effective_miss_count(score, attrs);
    if (effective_miss_count > 0) {
        flashlight_value *= 0.97 * std::pow(
            1.0 - std::pow(effective_miss_count / score.get_total_hits(), 0.775),
            std::pow(effective_miss_count, 0.875)
        );
    }

    // Combo scaling
    if (attrs.max_combo > 0) {
        flashlight_value *= std::min(
            std::pow(static_cast<double>(score.max_combo), 0.8) / std::pow(static_cast<double>(attrs.max_combo), 0.8),
            1.0
        );
    }

    // Accuracy scaling
    flashlight_value *= 0.5 + score.accuracy / 2.0;
    flashlight_value *= 0.98 + std::pow(attrs.overall_difficulty, 2.0) / 2500.0;

    return flashlight_value;
}

// Calculate complete performance attributes
inline PerformanceAttributes calculate_performance(const Beatmap& beatmap, const ScoreInfo& score) {
    PerformanceAttributes result;

    // Calculate difficulty attributes
    result.difficulty = calculate_difficulty(beatmap, score.mods);

    // Calculate individual PP values
    result.aim_pp = compute_aim_value(score, result.difficulty);
    result.speed_pp = compute_speed_value(score, result.difficulty);
    result.accuracy_pp = compute_accuracy_value(score, result.difficulty);
    result.flashlight_pp = compute_flashlight_value(score, result.difficulty);

    // Calculate total PP using power mean
    double multiplier = PERFORMANCE_BASE_MULTIPLIER;

    // Apply mod multipliers
    if (score.mods.no_fail) {
        multiplier *= 0.90;
    }
    if (score.mods.spun_out) {
        multiplier *= 0.95;
    }
    if (score.mods.relax) {
        multiplier *= 0.0; // Relax gives 0 PP
    }

    result.total_pp = std::pow(
        std::pow(result.aim_pp, 1.1) +
        std::pow(result.speed_pp, 1.1) +
        std::pow(result.accuracy_pp, 1.1) +
        std::pow(result.flashlight_pp, 1.1),
        1.0 / 1.1
    ) * multiplier;

    return result;
}

// Calculate FC (Full Combo) performance
inline PerformanceAttributes calculate_fc_performance(const Beatmap& beatmap, const ScoreInfo& score) {
    ScoreInfo fc_score = score;

    // Convert misses to 300s
    fc_score.count_300 += fc_score.count_miss;
    fc_score.count_miss = 0;

    // Set combo to max
    auto temp_diff = calculate_difficulty(beatmap, score.mods);
    fc_score.max_combo = temp_diff.max_combo;

    // Recalculate accuracy
    int total_hits = fc_score.get_total_hits();
    if (total_hits > 0) {
        fc_score.accuracy = (fc_score.count_300 * 300.0 + fc_score.count_100 * 100.0 + fc_score.count_50 * 50.0)
                          / (total_hits * 300.0);
    }

    return calculate_performance(beatmap, fc_score);
}

} // namespace osupp
