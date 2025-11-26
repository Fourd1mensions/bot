#pragma once

#include "types.h"
#include "difficulty_utils.h"
#include <cmath>
#include <algorithm>

namespace osupp {

// Based on OsuRatingCalculator.cs
class OsuRatingCalculator {
private:
    static constexpr double DIFFICULTY_MULTIPLIER = 0.0675;

    const Mods& mods;
    int total_hits;
    double approach_rate;
    double overall_difficulty;
    double mechanical_difficulty_rating;
    double slider_factor;

    double calculate_aim_visibility_factor(double ar) const {
        constexpr double ar_factor_end_point = 11.5;
        double mechanical_difficulty_factor = reverse_lerp(mechanical_difficulty_rating, 5.0, 10.0);
        double ar_factor_starting_point = lerp(9.0, 10.33, mechanical_difficulty_factor);
        return reverse_lerp(ar, ar_factor_end_point, ar_factor_starting_point);
    }

    double calculate_speed_visibility_factor(double ar) const {
        constexpr double ar_factor_end_point = 11.5;
        double mechanical_difficulty_factor = reverse_lerp(mechanical_difficulty_rating, 5.0, 10.0);
        double ar_factor_starting_point = lerp(10.0, 10.33, mechanical_difficulty_factor);
        return reverse_lerp(ar, ar_factor_end_point, ar_factor_starting_point);
    }

    double lerp(double a, double b, double t) const {
        return a + (b - a) * t;
    }

public:
    OsuRatingCalculator(
        const Mods& mods_ref,
        int hits,
        double ar,
        double od,
        double mech_diff,
        double slider_fact
    ) : mods(mods_ref),
        total_hits(hits),
        approach_rate(ar),
        overall_difficulty(od),
        mechanical_difficulty_rating(mech_diff),
        slider_factor(slider_fact) {}

    double compute_aim_rating(double aim_difficulty_value) const {
        double aim_rating = calculate_difficulty_rating(aim_difficulty_value);

        double rating_multiplier = 1.0;

        // AR length bonus
        double ar_length_bonus = 0.95 + 0.4 * std::min(1.0, total_hits / 2000.0);
        if (total_hits > 2000) {
            ar_length_bonus += std::log10(total_hits / 2000.0) * 0.5;
        }

        // AR factor
        double ar_factor = 0.0;
        if (approach_rate > 10.33) {
            ar_factor = 0.3 * (approach_rate - 10.33);
        } else if (approach_rate < 8.0) {
            ar_factor = 0.05 * (8.0 - approach_rate);
        }

        rating_multiplier += ar_factor * ar_length_bonus;

        // Hidden bonus
        if (mods.hidden) {
            double visibility_factor = calculate_aim_visibility_factor(approach_rate);
            rating_multiplier += calculate_visibility_bonus(approach_rate, visibility_factor, slider_factor);
        }

        // OD multiplier
        rating_multiplier *= 0.98 + std::pow(std::max(0.0, overall_difficulty), 2.0) / 2500.0;

        return aim_rating * std::cbrt(rating_multiplier);
    }

    double compute_speed_rating(double speed_difficulty_value) const {
        double speed_rating = calculate_difficulty_rating(speed_difficulty_value);

        double rating_multiplier = 1.0;

        // AR length bonus
        double ar_length_bonus = 0.95 + 0.4 * std::min(1.0, total_hits / 2000.0);
        if (total_hits > 2000) {
            ar_length_bonus += std::log10(total_hits / 2000.0) * 0.5;
        }

        // AR factor
        double ar_factor = 0.0;
        if (approach_rate > 10.33) {
            ar_factor = 0.3 * (approach_rate - 10.33);
        }

        rating_multiplier += ar_factor * ar_length_bonus;

        // Hidden bonus
        if (mods.hidden) {
            double visibility_factor = calculate_speed_visibility_factor(approach_rate);
            rating_multiplier += calculate_visibility_bonus(approach_rate, visibility_factor);
        }

        // OD multiplier
        rating_multiplier *= 0.95 + std::pow(std::max(0.0, overall_difficulty), 2.0) / 750.0;

        return speed_rating * std::cbrt(rating_multiplier);
    }

    double compute_flashlight_rating(double flashlight_difficulty_value) const {
        if (!mods.flashlight) {
            return 0.0;
        }

        double flashlight_rating = calculate_difficulty_rating(flashlight_difficulty_value);

        double rating_multiplier = 1.0;

        // Account for shorter maps having higher ratio of 0 combo/100 combo flashlight radius
        rating_multiplier *= 0.7 + 0.1 * std::min(1.0, total_hits / 200.0);
        if (total_hits > 200) {
            rating_multiplier += 0.2 * std::min(1.0, (total_hits - 200) / 200.0);
        }

        // OD multiplier
        rating_multiplier *= 0.98 + std::pow(std::max(0.0, overall_difficulty), 2.0) / 2500.0;

        return flashlight_rating * std::sqrt(rating_multiplier);
    }

    double calculate_visibility_bonus(double ar, double visibility_factor, double slider_fact = 1.0) const {
        bool is_always_partially_visible = false; // We don't support Traceable mod yet

        // Start from normal curve, rewarding lower AR up to AR7
        double reading_bonus = 0.04 * (12.0 - std::max(ar, 7.0));
        reading_bonus *= visibility_factor;

        // Slider visibility factor
        double slider_visibility_factor = std::pow(slider_fact, 3.0);

        // For AR up to 0 - reduce reward for very low ARs
        if (ar < 7.0) {
            reading_bonus += 0.045 * (7.0 - std::max(ar, 0.0)) * slider_visibility_factor;
        }

        // Starting from AR0 - cap values
        if (ar < 0.0) {
            reading_bonus += 0.1 * (1.0 - std::pow(1.5, ar)) * slider_visibility_factor;
        }

        return reading_bonus;
    }

    static double calculate_difficulty_rating(double difficulty_value) {
        return std::sqrt(difficulty_value) * DIFFICULTY_MULTIPLIER;
    }
};

} // namespace osupp
