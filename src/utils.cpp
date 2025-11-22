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
    config.weather_api_key = j.value("WEATHER_API_KEY", "");
    config.expires_at     = j.value("EXPIRES_AT", 0);

    // Load admin users array
    if (j.contains("ADMIN_USERS") && j["ADMIN_USERS"].is_array()) {
      config.admin_users = j["ADMIN_USERS"].get<std::vector<std::string>>();
    }

    return true;
  } catch(const json::exception& e) {
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

uint32_t utils::mods_string_to_bitset(const std::string& mods) {
  if (mods.empty() || mods == "NM") return 0;

  uint32_t result = 0;
  for (size_t i = 0; i + 1 < mods.length(); i += 2) {
    std::string mod = mods.substr(i, 2);

    if (mod == "NF") result |= 1;
    else if (mod == "EZ") result |= 2;
    else if (mod == "TD") result |= 4;
    else if (mod == "HD") result |= 8;
    else if (mod == "HR") result |= 16;
    else if (mod == "SD") result |= 32;
    else if (mod == "DT") result |= 64;
    else if (mod == "NC") result |= 576;  // NC = 512 + 64 (includes DT)
    else if (mod == "HT") result |= 256;
    else if (mod == "RX") result |= 128;
    else if (mod == "FL") result |= 1024;
    else if (mod == "AT") result |= 2048;
    else if (mod == "SO") result |= 4096;
    else if (mod == "AP") result |= 8192;
    else if (mod == "PF") result |= 16416; // PF = 16384 + 32 (includes SD)
  }
  return result;
}

