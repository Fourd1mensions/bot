#pragma once

#include <ctime>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <dpp/snowflake.h>

using json = nlohmann::json;

// TODO: bring back Config to request.h after rewrite save_config()
struct Config {
  std::string api_v1_key, client_id, client_secret, auth_code, access_token,
      refresh_token, redirect_uri, weather_api_key;
  std::vector<std::string> admin_users;
  size_t expires_in, expires_at;
};

namespace utils {

  // TODO: read_config, emoji_json

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
  // TODO: rewrite this function
  bool save_config(const Config& config);
  bool load_config(Config& config);
  time_t ISO8601_to_UNIX(const std::string& datetime);
  size_t get_time();
} // namespace utils
