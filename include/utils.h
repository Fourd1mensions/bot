#pragma once

#include <fstream>
#include <type_traits>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <dpp/snowflake.h>

using json = nlohmann::json;

namespace utils {
// write_users_json, read_users_json, read_config, emoji_json

// <string, string> or <snowflake, string>, path to .json file
template <typename K, typename V>
static bool map_to_file(const std::string& dst_path, const std::unordered_map<K, V>& src_map) {
  static_assert(
    (std::is_same_v<K, std::string> || std::is_same_v<K, dpp::snowflake>) &&
    std::is_same_v<V, std::string>,
    "function supports only <string, string> or <snowflake, string> types"
  );

  json j;
  for (const auto& [key, value] : src_map) {
    j[std::to_string(key)] = value; 
  }

  std::ofstream file(dst_path);
  if (!file.is_open()) {
    spdlog::error("Failed to open {}", dst_path);
    return false;
  }
  file << j.dump(4);
  file.close();
  return true;
}

// <string, string> or <snowflake, string>, path to .json file
template <typename K, typename V>
static bool file_to_map(std::unordered_map<K, V>& dst_map, const std::string& src_path) {
  static_assert(
    (std::is_same_v<K, std::string> || std::is_same_v<K, dpp::snowflake>) &&
    std::is_same_v<V, std::string>,
    "function supports only <string, string> or <snowflake, string> types"
  );

  std::ifstream file(src_path);
  if (!file.is_open()) {
    spdlog::error("Failed to open {}", src_path);
    return false;
  }

  json j = json::parse(file, nullptr, false);
  file.close();
  if (j.is_discarded()) {
    spdlog::error("Failed to parse {}", src_path);
    return false;
  }

  dst_map.clear();
  for (const auto& [key, value] : j.items()) {
    dst_map[key] = value;
  }
  return true;
}
} // namespace utils
