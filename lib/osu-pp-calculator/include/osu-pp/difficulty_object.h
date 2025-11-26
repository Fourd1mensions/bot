#pragma once

#include "types.h"
#include "difficulty_utils.h"
#include "slider_path.h"
#include <optional>
#include <algorithm>

namespace osupp {

// Constants from official implementation
constexpr int NORMALISED_RADIUS_INT = 50;
constexpr double NORMALISED_RADIUS_DOUBLE = 50.0;
constexpr int NORMALISED_DIAMETER = NORMALISED_RADIUS_INT * 2;
constexpr int MIN_DELTA_TIME = 25;

// Represents a hit object preprocessed for difficulty calculation
struct OsuDifficultyHitObject {
    const HitObject* base_object = nullptr;
    const HitObject* last_object = nullptr;
    const HitObject* last_last_object = nullptr;

    double start_time = 0.0;
    double end_time = 0.0;
    double delta_time = 0.0;
    double strain_time = 0.0;

    Vector2 start_position;
    Vector2 end_position;
    Vector2 lazy_end_position;

    // Jump distances
    double jump_distance = 0.0;
    double lazy_jump_distance = 0.0;
    double minimum_jump_distance = 0.0;
    double minimum_jump_time = 0.0;

    // Travel distances (for sliders)
    double travel_distance = 0.0;
    double travel_time = 0.0;
    double lazy_travel_distance = 0.0;
    double lazy_travel_time = 0.0;

    // Angle
    std::optional<double> angle;

    // Hit windows
    double hit_window_great = 0.0;

    // Circle size bonus
    double small_circle_bonus = 1.0;

    // Object radius (for calculations)
    double object_radius = OBJECT_RADIUS;

    // Index
    int index = 0;

    OsuDifficultyHitObject() = default;

    double adjusted_delta_time() const {
        return std::max(static_cast<double>(MIN_DELTA_TIME), delta_time);
    }
};

// Build difficulty objects from beatmap
inline std::vector<OsuDifficultyHitObject> build_difficulty_objects(
    const Beatmap& beatmap,
    const Mods& mods
) {
    std::vector<OsuDifficultyHitObject> diff_objects;

    if (beatmap.hit_objects.size() < 2) {
        return diff_objects;
    }

    // Apply mods to difficulty
    auto adjusted_diff = apply_mods_to_difficulty(beatmap.difficulty, mods);
    double clock_rate = mods.get_clock_rate();
    double radius = calculate_radius(adjusted_diff.circle_size);
    double scaling_factor = NORMALISED_RADIUS / radius;

    // Calculate hit windows
    double hit_window_great = od_to_ms_300(adjusted_diff.overall_difficulty) / clock_rate;

    // Get base slider velocity
    double base_slider_velocity = 100.0 * beatmap.difficulty.slider_multiplier / clock_rate;

    const HitObject* last_last = nullptr;
    const HitObject* last = &beatmap.hit_objects[0];

    for (size_t i = 1; i < beatmap.hit_objects.size(); i++) {
        const HitObject* current = &beatmap.hit_objects[i];

        OsuDifficultyHitObject diff_obj;
        diff_obj.base_object = current;
        diff_obj.last_object = last;
        diff_obj.last_last_object = last_last;
        diff_obj.index = static_cast<int>(diff_objects.size());  // Index in diff_objects array, not beatmap!

        diff_obj.start_time = current->start_time / clock_rate;
        diff_obj.start_position = current->position * scaling_factor;  // NORMALIZE position!
        diff_obj.hit_window_great = hit_window_great;

        // Calculate end position and time
        if (current->type == HitObjectType::Slider) {
            diff_obj.end_position = slider::get_slider_end_position(*current) * scaling_factor;  // NORMALIZE!

            // Calculate slider duration
            const TimingPoint* timing_point = get_timing_point_at(beatmap.timing_points, current->start_time);
            double beat_length = timing_point ? timing_point->beat_length : 500.0;
            double slider_velocity_multiplier = get_slider_velocity_at(beatmap.timing_points, current->start_time);

            double velocity = base_slider_velocity * slider_velocity_multiplier;
            double duration = current->pixel_length / velocity * 1000.0; // Convert seconds to milliseconds

            diff_obj.end_time = (current->start_time + duration * current->repeat_count) / clock_rate;
            diff_obj.travel_time = std::max(25.0, duration / clock_rate);
            diff_obj.travel_distance = current->pixel_length * scaling_factor;

            // Lazy travel distance (simplified - assume player takes shortest path)
            diff_obj.lazy_travel_distance = diff_obj.travel_distance;
            diff_obj.lazy_travel_time = diff_obj.travel_time;
            diff_obj.lazy_end_position = diff_obj.end_position;
        } else {
            diff_obj.end_position = current->position * scaling_factor;  // NORMALIZE!
            diff_obj.end_time = diff_obj.start_time;
            diff_obj.lazy_end_position = current->position * scaling_factor;  // NORMALIZE!
        }

        // Calculate delta time
        diff_obj.delta_time = (current->start_time - last->start_time) / clock_rate;
        diff_obj.strain_time = std::max(25.0, diff_obj.delta_time);

        // Get last object's end position (NORMALIZED!)
        Vector2 last_end_pos;
        if (last->type == HitObjectType::Slider) {
            last_end_pos = slider::get_slider_end_position(*last) * scaling_factor;
        } else {
            last_end_pos = last->position * scaling_factor;
        }

        // Calculate jump distance
        Vector2 last_cursor_pos = last_end_pos;
        double last_travel_time = 0.0;

        if (last->type == HitObjectType::Slider) {
            const TimingPoint* timing_point = get_timing_point_at(beatmap.timing_points, last->start_time);
            double beat_length = timing_point ? timing_point->beat_length : 500.0;
            double slider_velocity_multiplier = get_slider_velocity_at(beatmap.timing_points, last->start_time);

            double velocity = base_slider_velocity * slider_velocity_multiplier;
            double duration = last->pixel_length / velocity;
            last_travel_time = std::max(25.0, duration / clock_rate);
        }

        double movement_time = std::max(0.0, diff_obj.strain_time - last_travel_time);
        diff_obj.minimum_jump_time = movement_time;

        // Jump distance is already normalized (both positions are normalized)
        diff_obj.jump_distance = (diff_obj.start_position - last_cursor_pos).length();
        diff_obj.lazy_jump_distance = diff_obj.jump_distance;
        diff_obj.minimum_jump_distance = diff_obj.jump_distance;

        // Calculate minimum jump distance considering lazy movement through slider
        if (last->type == HitObjectType::Slider) {
            // Simplified: use actual end position
            // In real implementation, we'd calculate lazy cursor position
            double slider_travel = last->pixel_length * scaling_factor;
            diff_obj.minimum_jump_distance = std::max(0.0, diff_obj.minimum_jump_distance - slider_travel);
        }

        // Calculate angle if we have 3 objects (use NORMALIZED positions!)
        if (last_last != nullptr) {
            Vector2 last_last_end;
            if (last_last->type == HitObjectType::Slider) {
                last_last_end = slider::get_slider_end_position(*last_last) * scaling_factor;
            } else {
                last_last_end = last_last->position * scaling_factor;
            }

            diff_obj.angle = calculate_angle(last_last_end, last_end_pos, diff_obj.start_position);
        }

        // Calculate small circle bonus
        // Formula: Math.Max(1.0, 1.0 + (30 - BaseObject.Radius) / 40)
        diff_obj.object_radius = radius;
        diff_obj.small_circle_bonus = std::max(1.0, 1.0 + (30.0 - radius) / 40.0);

        diff_objects.push_back(diff_obj);

        last_last = last;
        last = current;
    }

    return diff_objects;
}

} // namespace osupp
