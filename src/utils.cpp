#include <utils.h>

#include <fstream>
#include <sstream>
#include <iomanip>

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

    // Load HTTP server settings with defaults
    config.http_host = j.value("HTTP_HOST", "127.0.0.1");
    config.http_port = j.value("HTTP_PORT", 8080);
    config.public_url = j.value("PUBLIC_URL", "https://kana.nisemonic.net");

    // Load beatmap mirrors with defaults
    if (j.contains("BEATMAP_MIRRORS") && j["BEATMAP_MIRRORS"].is_array()) {
      config.beatmap_mirrors = j["BEATMAP_MIRRORS"].get<std::vector<std::string>>();
    } else {
      // Default mirrors in priority order
      config.beatmap_mirrors = {
        "https://api.nerinyan.moe/d",
        "https://catboy.best/d",
        "https://api.chimu.moe/v1/download"
      };
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

std::string utils::format_time_ago(const std::chrono::system_clock::time_point& time_point) {
  auto now = std::chrono::system_clock::now();
  auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - time_point);

  // Less than 1 hour: show minutes
  if (diff < std::chrono::hours(1)) {
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(diff).count();
    return std::to_string(minutes) + "m";
  }

  // Less than 1 day: show hours
  if (diff < std::chrono::hours(24)) {
    auto hours = std::chrono::duration_cast<std::chrono::hours>(diff).count();
    return std::to_string(hours) + "h";
  }

  // Less than 1 week: show days
  if (diff < std::chrono::hours(24 * 7)) {
    auto days = std::chrono::duration_cast<std::chrono::hours>(diff).count() / 24;
    return std::to_string(days) + "d";
  }

  // More than 1 week: show date in format "Nov 22 2025"
  std::time_t time = std::chrono::system_clock::to_time_t(time_point);
  std::tm tm = *std::localtime(&time);

  const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%s %d %d",
                months[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);

  return std::string(buffer);
}

std::string utils::url_encode(const std::string& value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (char c : value) {
    // Keep alphanumeric and other safe characters intact
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped << c;
    } else {
      // Any other characters are percent-encoded
      escaped << std::uppercase;
      escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
      escaped << std::nouppercase;
    }
  }

  return escaped.str();
}

std::string utils::url_decode(const std::string& value) {
  std::ostringstream decoded;

  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] == '%' && i + 2 < value.length()) {
      // Decode %XX sequence
      std::string hex = value.substr(i + 1, 2);
      try {
        int ch = std::stoi(hex, nullptr, 16);
        decoded << static_cast<char>(ch);
        i += 2;
      } catch (...) {
        // Invalid hex sequence, keep as-is
        decoded << value[i];
      }
    } else if (value[i] == '+') {
      // Some URL encoders use + for space
      decoded << ' ';
    } else {
      decoded << value[i];
    }
  }

  return decoded.str();
}

utils::ModFlags utils::parse_mod_flags(const std::string& mods) {
  ModFlags flags;
  flags.has_ez = mods.find("EZ") != std::string::npos;
  flags.has_hr = mods.find("HR") != std::string::npos;
  flags.has_dt = mods.find("DT") != std::string::npos || mods.find("NC") != std::string::npos;
  flags.has_ht = mods.find("HT") != std::string::npos;
  return flags;
}
