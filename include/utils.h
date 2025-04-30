#pragma once

#include <ctime>
#include <fstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <dpp/snowflake.h>

using json = nlohmann::json;

namespace utils {

  // write_users_json, read_users_json, read_config, emoji_json

  namespace file {
    std::string read(const std::string& path);
  }

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
      j[std::to_string(key)] = value; 
    }

    std::ofstream file(path);
    if (!file.is_open()) {
      spdlog::error("Failed to open {}", path);
      return false;
    }
    file << j.dump(4);
    file.close();

    return true;
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

  time_t ISO8601_to_UNIX(const std::string& datetime);
} // namespace utils
