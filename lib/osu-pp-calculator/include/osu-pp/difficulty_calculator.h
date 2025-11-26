#pragma once

#include "types.h"
#include "beatmap_parser.h"
#include "difficulty_object.h"
#include "strain_skill.h"
#include "rating_calculator.h"
#include <cmath>
#include <iostream>

namespace osupp {

// Helper function: calculate star rating from performance
inline double calculate_star_rating(double base_performance) {
    if (base_performance <= 0.00001) {
        return 0.0;
    }
    constexpr double PERFORMANCE_BASE_MULTIPLIER = 1.14;
    constexpr double STAR_RATING_MULTIPLIER = 0.0265;
    constexpr double divisor = 100000.0 / std::pow(2.0, 1.0 / 1.1);
    return std::cbrt(PERFORMANCE_BASE_MULTIPLIER) * STAR_RATING_MULTIPLIER *
           (std::cbrt(divisor * base_performance) + 4.0);
}

// Helper function: calculate mechanical difficulty rating
inline double calculate_mechanical_difficulty_rating(double aim_difficulty_value, double speed_difficulty_value) {
    double aim_rating = OsuRatingCalculator::calculate_difficulty_rating(aim_difficulty_value);
    double speed_rating = OsuRatingCalculator::calculate_difficulty_rating(speed_difficulty_value);

    double aim_value = difficulty_to_performance(aim_rating);
    double speed_value = difficulty_to_performance(speed_rating);

    double total_value = std::pow(
        std::pow(aim_value, 1.1) + std::pow(speed_value, 1.1),
        1.0 / 1.1
    );

    return calculate_star_rating(total_value);
}

// Calculate difficulty attributes for a beatmap
inline DifficultyAttributes calculate_difficulty(const Beatmap& beatmap, const Mods& mods) {
    DifficultyAttributes attrs;

    // Apply mods to difficulty settings
    auto adjusted_diff = apply_mods_to_difficulty(beatmap.difficulty, mods);
    double clock_rate = mods.get_clock_rate();

    attrs.approach_rate = adjusted_diff.approach_rate;
    attrs.overall_difficulty = adjusted_diff.overall_difficulty;
    attrs.circle_size = adjusted_diff.circle_size;
    attrs.hp_drain = adjusted_diff.hp_drain_rate;

    // Count object types
    for (const auto& obj : beatmap.hit_objects) {
        if (obj.type == HitObjectType::Circle) {
            attrs.circle_count++;
        } else if (obj.type == HitObjectType::Slider) {
            attrs.slider_count++;
        } else if (obj.type == HitObjectType::Spinner) {
            attrs.spinner_count++;
        }
    }

    attrs.max_combo = beatmap.max_combo;

    // Build difficulty objects
    auto diff_objects = build_difficulty_objects(beatmap, mods);

    if (diff_objects.empty()) {
        return attrs;
    }

    // Calculate hit window for speed skill
    double hit_window_great = od_to_ms_300(adjusted_diff.overall_difficulty) / clock_rate;

    // Create skills
    AimSkill aim_skill_with_sliders(true);
    AimSkill aim_skill_no_sliders(false);
    SpeedSkill speed_skill(hit_window_great);
    FlashlightSkill flashlight_skill(mods.flashlight);

    // Pass diff_objects reference to skills for official evaluators
    aim_skill_with_sliders.set_difficulty_objects(&diff_objects);
    aim_skill_no_sliders.set_difficulty_objects(&diff_objects);
    speed_skill.set_difficulty_objects(&diff_objects);
    flashlight_skill.set_difficulty_objects(&diff_objects);

    // Process all difficulty objects through skills
    for (const auto& diff_obj : diff_objects) {
        aim_skill_with_sliders.process(diff_obj);
        aim_skill_no_sliders.process(diff_obj);
        speed_skill.process(diff_obj);
        flashlight_skill.process(diff_obj);
    }

    // Get raw difficulty values (peaks are saved automatically in difficulty_value())
    double aim_difficulty_value = aim_skill_with_sliders.difficulty_value();
    double aim_no_sliders_value = aim_skill_no_sliders.difficulty_value();
    double speed_difficulty_value = speed_skill.difficulty_value();
    double flashlight_difficulty_value = flashlight_skill.difficulty_value();

    // Calculate mechanical difficulty rating (needed for OsuRatingCalculator)
    double mechanical_difficulty_rating = calculate_mechanical_difficulty_rating(aim_difficulty_value, speed_difficulty_value);

    // Calculate slider factor
    double aim_rating_basic = OsuRatingCalculator::calculate_difficulty_rating(aim_difficulty_value);
    double aim_no_sliders_rating_basic = OsuRatingCalculator::calculate_difficulty_rating(aim_no_sliders_value);
    double slider_factor = aim_rating_basic > 0.0 ? aim_no_sliders_rating_basic / aim_rating_basic : 1.0;

    // Total hits
    int total_hits = beatmap.hit_objects.size();

    // Create OsuRatingCalculator
    OsuRatingCalculator rating_calculator(
        mods,
        total_hits,
        adjusted_diff.approach_rate,
        adjusted_diff.overall_difficulty,
        mechanical_difficulty_rating,
        slider_factor
    );

    // Compute final ratings with all bonuses
    double aim_rating = rating_calculator.compute_aim_rating(aim_difficulty_value);
    double speed_rating = rating_calculator.compute_speed_rating(speed_difficulty_value);
    double flashlight_rating = rating_calculator.compute_flashlight_rating(flashlight_difficulty_value);

    // Debug logging
    std::cerr << "[DEBUG] total_hits: " << total_hits << std::endl;
    std::cerr << "[DEBUG] aim_difficulty_value: " << aim_difficulty_value << std::endl;
    std::cerr << "[DEBUG] speed_difficulty_value: " << speed_difficulty_value << std::endl;
    std::cerr << "[DEBUG] aim_rating_basic: " << aim_rating_basic << std::endl;
    std::cerr << "[DEBUG] aim_rating: " << aim_rating << std::endl;
    std::cerr << "[DEBUG] speed_rating: " << speed_rating << std::endl;
    std::cerr << "[DEBUG] mechanical_difficulty_rating: " << mechanical_difficulty_rating << std::endl;
    std::cerr << "[DEBUG] slider_factor: " << slider_factor << std::endl;
    std::cerr << "[DEBUG] aim_difficult_strain_count: " << aim_skill_with_sliders.get_strain_peaks().size() << std::endl;
    std::cerr << "[DEBUG] speed_difficult_strain_count: " << speed_skill.get_strain_peaks().size() << std::endl;

    attrs.aim_difficulty = aim_rating;
    attrs.speed_difficulty = speed_rating;
    attrs.flashlight_difficulty = flashlight_rating;

    // Calculate base performance values
    double base_aim_performance = difficulty_to_performance(aim_rating);
    double base_speed_performance = difficulty_to_performance(speed_rating);
    double base_flashlight_performance = 0.0;

    if (mods.flashlight) {
        base_flashlight_performance = std::pow(flashlight_rating, 2.0) * 25.0;
    }

    // Combine performances
    double combined_performance = std::pow(
        std::pow(base_aim_performance, 1.1) +
        std::pow(base_speed_performance, 1.1) +
        std::pow(base_flashlight_performance, 1.1),
        1.0 / 1.1
    );

    // Convert to star rating
    attrs.star_rating = calculate_star_rating(combined_performance);

    // Calculate advanced attributes
    attrs.aim_difficult_strain_count = aim_skill_with_sliders.get_strain_peaks().size();
    attrs.speed_difficult_strain_count = speed_skill.get_strain_peaks().size();
    attrs.speed_note_count = speed_skill.relevant_note_count();

    // Slider factors
    attrs.aim_slider_factor = slider_factor;
    attrs.aim_difficult_slider_count = aim_skill_with_sliders.get_difficult_sliders();
    attrs.speed_difficult_slider_count = 0.0; // Speed doesn't track difficult sliders

    return attrs;
}

} // namespace osupp
