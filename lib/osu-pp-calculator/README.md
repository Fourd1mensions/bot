# osu! PP Calculator Library

A header-only C++ library for **accurate** performance points (PP) and difficulty calculation for all osu! game modes.

## Features

- **Full beatmap parsing**: Reads `.osu` files to extract all necessary data
- **Accurate difficulty calculation**: Implements official ppy/osu algorithms for aim, speed, and strain
- **Precise PP calculation**: Matches official implementation with 1:1 formula accuracy
- **All game modes**: Supports osu!standard, Taiko, Catch the Beat (Fruits), and Mania
- **Modern C++17**: Clean API with zero external dependencies
- **Header-only**: No compilation required, just include and use
- **Based on official formulas**: Direct port from [ppy/osu](https://github.com/ppy/osu)

## Supported Modes

- **osu!standard**: Full aim, speed, accuracy, and flashlight PP calculation
- **Taiko**: Strain and accuracy-based PP (coming soon)
- **Catch**: Movement and accuracy PP with combo scaling (coming soon)
- **Mania**: Strain and accuracy with different weights (coming soon)

## Usage

```cpp
#include <osu-pp/calculator.h>

// Parse a beatmap from .osu file
auto beatmap = osupp::parse_beatmap("path/to/beatmap.osu");

// Create score info
osupp::ScoreInfo score;
score.accuracy = 0.9850;
score.max_combo = 2000;
score.count_300 = 950;
score.count_100 = 45;
score.count_50 = 3;
score.count_miss = 2;
score.mods.hidden = true;
score.mods.hard_rock = false;
score.mods.double_time = false;

// Calculate difficulty attributes from beatmap
auto difficulty = osupp::calculate_difficulty(beatmap, score.mods);
std::cout << "Star Rating: " << difficulty.star_rating << std::endl;
std::cout << "Aim Difficulty: " << difficulty.aim_difficulty << std::endl;
std::cout << "Speed Difficulty: " << difficulty.speed_difficulty << std::endl;

// Calculate PP
auto result = osupp::calculate_performance(beatmap, score);
std::cout << "Total PP: " << result.total_pp << std::endl;
std::cout << "Aim PP: " << result.aim_pp << std::endl;
std::cout << "Speed PP: " << result.speed_pp << std::endl;
std::cout << "Accuracy PP: " << result.accuracy_pp << std::endl;

// Calculate FC PP (what if no misses)
auto fc_result = osupp::calculate_fc_performance(beatmap, score);
std::cout << "FC PP: " << fc_result.total_pp << std::endl;
```

## Integration with CMake

```cmake
add_subdirectory(lib/osu-pp-calculator)
target_link_libraries(your_target PRIVATE osupp::calculator)
```

## Implementation Details

This library implements the official osu! difficulty and performance calculation algorithms:

### Difficulty Calculation
- Full beatmap object parsing (circles, sliders, spinners)
- Slider path calculation with control points
- Timing point processing (BPM, slider velocity)
- Difficulty object preprocessing (distances, angles, strain times)
- Aim evaluator (angle bonuses, velocity changes, wiggle detection)
- Speed evaluator (rhythm complexity, doubletap detection)
- Flashlight evaluator

### Performance Calculation
- Accurate effective miss count calculation
- Combo scaling with slider breaks
- Length bonuses
- Mod multipliers (HD, HR, DT, FL, NF, SO, etc.)
- Accuracy scaling based on OD
- Individual skill PP values (aim, speed, accuracy, flashlight)

## Accuracy

This library aims for **100% accuracy** compared to official osu!lazer calculations:
- Star rating: Should match within ±0.01 stars
- PP values: Should match within ±0.1 PP

Any deviations are considered bugs and should be reported.

## License

This library implements performance point calculations based on the official osu! implementation:
https://github.com/ppy/osu

## References

- [ppy/osu](https://github.com/ppy/osu) - Official osu! implementation
- [osu-tools](https://github.com/ppy/osu-tools) - Official C# difficulty/PP calculator
- [rosu-pp](https://github.com/MaxOhn/rosu-pp) - High-performance Rust implementation
