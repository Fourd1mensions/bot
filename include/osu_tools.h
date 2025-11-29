#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace osu_tools {

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace rng = std::ranges;
namespace vws = std::views;

// ============================================================================
// Result Structures
// ============================================================================

/// @brief Difficulty calculation results for a beatmap
struct DifficultyResult {
  double star_rating{0.0};
  double aim_difficulty{0.0};
  double speed_difficulty{0.0};
  int    max_combo{0};
};

/// @brief Performance calculation results including PP breakdown
struct PerformanceResult {
  double           pp{0.0};
  double           aim_pp{0.0};
  double           speed_pp{0.0};
  double           accuracy_pp{0.0};
  DifficultyResult difficulty{};
};

// ============================================================================
// Game Mode Enum
// ============================================================================

/// @brief osu! game modes
enum class GameMode : uint8_t {
  Osu    = 0,
  Taiko  = 1,
  Catch  = 2,
  Mania  = 3
};

/// @brief Convert game mode to string representation
[[nodiscard]] constexpr std::string_view to_string(GameMode mode) noexcept {
  switch (mode) {
    case GameMode::Osu:   return "osu";
    case GameMode::Taiko: return "taiko";
    case GameMode::Catch: return "catch";
    case GameMode::Mania: return "mania";
  }
  return "osu";
}

/// @brief Convert string to game mode
[[nodiscard]] constexpr std::optional<GameMode> from_string(std::string_view mode_str) noexcept {
  if (mode_str == "osu" || mode_str == "std" || mode_str == "standard") {
    return GameMode::Osu;
  }
  if (mode_str == "taiko") {
    return GameMode::Taiko;
  }
  if (mode_str == "catch" || mode_str == "fruits" || mode_str == "ctb") {
    return GameMode::Catch;
  }
  if (mode_str == "mania") {
    return GameMode::Mania;
  }
  return std::nullopt;
}

// ============================================================================
// Configuration
// ============================================================================

/// @brief Configuration for osu-tools paths
struct ToolsConfig {
  fs::path dotnet_path;
  fs::path calculator_path;

  /// @brief Get default configuration from environment
  [[nodiscard]] static ToolsConfig from_environment() {
    const char* home = std::getenv("HOME");
    if (!home) {
      throw std::runtime_error("HOME environment variable not set");
    }

    return ToolsConfig{
      .dotnet_path = fs::path(home) / ".dotnet" / "dotnet",
      .calculator_path = fs::path("/tmp/osu-tools/PerformanceCalculator/bin/Release/net8.0/PerformanceCalculator.dll")
    };
  }

  /// @brief Validate that paths exist
  [[nodiscard]] bool validate() const noexcept {
    return fs::exists(dotnet_path) && fs::exists(calculator_path);
  }
};

// ============================================================================
// Command Execution
// ============================================================================

namespace detail {

/// @brief RAII wrapper for FILE* from popen
class PipeHandle {
public:
  explicit PipeHandle(const char* command)
    : pipe_(popen(command, "r")) {
    if (!pipe_) {
      throw std::system_error(errno, std::generic_category(),
                             "Failed to open pipe for command");
    }
  }

  ~PipeHandle() {
    if (pipe_) {
      pclose(pipe_);
    }
  }

  // Non-copyable, movable
  PipeHandle(const PipeHandle&) = delete;
  PipeHandle& operator=(const PipeHandle&) = delete;

  PipeHandle(PipeHandle&& other) noexcept : pipe_(other.pipe_) {
    other.pipe_ = nullptr;
  }

  PipeHandle& operator=(PipeHandle&& other) noexcept {
    if (this != &other) {
      if (pipe_) {
        pclose(pipe_);
      }
      pipe_ = other.pipe_;
      other.pipe_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] FILE* get() const noexcept { return pipe_; }
  [[nodiscard]] explicit operator bool() const noexcept { return pipe_ != nullptr; }

private:
  FILE* pipe_;
};

/// @brief Escape path for shell command (safe for all special characters)
[[nodiscard]] inline std::string shell_escape(std::string_view str) {
  std::string result;
  result.reserve(str.length() * 2);  // Reserve extra space for escaping

  for (char c : str) {
    // In single quotes, only ' needs special handling
    // We close the quote, add escaped single quote, and reopen
    if (c == '\'') {
      result += "'\"'\"'";  // End quote, add ", ', ", start quote
    } else {
      result += c;
    }
  }

  return result;
}

/// @brief Execute shell command and capture output
[[nodiscard]] inline std::optional<std::string> execute_command(std::string_view command) {
  try {
    PipeHandle pipe(command.data());

    std::string result;
    result.reserve(4096); // Reserve reasonable initial capacity

    std::array<char, 256> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
      result += buffer.data();
    }

    return result;
  } catch (const std::system_error& e) {
    spdlog::error("[OSU_TOOLS] Command execution failed: {}", e.what());
    return std::nullopt;
  }
}

/// @brief Parse mods string and convert to command arguments
[[nodiscard]] inline std::string build_mods_args(std::string_view mods) {
  if (mods.empty() || mods == "NM") {
    return "";
  }

  std::string args;
  args.reserve(mods.length() * 5); // Approximate: " -m xx" per mod

  // Parse 2-character mod codes (HD, HR, DT, etc.)
  for (size_t i = 0; i + 1 < mods.length(); i += 2) {
    std::string mod{mods.substr(i, 2)};

    // Convert to lowercase for osu-tools
    rng::transform(mod, mod.begin(), [](unsigned char c) {
      return std::tolower(c);
    });

    args += fmt::format(" -m {}", mod);
  }

  return args;
}

/// @brief Parse JSON response from osu-tools
template<typename T>
concept JsonParseable = requires(const json& j) {
  { T{} } -> std::same_as<T>;
};

template<JsonParseable T>
[[nodiscard]] std::optional<T> parse_json_response(
    std::string_view output,
    auto&& parser_fn) {
  try {
    json j = json::parse(output);
    return parser_fn(j);
  } catch (const json::exception& e) {
    spdlog::error("[OSU_TOOLS] JSON parsing failed: {}", e.what());
    spdlog::debug("[OSU_TOOLS] Output was: {}", output);
    return std::nullopt;
  }
}

} // namespace detail

// ============================================================================
// Main Calculator Class
// ============================================================================

/// @brief High-level interface for osu-tools performance calculator
class PerformanceCalculator {
public:
  /// @brief Construct calculator with default configuration
  PerformanceCalculator()
    : config_(ToolsConfig::from_environment()) {
    if (!config_.validate()) {
      throw std::runtime_error("osu-tools paths are invalid");
    }
  }

  /// @brief Construct calculator with custom configuration
  explicit PerformanceCalculator(ToolsConfig config)
    : config_(std::move(config)) {
    if (!config_.validate()) {
      throw std::runtime_error("osu-tools paths are invalid");
    }
  }

  // ============================================================================
  // Difficulty Calculation
  // ============================================================================

  /// @brief Calculate difficulty (star rating) for a beatmap
  /// @param beatmap_path Path to .osu file or beatmap ID
  /// @param mods Mods string (e.g., "HDDT")
  /// @param ruleset Optional ruleset override for converts
  /// @return Difficulty calculation results
  [[nodiscard]] std::optional<DifficultyResult> calculate_difficulty(
      const fs::path& beatmap_path,
      std::string_view mods = "",
      std::optional<GameMode> ruleset = std::nullopt) const {

    std::string command = fmt::format("{} {} difficulty '{}' -j",
        config_.dotnet_path.string(),
        config_.calculator_path.string(),
        detail::shell_escape(beatmap_path.string()));

    // Add ruleset if specified
    if (ruleset.has_value()) {
      command += fmt::format(" -r {}", static_cast<int>(*ruleset));
    }

    // Add mods
    command += detail::build_mods_args(mods);

    auto output = detail::execute_command(command);
    if (!output.has_value()) {
      return std::nullopt;
    }

    return detail::parse_json_response<DifficultyResult>(*output,
      [](const json& j) -> DifficultyResult {
        return DifficultyResult{
          .star_rating = j.value("star_rating", 0.0),
          .aim_difficulty = j.value("aim_difficulty", 0.0),
          .speed_difficulty = j.value("speed_difficulty", 0.0),
          .max_combo = j.value("max_combo", 0)
        };
      });
  }

  // ============================================================================
  // Performance Simulation
  // ============================================================================

  /// @brief Parameters for performance simulation
  struct SimulationParams {
    double accuracy{1.0};        ///< Accuracy (0.0-1.0)
    int combo{0};                ///< Max combo (0 = beatmap max)
    int misses{0};               ///< Number of misses
    int count_100{-1};           ///< Number of 100s (-1 = auto)
    int count_50{-1};            ///< Number of 50s (-1 = auto)
    int count_geki{-1};          ///< Number of gekis/MAX (mania, -1 = auto)
    int count_katu{-1};          ///< Number of katus/200 (mania, -1 = auto)
    std::string mods{};          ///< Mods string
    double percent_combo{-1.0};  ///< Percent of max combo (-1 = use combo param)
  };

  /// @brief Simulate a play and calculate PP
  /// @param beatmap_path Path to .osu file or beatmap ID
  /// @param mode Game mode
  /// @param params Simulation parameters
  /// @return Performance calculation results
  [[nodiscard]] std::optional<PerformanceResult> simulate_performance(
      const fs::path& beatmap_path,
      GameMode mode,
      const SimulationParams& params) const {

    // Convert accuracy to percentage (0-100)
    const double accuracy_percent = params.accuracy * 100.0;

    std::string command = fmt::format("{} {} simulate {} '{}' -a {} -j",
        config_.dotnet_path.string(),
        config_.calculator_path.string(),
        to_string(mode),
        detail::shell_escape(beatmap_path.string()),
        accuracy_percent);

    // Add mods
    command += detail::build_mods_args(params.mods);

    // Add combo
    if (params.percent_combo >= 0.0 && params.percent_combo <= 100.0) {
      command += fmt::format(" -C {}", params.percent_combo);
    } else if (params.combo > 0) {
      command += fmt::format(" -c {}", params.combo);
    }

    // Add misses
    if (params.misses > 0) {
      command += fmt::format(" -X {}", params.misses);
    }

    // Add hit counts based on mode
    if (mode == GameMode::Osu || mode == GameMode::Taiko || mode == GameMode::Catch) {
      if (params.count_100 >= 0) {
        command += fmt::format(" -G {}", params.count_100);
      }
      if (params.count_50 >= 0) {
        command += fmt::format(" -M {}", params.count_50);
      }
    } else if (mode == GameMode::Mania) {
      // Mania has different hit judgments
      if (params.count_geki >= 0) {  // MAX/320
        command += fmt::format(" -T {}", params.count_geki);
      }
      if (params.count_100 >= 0) {   // 200
        command += fmt::format(" -G {}", params.count_100);
      }
      if (params.count_katu >= 0) {  // 100
        command += fmt::format(" -O {}", params.count_katu);
      }
      if (params.count_50 >= 0) {    // 50
        command += fmt::format(" -M {}", params.count_50);
      }
    }

    auto output = detail::execute_command(command);
    if (!output.has_value()) {
      return std::nullopt;
    }

    return detail::parse_json_response<PerformanceResult>(*output,
      [](const json& j) -> PerformanceResult {
        const auto& perf_attrs = j.contains("performance_attributes") ? j["performance_attributes"] : j;
        const auto& diff_attrs = j.contains("difficulty_attributes") ? j["difficulty_attributes"] : j;

        return PerformanceResult{
          .pp = perf_attrs.value("pp", 0.0),
          .aim_pp = perf_attrs.value("aim", 0.0),
          .speed_pp = perf_attrs.value("speed", 0.0),
          .accuracy_pp = perf_attrs.value("accuracy", 0.0),
          .difficulty = DifficultyResult{
            .star_rating = diff_attrs.value("star_rating", 0.0),
            .aim_difficulty = diff_attrs.value("aim_difficulty", 0.0),
            .speed_difficulty = diff_attrs.value("speed_difficulty", 0.0),
            .max_combo = diff_attrs.value("max_combo", 0)
          }
        };
      });
  }

  /// @brief Convenience overload with legacy parameter style
  [[nodiscard]] std::optional<PerformanceResult> simulate_performance(
      const fs::path& beatmap_path,
      double accuracy,
      std::string_view mode_str = "osu",
      std::string_view mods = "",
      int combo = 0,
      int misses = 0,
      int count_100 = -1,
      int count_50 = -1) const {

    auto mode = from_string(mode_str).value_or(GameMode::Osu);

    return simulate_performance(beatmap_path, mode, SimulationParams{
      .accuracy = accuracy,
      .combo = combo,
      .misses = misses,
      .count_100 = count_100,
      .count_50 = count_50,
      .mods = std::string(mods)
    });
  }

private:
  ToolsConfig config_;
};

// ============================================================================
// Legacy C-style interface for backward compatibility
// ============================================================================

namespace legacy {

/// @brief Execute command and capture output (legacy interface)
[[nodiscard]] inline std::optional<std::string> execute_command(const std::string& command) {
  return detail::execute_command(command);
}

/// @brief Calculate difficulty (legacy interface)
[[nodiscard]] inline std::optional<DifficultyResult> calculate_difficulty(
    const std::string& osu_file_path,
    const std::string& mods = "") {

  static PerformanceCalculator calc;
  return calc.calculate_difficulty(osu_file_path, mods);
}

/// @brief Simulate performance (legacy interface)
[[nodiscard]] inline std::optional<PerformanceResult> simulate_performance(
    const std::string& osu_file_path,
    double accuracy,
    const std::string& mode = "osu",
    const std::string& mods = "",
    int combo = 0,
    int misses = 0,
    int count_100 = -1,
    int count_50 = -1,
    [[maybe_unused]] double ratio = -1.0) {

  static PerformanceCalculator calc;
  return calc.simulate_performance(osu_file_path, accuracy, mode, mods,
                                   combo, misses, count_100, count_50);
}

} // namespace legacy

// Re-export legacy functions for backward compatibility
using legacy::execute_command;
using legacy::calculate_difficulty;
using legacy::simulate_performance;

} // namespace osu_tools
