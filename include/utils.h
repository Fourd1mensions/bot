#pragma once

#include <ctime>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <chrono>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <dpp/snowflake.h>

using json = nlohmann::json;

struct WebhookConfig {
  std::string mirror_errors;  // Beatmap mirror failure alerts
  std::string general;        // General notifications
  std::string debug;          // Debug/dev notifications
};

struct Config {
  std::string api_v1_key, client_id, client_secret, auth_code, access_token,
      refresh_token, redirect_uri, weather_api_key;
  std::string discord_client_id, discord_client_secret;
  std::string osu_oauth_client_id, osu_oauth_client_secret;  // For user linking via OAuth
  std::string guild_id;
  std::string http_host;
  uint16_t http_port;
  std::string public_url;
  std::vector<std::string> admin_users;
  std::vector<std::string> beatmap_mirrors;
  std::vector<std::string> music_allowed_users;  // Discord IDs allowed to use music player
  WebhookConfig webhooks;
  size_t expires_in, expires_at;
  std::string bot_token;  // Discord bot token for API calls
};

namespace utils {

  namespace file {
    std::string read(const std::string& path);
    bool write(const std::string& path, const json& content);
  } // namespace file

  std::string read_field(const std::string_view key, const std::string& path);

  // <string, string> or <snowflake, string>, path to .json file
  template <typename K, typename V>
  bool map_to_file(const std::unordered_map<K, V>& map, const std::string& path) {
    static_assert(
      (std::is_same_v<K, std::string> || std::is_same_v<K, dpp::snowflake>) &&
      std::is_same_v<V, std::string>,
      "function supports only <string, string> or <snowflake, string> types"
    );

    json j;
    for (const auto& [key, value] : map) {
      if constexpr (std::is_same_v<K, std::string>) {
        j[key] = value;
      } else {
        j[std::to_string(key)] = value;
      }
    }

    return file::write(path, j);
  }

  // <string, string> or <snowflake, string>, path to .json file
  template <typename K, typename V>
  bool file_to_map(std::unordered_map<K, V>& map, const std::string& path) {
    static_assert(
      (std::is_same_v<K, std::string> || std::is_same_v<K, dpp::snowflake>) &&
      std::is_same_v<V, std::string>,
      "function supports only <string, string> or <snowflake, string> types"
    );

    std::string content(file::read(path));
    json j = json::parse(content, nullptr, false);
    if (j.is_discarded()) {
      spdlog::error("Failed to parse {}", path);
      return false;
    }

    map.clear();
    for (const auto& [key, value] : j.items()) {
      map[key] = value;
    }

    return true;
  }

  bool save_config(const Config& config);
  bool load_config(Config& config);
  time_t ISO8601_to_UNIX(const std::string& datetime);
  size_t get_time();
  uint32_t mods_string_to_bitset(const std::string& mods);

  // Format time ago: "5m", "2h", "3d", or "Nov 22 2025"
  std::string format_time_ago(const std::chrono::system_clock::time_point& time_point);

  // URL encode a string (for filenames with spaces, special chars)
  std::string url_encode(const std::string& value);

  // URL decode a string (decode %XX sequences)
  std::string url_decode(const std::string& value);

  // Generate cryptographically secure random token (64 hex chars = 256 bits)
  std::string generate_secure_token();

  // Escape HTML special characters to prevent XSS
  std::string html_escape(const std::string& text);

  // Mod flags for beatmap difficulty calculation
  struct ModFlags {
    bool has_ez = false;
    bool has_hr = false;
    bool has_dt = false;
    bool has_ht = false;
  };

  // Parse mod string (e.g., "HDDT", "HRHD") into mod flags
  ModFlags parse_mod_flags(const std::string& mods);

  // Result of mods validation
  struct ModsValidationResult {
    std::string normalized;           // Cleaned up mods string (uppercase, no duplicates)
    std::vector<std::string> invalid; // List of invalid mod codes found
    bool has_incompatible = false;    // e.g., HR+EZ, DT+HT
    std::string incompatible_msg;     // Description of incompatibility
    bool is_nomod = false;            // True when input was explicitly "NM" (NoMod)

    bool is_valid() const { return invalid.empty() && !has_incompatible; }
  };

  // Valid osu! mod codes
  inline const std::vector<std::string> VALID_MODS = {
    "NF", "EZ", "TD", "HD", "HR", "SD", "DT", "RX", "HT", "NC",
    "FL", "AT", "SO", "AP", "PF", "K4", "K5", "K6", "K7", "K8",
    "FI", "RN", "CN", "TP", "K9", "KC", "K1", "K3", "K2", "SV2", "MR"
  };

  // Validate and normalize mods string
  // Returns validation result with normalized mods and any errors
  ModsValidationResult validate_mods(const std::string& mods);

  // Extract mods from command content (finds +MODS pattern)
  // Returns empty string if no mods found
  std::string extract_mods_from_content(const std::string& content);

  // Get Discord emoji string for osu! rank (X, XH, S, SH, A, B, C, D)
  std::string get_rank_emoji(const std::string& rank);

  // Apply DT/NC/HT speed mods to BPM
  inline float apply_speed_mods_to_bpm(float bpm, const std::string& mods) {
    bool has_dt = mods.find("DT") != std::string::npos || mods.find("NC") != std::string::npos;
    bool has_ht = mods.find("HT") != std::string::npos;

    if (has_dt) {
      return bpm * 1.5f;  // DT/NC increases speed by 50%
    } else if (has_ht) {
      return bpm * 0.75f; // HT decreases speed by 25%
    }
    return bpm;
  }

  // Apply DT/NC/HT speed mods to song length
  inline uint32_t apply_speed_mods_to_length(uint32_t length_seconds, const std::string& mods) {
    bool has_dt = mods.find("DT") != std::string::npos || mods.find("NC") != std::string::npos;
    bool has_ht = mods.find("HT") != std::string::npos;

    if (has_dt) {
      return static_cast<uint32_t>(length_seconds / 1.5f);  // DT/NC shortens map by 1.5x
    } else if (has_ht) {
      return static_cast<uint32_t>(length_seconds / 0.75f); // HT lengthens map by 0.75x
    }
    return length_seconds;
  }

  inline std::string rtrim(std::string s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
      return !std::isspace(ch);
    }).base(), s.end());
    return s;
  }

  inline std::string gamemode_to_string(const std::string& mode) {
    if (mode == "taiko")                        return "Taiko";
    if (mode == "fruits" || mode == "catch")    return "Catch the Beat";
    if (mode == "mania")                        return "osu!Mania";
    return "osu! Standard";
  }

  std::string sanitize_filename(const std::string& filename);

  // Check if content starts with a command prefix (handles Cyrillic case-insensitivity)
  // For ASCII: uses standard tolower comparison
  // For Cyrillic: checks both lowercase and uppercase variants
  // Example: starts_with_command("!ДИ +HD", "!ди") returns true
  bool starts_with_command(const std::string& content, const std::string& prefix);

  // Convert Cyrillic string to lowercase (basic Russian alphabet support)
  // Note: Only handles А-Я -> а-я, not full Unicode case folding
  std::string cyrillic_tolower(const std::string& str);

  // Find the end of command prefix (first space or end of string)
  // Works correctly with UTF-8 strings
  inline size_t find_command_end(const std::string& content) {
    size_t pos = content.find(' ');
    return pos == std::string::npos ? content.length() : pos;
  }

  // Extract arguments after command prefix
  // Example: extract_args("!cmd arg1 arg2") returns "arg1 arg2"
  inline std::string extract_args(const std::string& content) {
    size_t cmd_end = find_command_end(content);
    if (cmd_end >= content.length()) return "";
    // Skip the space after command
    size_t args_start = content.find_first_not_of(" \t", cmd_end);
    if (args_start == std::string::npos) return "";
    return content.substr(args_start);
  }
} // namespace utils
