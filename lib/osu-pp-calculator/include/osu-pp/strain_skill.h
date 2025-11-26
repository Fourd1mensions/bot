#pragma once

#include "difficulty_object.h"
#include "evaluators.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace osupp {

// ============================================================================
// BASE STRAIN SKILL CLASS - 1:1 PORT FROM OFFICIAL
// ============================================================================

class StrainSkill {
protected:
    double current_section_peak = 0.0;
    double current_section_end = 0.0;
    std::vector<double> strain_peaks;
    std::vector<double> object_strains;  // Store individual strains

    constexpr static double SECTION_LENGTH = 400.0;  // milliseconds
    constexpr static double DECAY_WEIGHT = 0.9;

    // Abstract methods that subclasses must implement
    virtual double strain_value_at(const OsuDifficultyHitObject& current) = 0;
    virtual double calculate_initial_strain(double time, const OsuDifficultyHitObject& current) = 0;

    void save_current_peak() {
        strain_peaks.push_back(current_section_peak);
    }

    void start_new_section_from(double time, const OsuDifficultyHitObject& current) {
        // The maximum strain of the new section is not zero by default
        current_section_peak = calculate_initial_strain(time, current);
    }

public:
    virtual ~StrainSkill() = default;

    void process(const OsuDifficultyHitObject& current) {
        // The first object doesn't generate a strain, so we begin with an incremented section end
        if (current.index == 0) {
            current_section_end = std::ceil(current.start_time / SECTION_LENGTH) * SECTION_LENGTH;
        }

        while (current.start_time > current_section_end) {
            save_current_peak();
            start_new_section_from(current_section_end, current);
            current_section_end += SECTION_LENGTH;
        }

        double strain = strain_value_at(current);
        current_section_peak = std::max(strain, current_section_peak);

        // Store the strain value for the object
        object_strains.push_back(strain);
    }

    // Get current strain peaks including the current section
    std::vector<double> get_current_strain_peaks() const {
        std::vector<double> all_peaks = strain_peaks;
        all_peaks.push_back(current_section_peak);
        return all_peaks;
    }

    const std::vector<double>& get_strain_peaks() const {
        return strain_peaks;
    }

    const std::vector<double>& get_object_strains() const {
        return object_strains;
    }

    virtual double difficulty_value() const {
        double difficulty = 0.0;
        double weight = 1.0;

        // Get all peaks and filter out zeros
        auto peaks = get_current_strain_peaks();
        std::vector<double> non_zero_peaks;
        for (double p : peaks) {
            if (p > 0.0) {
                non_zero_peaks.push_back(p);
            }
        }

        // Sort descending
        std::sort(non_zero_peaks.rbegin(), non_zero_peaks.rend());

        // Weighted sum
        for (double strain : non_zero_peaks) {
            difficulty += strain * weight;
            weight *= DECAY_WEIGHT;
        }

        return difficulty;
    }

    // Difficulty to performance conversion (official formula)
    static double difficulty_to_performance(double difficulty) {
        return std::pow(5.0 * std::max(1.0, difficulty / 0.0675) - 4.0, 3.0) / 100000.0;
    }
};

// ============================================================================
// OSU STRAIN SKILL - WITH TOP STRAIN REDUCTION
// ============================================================================

class OsuStrainSkill : public StrainSkill {
protected:
    virtual int get_reduced_section_count() const { return 10; }
    virtual double get_reduced_strain_baseline() const { return 0.75; }

public:
    double difficulty_value() const override {
        double difficulty = 0.0;
        double weight = 1.0;

        // Get all peaks and filter out zeros
        auto peaks = get_current_strain_peaks();
        std::vector<double> non_zero_peaks;
        for (double p : peaks) {
            if (p > 0.0) {
                non_zero_peaks.push_back(p);
            }
        }

        // Sort descending
        std::vector<double> strains = non_zero_peaks;
        std::sort(strains.rbegin(), strains.rend());

        // Apply top strain reduction
        int reduced_count = get_reduced_section_count();
        double baseline = get_reduced_strain_baseline();

        for (int i = 0; i < std::min((int)strains.size(), reduced_count); i++) {
            double progress = static_cast<double>(i) / reduced_count;
            progress = std::clamp(progress, 0.0, 1.0);
            double scale = std::log10(std::lerp(1.0, 10.0, progress));
            strains[i] *= std::lerp(baseline, 1.0, scale);
        }

        // Sort again after reduction (IMPORTANT!)
        std::sort(strains.rbegin(), strains.rend());

        // Weighted sum
        for (double strain : strains) {
            difficulty += strain * weight;
            weight *= DECAY_WEIGHT;
        }

        return difficulty;
    }
};

// ============================================================================
// AIM SKILL
// ============================================================================

class AimSkill : public OsuStrainSkill {
private:
    bool include_sliders;
    double current_strain = 0.0;
    std::vector<double> slider_strains;
    const std::vector<OsuDifficultyHitObject>* diff_objects = nullptr;

    constexpr static double SKILL_MULTIPLIER = 26.0;
    constexpr static double STRAIN_DECAY_BASE = 0.15;

    double strain_decay(double ms) const {
        return std::pow(STRAIN_DECAY_BASE, ms / 1000.0);
    }

protected:
    double calculate_initial_strain(double time, const OsuDifficultyHitObject& current) override {
        // Get previous object's start time
        if (current.index == 0) return 0.0;
        const auto& prev = (*diff_objects)[current.index - 1];
        return current_strain * strain_decay(time - prev.start_time);
    }

    double strain_value_at(const OsuDifficultyHitObject& current) override {
        current_strain *= strain_decay(current.delta_time);
        current_strain += evaluators::evaluate_aim_official(*diff_objects, current.index, include_sliders) * SKILL_MULTIPLIER;

        if (current.base_object->type == HitObjectType::Slider) {
            slider_strains.push_back(current_strain);
        }

        return current_strain;
    }

public:
    AimSkill(bool with_sliders) : include_sliders(with_sliders) {}

    void set_difficulty_objects(const std::vector<OsuDifficultyHitObject>* objects) {
        diff_objects = objects;
    }

    double get_difficult_sliders() const {
        if (slider_strains.empty()) return 0.0;

        double max_slider_strain = *std::max_element(slider_strains.begin(), slider_strains.end());
        if (max_slider_strain == 0.0) return 0.0;

        double count = 0.0;
        for (double strain : slider_strains) {
            double normalized = strain / max_slider_strain * 12.0 - 6.0;
            count += 1.0 / (1.0 + std::exp(-normalized));
        }
        return count;
    }
};

// ============================================================================
// SPEED SKILL
// ============================================================================

class SpeedSkill : public OsuStrainSkill {
private:
    double current_strain = 0.0;
    double current_rhythm = 1.0;
    double hit_window_great;
    std::vector<double> slider_strains;
    const std::vector<OsuDifficultyHitObject>* diff_objects = nullptr;

    constexpr static double SKILL_MULTIPLIER = 1.47;
    constexpr static double STRAIN_DECAY_BASE = 0.3;

    double strain_decay(double ms) const {
        return std::pow(STRAIN_DECAY_BASE, ms / 1000.0);
    }

protected:
    int get_reduced_section_count() const override { return 5; }  // Speed uses 5 instead of 10!

    double calculate_initial_strain(double time, const OsuDifficultyHitObject& current) override {
        if (current.index == 0) return 0.0;
        const auto& prev = (*diff_objects)[current.index - 1];
        return (current_strain * current_rhythm) * strain_decay(time - prev.start_time);
    }

    double strain_value_at(const OsuDifficultyHitObject& current) override {
        current_strain *= strain_decay(current.adjusted_delta_time());
        current_strain += evaluators::evaluate_speed(*diff_objects, current.index, hit_window_great) * SKILL_MULTIPLIER;

        current_rhythm = evaluators::evaluate_rhythm(*diff_objects, current.index);

        double total_strain = current_strain * current_rhythm;

        if (current.base_object->type == HitObjectType::Slider) {
            slider_strains.push_back(total_strain);
        }

        return total_strain;
    }

public:
    SpeedSkill(double hit_window) : hit_window_great(hit_window) {}

    void set_difficulty_objects(const std::vector<OsuDifficultyHitObject>* objects) {
        diff_objects = objects;
    }

    double relevant_note_count() const {
        if (object_strains.empty()) return 0.0;

        double max_strain = *std::max_element(object_strains.begin(), object_strains.end());
        if (max_strain == 0.0) return 0.0;

        double count = 0.0;
        for (double strain : object_strains) {
            double normalized = strain / max_strain * 12.0 - 6.0;
            count += 1.0 / (1.0 + std::exp(-normalized));
        }
        return count;
    }
};

// ============================================================================
// FLASHLIGHT SKILL
// ============================================================================

class FlashlightSkill : public OsuStrainSkill {
private:
    bool has_flashlight;
    double current_strain = 0.0;
    const std::vector<OsuDifficultyHitObject>* diff_objects = nullptr;

    constexpr static double SKILL_MULTIPLIER = 0.15;
    constexpr static double STRAIN_DECAY_BASE = 0.15;

    double strain_decay(double ms) const {
        return std::pow(STRAIN_DECAY_BASE, ms / 1000.0);
    }

protected:
    int get_reduced_section_count() const override { return 0; }  // No reduction for flashlight
    double get_reduced_strain_baseline() const override { return 1.0; }

    double calculate_initial_strain(double time, const OsuDifficultyHitObject& current) override {
        if (current.index == 0 || !diff_objects) return 0.0;
        const auto& prev = (*diff_objects)[current.index - 1];
        return current_strain * strain_decay(time - prev.start_time);
    }

    double strain_value_at(const OsuDifficultyHitObject& current) override {
        current_strain *= strain_decay(current.delta_time);
        current_strain += evaluators::evaluate_flashlight(current, has_flashlight) * SKILL_MULTIPLIER;
        return current_strain;
    }

public:
    FlashlightSkill(bool flashlight_enabled) : has_flashlight(flashlight_enabled) {}

    void set_difficulty_objects(const std::vector<OsuDifficultyHitObject>* objects) {
        diff_objects = objects;
    }
};

} // namespace osupp
