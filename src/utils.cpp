#include <utils.h>

#include <fstream>

using namespace std::chrono;

std::string utils::file::read(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) {
    spdlog::error("Failed to open {}", path);
    return {};
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
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
    j["EXPIRES_AT"]    = config.expires_at;
    return file::write(path, j);
  } catch (json::exception e) {
    spdlog::error("{}", e.what());
    return false;
  }
}

bool utils::load_config(Config& config) {
  const auto path = "config.json";
  const auto content = file::read(path);

  try {
    json j = json::parse(content);
    config.api_v1_key     = j.value("API_V1_KEY", "");
    config.client_id      = j.value("CLIENT_ID", "");
    config.client_secret  = j.value("CLIENT_SECRET", "");
    config.access_token   = j.value("ACCESS_TOKEN", "");
    config.redirect_uri   = j.value("REDIRECT_URI", "");
    config.expires_at     = j.value("EXPIRES_AT", 0);
    return true;
  } catch(json::exception e) {
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

size_t utils::get_time() {
  return static_cast<size_t>(system_clock::to_time_t(system_clock::now()));
}

