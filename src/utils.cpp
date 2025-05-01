#include <utils.h>

#include <fstream>

std::string utils::file::read(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) {
    spdlog::error("Failed to open {}", path);
    return {};
  }
  std::string result;
  f >> result;
  return result;
}

bool utils::file::write(const std::string& path, const json& content) {
  std::ofstream f(path);
  if (!f.is_open()) {
    spdlog::error("Failed to open {}", path);
    return false;
  }
  f << content.dump(4);
  f.close();
  return true;
}

std::string utils::read_field(const std::string_view key, const std::string& path) {
  std::string content(utils::file::read(path));
  json j = json::parse(content, nullptr, false);
  if (j.is_discarded()) {
    spdlog::error("Failed to parse {}", path);
    return {};
  }

  std::string result(j.value(key, ""));
  if (result.empty()) {
    spdlog::error("Failed to read key \"{}\" from {}", key, path);
    return "";
  }

  return result;
}

bool utils::save_config(const Config& config) {
  const auto path = "config.json";
  const auto content = file::read(path);  

  try {
    json j = json::parse(content);

    j["AUTH_CODE"]     = config.auth_code;
    j["ACCESS_TOKEN"]  = config.access_token;
    j["REFRESH_TOKEN"] = config.refresh_token;
    j["EXPIRES_IN"]    = config.expires_in;
    return file::write(path, j);
  } catch (json::exception e) {
    spdlog::error("{}", e.what());
    return false;
  }
}


time_t utils::ISO8601_to_UNIX(const std::string& datetime) {
  std::tm tm = {};
  std::istringstream ss(datetime);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return std::mktime(&tm) - timezone;
}
