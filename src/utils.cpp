#include <utils.h>

std::string utils::file::read(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    spdlog::error("Failed to open {}", path);
    return {};
  }
  std::string result;
  file >> result;

  return result;
}

std::string read_field(const std::string_view key, const std::string& path) {
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

time_t utils::ISO8601_to_UNIX(const std::string& datetime) {
  std::tm tm = {};
  std::istringstream ss(datetime);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return std::mktime(&tm) - timezone;
}
