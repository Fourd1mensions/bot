#pragma once

#include "difficulty_object.h"
#include "difficulty_utils.h"
#include <cmath>
#include <algorithm>

namespace osupp {
namespace evaluators {

// Constants from official implementation
constexpr double WIDE_ANGLE_MULTIPLIER = 1.5;
constexpr double ACUTE_ANGLE_MULTIPLIER = 2.55;
constexpr double SLIDER_MULTIPLIER = 1.35;
constexpr double VELOCITY_CHANGE_MULTIPLIER = 0.75;
constexpr double WIGGLE_MULTIPLIER = 1.02;

// Helper functions for angle bonuses
inline double calc_wide_angle_bonus(double angle) {
    return smootherstep(deg_to_rad(40.0), deg_to_rad(140.0), angle);
}

inline double calc_acute_angle_bonus(double angle) {
    return smootherstep(deg_to_rad(140.0), deg_to_rad(40.0), angle);
}

// Helper: Calculate doubletap penalty (used by speed and rhythm evaluators)
inline double get_doubletapness(
    const OsuDifficultyHitObject& current,
    const OsuDifficultyHitObject* next
) {
    if (next != nullptr) {
        double curr_delta_time = std::max(1.0, current.delta_time);
        double next_delta_time = std::max(1.0, next->delta_time);
        double delta_difference = std::abs(next_delta_time - curr_delta_time);
        double speed_ratio = curr_delta_time / std::max(curr_delta_time, delta_difference);
        double window_ratio = std::pow(std::min(1.0, curr_delta_time / current.hit_window_great), 2.0);
        return 1.0 - std::pow(speed_ratio, 1.0 - window_ratio);
    }
    return 0.0;
}

// === AIM EVALUATOR (1:1 with official) ===
inline double evaluate_aim_official(
    const std::vector<OsuDifficultyHitObject>& diff_objects,
    int current_index,
    bool with_sliders
) {
    if (current_index <= 1) {
        return 0.0;
    }

    const auto& current = diff_objects[current_index];
    const auto& last = diff_objects[current_index - 1];
    const auto& last_last = diff_objects[current_index - 2];

    // Skip spinners
    if (current.base_object->type == HitObjectType::Spinner ||
        last.base_object->type == HitObjectType::Spinner) {
        return 0.0;
    }

    constexpr double radius = NORMALISED_RADIUS_DOUBLE;
    constexpr double diameter = NORMALISED_DIAMETER;

    // Calculate current velocity
    double curr_velocity = current.lazy_jump_distance / current.adjusted_delta_time();

    // Extend travel velocity through slider into current object
    if (last.base_object->type == HitObjectType::Slider && with_sliders) {
        double travel_velocity = last.travel_distance / last.travel_time;
        double movement_velocity = current.minimum_jump_distance / std::max(1.0, current.minimum_jump_time);
        curr_velocity = std::max(curr_velocity, movement_velocity + travel_velocity);
    }

    // Calculate previous velocity
    double prev_velocity = last.lazy_jump_distance / last.adjusted_delta_time();

    if (last_last.base_object->type == HitObjectType::Slider && with_sliders) {
        double travel_velocity = last_last.travel_distance / last_last.travel_time;
        double movement_velocity = last.minimum_jump_distance / std::max(1.0, last.minimum_jump_time);
        prev_velocity = std::max(prev_velocity, movement_velocity + travel_velocity);
    }

    double wide_angle_bonus = 0.0;
    double acute_angle_bonus = 0.0;
    double slider_bonus = 0.0;
    double velocity_change_bonus = 0.0;
    double wiggle_bonus = 0.0;

    double aim_strain = curr_velocity; // Start strain with regular velocity

    // Angle bonuses
    if (current.angle.has_value() && last.angle.has_value()) {
        double curr_angle = current.angle.value();
        double last_angle = last.angle.value();

        // Rewarding angles, take the smaller velocity as base
        double angle_bonus = std::min(curr_velocity, prev_velocity);

        // Check for same rhythm (within 25% time difference)
        bool same_rhythm = std::max(current.adjusted_delta_time(), last.adjusted_delta_time()) <
                          1.25 * std::min(current.adjusted_delta_time(), last.adjusted_delta_time());

        if (same_rhythm) {
            acute_angle_bonus = calc_acute_angle_bonus(curr_angle);

            // Angle repetition penalty
            acute_angle_bonus *= 0.08 + 0.92 * (1.0 - std::min(acute_angle_bonus,
                std::pow(calc_acute_angle_bonus(last_angle), 3.0)));

            // Apply acute angle bonus for BPM above 300 1/2 and distance more than one diameter
            acute_angle_bonus *= angle_bonus *
                smootherstep(ms_to_bpm(current.adjusted_delta_time(), 2.0), 300.0, 400.0) *
                smootherstep(current.lazy_jump_distance, diameter, diameter * 2.0);
        }

        wide_angle_bonus = calc_wide_angle_bonus(curr_angle);

        // Angle repetition penalty
        wide_angle_bonus *= 1.0 - std::min(wide_angle_bonus,
            std::pow(calc_wide_angle_bonus(last_angle), 3.0));

        wide_angle_bonus *= angle_bonus *
            smootherstep(current.lazy_jump_distance, 0.0, diameter);

        // Check for back-and-forth movement (wiggle reduction)
        if (current_index >= 2) {
            const auto& last_2 = diff_objects[current_index - 2];

            if (last_2.base_object && last.base_object) {
                Vector2 last_2_pos = last_2.base_object->position;
                Vector2 last_pos = last.base_object->position;

                if (last_2.base_object->type == HitObjectType::Slider) {
                    last_2_pos = slider::get_slider_end_position(*last_2.base_object);
                }
                if (last.base_object->type == HitObjectType::Slider) {
                    last_pos = slider::get_slider_end_position(*last.base_object);
                }

                double distance = (last_2_pos - last_pos).length();
                if (distance < 1.0) {
                    wide_angle_bonus *= 1.0 - 0.35 * (1.0 - distance);
                }
            }
        }

        // Wiggle bonus for specific jump patterns
        wiggle_bonus = angle_bonus *
            smootherstep(current.lazy_jump_distance, radius, diameter) *
            std::pow(reverse_lerp(diameter * 3.0, diameter, current.lazy_jump_distance), 1.8) *
            smootherstep(curr_angle, deg_to_rad(110.0), deg_to_rad(60.0)) *
            smootherstep(last.lazy_jump_distance, radius, diameter) *
            std::pow(reverse_lerp(diameter * 3.0, diameter, last.lazy_jump_distance), 1.8) *
            smootherstep(last_angle, deg_to_rad(110.0), deg_to_rad(60.0));
    }

    // Velocity change bonus
    if (std::max(prev_velocity, curr_velocity) != 0.0) {
        // Use average velocity over the whole object
        double prev_avg_velocity = (last.lazy_jump_distance + last_last.travel_distance) /
            last.adjusted_delta_time();
        double curr_avg_velocity = (current.lazy_jump_distance + last.travel_distance) /
            current.adjusted_delta_time();

        double dist_ratio = smootherstep(
            std::abs(prev_avg_velocity - curr_avg_velocity) /
            std::max(prev_avg_velocity, curr_avg_velocity), 0.0, 1.0);

        double overlap_velocity_buff = std::min(diameter * 1.25 /
            std::min(current.adjusted_delta_time(), last.adjusted_delta_time()),
            std::abs(prev_avg_velocity - curr_avg_velocity));

        velocity_change_bonus = overlap_velocity_buff * dist_ratio;

        velocity_change_bonus *= std::pow(
            std::min(current.adjusted_delta_time(), last.adjusted_delta_time()) /
            std::max(current.adjusted_delta_time(), last.adjusted_delta_time()), 2.0);
    }

    // Slider bonus
    if (last.base_object->type == HitObjectType::Slider) {
        slider_bonus = last.travel_distance / last.travel_time;
    }

    // Combine all bonuses
    aim_strain += wiggle_bonus * WIGGLE_MULTIPLIER;
    aim_strain += velocity_change_bonus * VELOCITY_CHANGE_MULTIPLIER;
    aim_strain += std::max(acute_angle_bonus * ACUTE_ANGLE_MULTIPLIER,
                          wide_angle_bonus * WIDE_ANGLE_MULTIPLIER);
    aim_strain *= current.small_circle_bonus;

    if (with_sliders) {
        aim_strain += slider_bonus * SLIDER_MULTIPLIER;
    }

    return aim_strain;
}

// === SPEED EVALUATOR ===
// Based on ppy/osu SpeedEvaluator.cs

inline double evaluate_speed(
    const std::vector<OsuDifficultyHitObject>& diff_objects,
    int current_index,
    double hit_window_great
) {
    if (current_index >= static_cast<int>(diff_objects.size())) {
        return 0.0;
    }

    const auto& current = diff_objects[current_index];

    if (current.base_object->type == HitObjectType::Spinner) {
        return 0.0;
    }

    constexpr double single_spacing_threshold = NORMALISED_DIAMETER * 1.25; // 1.25 circles
    constexpr double min_speed_bonus = 200.0; // 200 BPM 1/4th
    constexpr double speed_balancing_factor = 40.0;
    constexpr double distance_multiplier = 0.8;

    double strain_time = current.adjusted_delta_time();

    // Get doubletapness
    const OsuDifficultyHitObject* next_obj = (current_index + 1 < static_cast<int>(diff_objects.size())) ?
        &diff_objects[current_index + 1] : nullptr;
    double doubletapness = 1.0 - get_doubletapness(current, next_obj);

    // Cap deltatime to the OD 300 hitwindow
    // 0.93 is derived from making sure 260bpm OD8 streams aren't nerfed harshly
    strain_time /= std::clamp((strain_time / hit_window_great) / 0.93, 0.92, 1.0);

    // speedBonus will be 0.0 for BPM < 200
    double speed_bonus = 0.0;

    // Add additional scaling bonus for streams/bursts higher than 200bpm
    double bpm = ms_to_bpm(strain_time, 4.0);
    if (bpm > min_speed_bonus) {
        double min_speed_ms = 60000.0 / (min_speed_bonus * 4.0); // BPMToMilliseconds
        speed_bonus = 0.75 * std::pow((min_speed_ms - strain_time) / speed_balancing_factor, 2.0);
    }

    // Get previous object's travel distance
    double prev_travel_distance = 0.0;
    if (current_index > 0) {
        prev_travel_distance = diff_objects[current_index - 1].travel_distance;
    }

    double distance = prev_travel_distance + current.minimum_jump_distance;

    // Cap distance at single_spacing_threshold
    distance = std::min(distance, single_spacing_threshold);

    // Max distance bonus is 1 * distance_multiplier at single_spacing_threshold
    double distance_bonus = std::pow(distance / single_spacing_threshold, 3.95) * distance_multiplier;

    // Apply reduced small circle bonus (sqrt instead of full value)
    distance_bonus *= std::sqrt(current.small_circle_bonus);

    // Base difficulty with all bonuses
    double difficulty = (1.0 + speed_bonus + distance_bonus) * 1000.0 / strain_time;

    // Apply penalty if there's doubletappable doubles
    return difficulty * doubletapness;
}

// === RHYTHM EVALUATOR ===
// Based on ppy/osu RhythmEvaluator.cs

// Island pattern detection helper class for rhythm evaluation
class Island {
private:
    double delta_difference_epsilon;

public:
    int delta = INT_MAX;
    int delta_count = 0;

    Island(double epsilon) : delta_difference_epsilon(epsilon) {}

    Island(int delta_val, double epsilon) : delta_difference_epsilon(epsilon) {
        delta = std::max(delta_val, static_cast<int>(MIN_DELTA_TIME));
        delta_count = 1;
    }

    void add_delta(int delta_val) {
        if (delta == INT_MAX) {
            delta = std::max(delta_val, static_cast<int>(MIN_DELTA_TIME));
        }
        delta_count++;
    }

    bool is_similar_polarity(const Island& other) const {
        return delta_count % 2 == other.delta_count % 2;
    }

    bool equals(const Island& other) const {
        return std::abs(delta - other.delta) < delta_difference_epsilon &&
               delta_count == other.delta_count;
    }
};

inline double evaluate_rhythm(
    const std::vector<OsuDifficultyHitObject>& diff_objects,
    int current_index
) {
    if (current_index >= static_cast<int>(diff_objects.size())) {
        return 0.0;
    }

    const auto& current = diff_objects[current_index];

    if (current.base_object->type == HitObjectType::Spinner) {
        return 0.0;
    }

    constexpr int history_objects_max = 32;
    constexpr int history_time_max = 5000; // 5 seconds
    constexpr double rhythm_ratio_multiplier = 12.0;
    constexpr double rhythm_overall_multiplier = 0.95;

    double rhythm_complexity_sum = 0.0;
    double delta_difference_epsilon = current.hit_window_great * 0.3;

    Island island(delta_difference_epsilon);
    Island previous_island(delta_difference_epsilon);

    std::vector<std::pair<Island, int>> island_counts;

    double start_ratio = 0.0;
    bool first_delta_switch = false;

    int historical_note_count = std::min(current_index, history_objects_max);

    // Find rhythm start (furthest object to look back to)
    int rhythm_start = 0;
    while (rhythm_start < historical_note_count - 2 &&
           current.start_time - diff_objects[current_index - (rhythm_start + 1)].start_time < history_time_max) {
        rhythm_start++;
    }

    const OsuDifficultyHitObject* prev_obj = (rhythm_start < current_index) ?
        &diff_objects[current_index - (rhythm_start + 1)] : nullptr;
    const OsuDifficultyHitObject* last_obj = (rhythm_start + 1 < current_index) ?
        &diff_objects[current_index - (rhythm_start + 2)] : nullptr;

    // Go from furthest object back to current one
    for (int i = rhythm_start; i > 0; i--) {
        const auto& curr_obj = diff_objects[current_index - i];

        // Scale note 0 to 1 from history to now
        double time_decay = (history_time_max - (current.start_time - curr_obj.start_time)) / history_time_max;
        double note_decay = static_cast<double>(historical_note_count - i) / historical_note_count;
        double curr_historical_decay = std::min(note_decay, time_decay);

        // Use custom cap to ensure delta time is never zero
        double curr_delta = std::max(curr_obj.delta_time, 1e-7);
        double prev_delta = prev_obj ? std::max(prev_obj->delta_time, 1e-7) : curr_delta;
        double last_delta = last_obj ? std::max(last_obj->delta_time, 1e-7) : curr_delta;

        // Calculate how much current delta difference deserves rhythm bonus
        double delta_difference = std::max(prev_delta, curr_delta) / std::min(prev_delta, curr_delta);

        // Take only fractional part (only interested in punishing multiples)
        double delta_difference_fraction = delta_difference - std::trunc(delta_difference);

        double curr_ratio = 1.0 + rhythm_ratio_multiplier *
            std::min(0.5, smoothstep_bell_curve(delta_difference_fraction));

        // Reduce ratio bonus if delta difference is too big
        double difference_multiplier = std::clamp(2.0 - delta_difference / 8.0, 0.0, 1.0);

        double window_penalty = std::min(1.0, std::max(0.0,
            std::abs(prev_delta - curr_delta) - delta_difference_epsilon) / delta_difference_epsilon);

        double effective_ratio = window_penalty * curr_ratio * difference_multiplier;

        if (first_delta_switch) {
            if (std::abs(prev_delta - curr_delta) < delta_difference_epsilon) {
                // Island is still progressing
                island.add_delta(static_cast<int>(curr_delta));
            } else {
                // BPM change into slider = easy acc window
                if (curr_obj.base_object->type == HitObjectType::Slider) {
                    effective_ratio *= 0.125;
                }

                // BPM change from slider = easier than circle -> circle
                if (prev_obj && prev_obj->base_object->type == HitObjectType::Slider) {
                    effective_ratio *= 0.3;
                }

                // Repeated island polarity (2 -> 4, 3 -> 5)
                if (island.is_similar_polarity(previous_island)) {
                    effective_ratio *= 0.5;
                }

                // Previous increase happened a note ago (1/1 -> 1/2 -> 1/4)
                if (last_delta > prev_delta + delta_difference_epsilon &&
                    prev_delta > curr_delta + delta_difference_epsilon) {
                    effective_ratio *= 0.125;
                }

                // Repeated island size (ex: triplet -> triplet)
                if (previous_island.delta_count == island.delta_count) {
                    effective_ratio *= 0.5;
                }

                // Find matching island in counts
                auto it = std::find_if(island_counts.begin(), island_counts.end(),
                    [&island](const auto& pair) { return pair.first.equals(island); });

                if (it != island_counts.end()) {
                    // Only add to island count if they're going one after another
                    if (previous_island.equals(island)) {
                        it->second++;
                    }

                    // Repeated island penalty with logistic scaling
                    double power = logistic(island.delta, 58.33, 0.24, 2.75);
                    effective_ratio *= std::min(3.0 / it->second, std::pow(1.0 / it->second, power));
                } else {
                    island_counts.push_back({island, 1});
                }

                // Scale down for doubletappable objects
                if (prev_obj) {
                    double doubletapness = get_doubletapness(*prev_obj, &curr_obj);
                    effective_ratio *= 1.0 - doubletapness * 0.75;
                }

                rhythm_complexity_sum += std::sqrt(effective_ratio * start_ratio) * curr_historical_decay;

                start_ratio = effective_ratio;
                previous_island = island;

                // If we're slowing down, stop counting
                if (prev_delta + delta_difference_epsilon < curr_delta) {
                    first_delta_switch = false;
                }

                island = Island(static_cast<int>(curr_delta), delta_difference_epsilon);
            }
        } else if (prev_delta > curr_delta + delta_difference_epsilon) {
            // We're speeding up - begin counting island
            first_delta_switch = true;

            // BPM change into slider = easy acc window
            if (curr_obj.base_object->type == HitObjectType::Slider) {
                effective_ratio *= 0.6;
            }

            // BPM change from slider = easier than circle -> circle
            if (prev_obj && prev_obj->base_object->type == HitObjectType::Slider) {
                effective_ratio *= 0.6;
            }

            start_ratio = effective_ratio;
            island = Island(static_cast<int>(curr_delta), delta_difference_epsilon);
        }

        last_obj = prev_obj;
        prev_obj = &curr_obj;
    }

    // Final calculation
    double rhythm_difficulty = std::sqrt(4.0 + rhythm_complexity_sum * rhythm_overall_multiplier) / 2.0;

    // Apply doubletapness for next object
    if (current_index + 1 < static_cast<int>(diff_objects.size())) {
        double doubletapness = get_doubletapness(current, &diff_objects[current_index + 1]);
        rhythm_difficulty *= 1.0 - doubletapness;
    }

    return rhythm_difficulty;
}

// === FLASHLIGHT EVALUATOR ===

inline double evaluate_flashlight(const OsuDifficultyHitObject& current, bool has_flashlight) {
    if (!has_flashlight || current.base_object->type == HitObjectType::Spinner) {
        return 0.0;
    }

    // Flashlight difficulty increases with distance and is affected by time
    double difficulty = current.lazy_jump_distance;

    // Scale difficulty based on how hidden the object is
    double hidden_bonus = std::min(1.0, current.strain_time / 200.0);

    return difficulty * (1.0 + hidden_bonus * 0.35);
}

} // namespace evaluators
} // namespace osupp
