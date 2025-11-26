#pragma once

// osu! PP Calculator Library
//
// FULL IMPLEMENTATION
// This library implements the complete osu! difficulty and performance calculation
// algorithms, including full beatmap parsing, slider path calculation, and accurate
// PP calculations matching the official ppy/osu implementation.
//
// Reference implementation: https://github.com/ppy/osu
//
// Features:
// - Full .osu file parsing with support for all object types
// - Accurate slider path calculation (Linear, Bezier, Perfect Circle, Catmull)
// - Complete difficulty attribute calculation (aim, speed, flashlight)
// - Precise PP calculation with all bonuses and penalties
// - Support for all mods (HD, HR, DT, FL, EZ, HT, etc.)
// - Effective miss count calculation with slider breaks
// - Rhythm complexity evaluation
//
// Accuracy: Aims for 100% match with official calculations (±0.01 stars, ±0.1 PP)

#include "types.h"
#include "beatmap_parser.h"
#include "difficulty_utils.h"
#include "slider_path.h"
#include "difficulty_object.h"
#include "evaluators.h"
#include "strain_skill.h"
#include "difficulty_calculator.h"
#include "performance_calculator.h"

namespace osupp {

// Main API - these are the primary functions you'll use

// Parse a beatmap from file
// Usage: auto beatmap = parse_beatmap("path/to/beatmap.osu");
using osupp::parse_beatmap;

// Parse a beatmap from string content
// Usage: auto beatmap = parse_beatmap_content(osu_file_contents);
using osupp::parse_beatmap_content;

// Calculate difficulty attributes (star rating, aim/speed difficulty, etc.)
// Usage: auto difficulty = calculate_difficulty(beatmap, mods);
using osupp::calculate_difficulty;

// Calculate performance points (PP)
// Usage: auto performance = calculate_performance(beatmap, score);
using osupp::calculate_performance;

// Calculate full combo (FC) performance
// Usage: auto fc_pp = calculate_fc_performance(beatmap, score);
using osupp::calculate_fc_performance;

// Example usage:
//
// // 1. Parse beatmap
// auto beatmap = osupp::parse_beatmap("beatmap.osu");
//
// // 2. Set up score info
// osupp::ScoreInfo score;
// score.accuracy = 0.9850;
// score.max_combo = 2000;
// score.count_300 = 950;
// score.count_100 = 45;
// score.count_50 = 3;
// score.count_miss = 2;
// score.mods.hidden = true;
// score.mods.double_time = false;
//
// // 3. Calculate difficulty
// auto difficulty = osupp::calculate_difficulty(beatmap, score.mods);
// std::cout << "Star Rating: " << difficulty.star_rating << std::endl;
// std::cout << "Aim: " << difficulty.aim_difficulty << std::endl;
// std::cout << "Speed: " << difficulty.speed_difficulty << std::endl;
//
// // 4. Calculate PP
// auto result = osupp::calculate_performance(beatmap, score);
// std::cout << "Total PP: " << result.total_pp << std::endl;
// std::cout << "Aim PP: " << result.aim_pp << std::endl;
// std::cout << "Speed PP: " << result.speed_pp << std::endl;
// std::cout << "Accuracy PP: " << result.accuracy_pp << std::endl;
//
// // 5. Calculate FC PP
// auto fc_result = osupp::calculate_fc_performance(beatmap, score);
// std::cout << "FC PP: " << fc_result.total_pp << std::endl;

} // namespace osupp
