#include <utils.h>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>
#include <openssl/rand.h>
#include <fmt/format.h>

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
    config.discord_client_id     = j.value("DISCORD_CLIENT_ID", "");
    config.discord_client_secret = j.value("DISCORD_CLIENT_SECRET", "");
    config.osu_oauth_client_id     = j.value("OSU_OAUTH_CLIENT_ID", "");
    config.osu_oauth_client_secret = j.value("OSU_OAUTH_CLIENT_SECRET", "");
    config.guild_id              = j.value("GUILD_ID", "");
    config.expires_at     = j.value("EXPIRES_AT", 0);
    config.bot_token     = j.value("DISCORD_TOKEN", "");

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
        "https://catboy.best/d"
      };
    }

    // Load command prefix (default: "!")
    config.command_prefix = j.value("COMMAND_PREFIX", "!");

    // Load music allowed users
    if (j.contains("MUSIC_ALLOWED_USERS") && j["MUSIC_ALLOWED_USERS"].is_array()) {
      config.music_allowed_users = j["MUSIC_ALLOWED_USERS"].get<std::vector<std::string>>();
    }

    // Load webhooks (optional)
    if (j.contains("WEBHOOKS") && j["WEBHOOKS"].is_object()) {
      auto& wh = j["WEBHOOKS"];
      config.webhooks.mirror_errors = wh.value("MIRROR_ERRORS", "");
      config.webhooks.general = wh.value("GENERAL", "");
      config.webhooks.debug = wh.value("DEBUG", "");
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
  if (ss.fail()) {
    spdlog::warn("[utils] Failed to parse ISO8601 datetime: '{}'", datetime);
    return 0;
  }
  time_t result = std::mktime(&tm);
  if (result == -1) {
    spdlog::warn("[utils] mktime failed for datetime: '{}'", datetime);
    return 0;
  }
  return result - timezone;
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

std::string utils::generate_secure_token() {
  unsigned char buffer[32];  // 256 bits of entropy
  if (RAND_bytes(buffer, sizeof(buffer)) != 1) {
    // Fallback to /dev/urandom if OpenSSL fails
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.is_open()) {
      urandom.read(reinterpret_cast<char*>(buffer), sizeof(buffer));
    } else {
      throw std::runtime_error("Failed to generate secure random token");
    }
  }
  std::string result;
  result.reserve(64);
  for (size_t i = 0; i < sizeof(buffer); ++i) {
    result += fmt::format("{:02x}", buffer[i]);
  }
  return result;
}

std::string utils::html_escape(const std::string& text) {
  std::string escaped;
  escaped.reserve(text.size() * 1.1);  // Slight buffer for escapes
  for (char c : text) {
    switch (c) {
      case '&':  escaped += "&amp;";  break;
      case '<':  escaped += "&lt;";   break;
      case '>':  escaped += "&gt;";   break;
      case '"':  escaped += "&quot;"; break;
      case '\'': escaped += "&#39;";  break;
      default:   escaped += c;        break;
    }
  }
  return escaped;
}

utils::ModFlags utils::parse_mod_flags(const std::string& mods) {
  ModFlags flags;
  flags.has_ez = mods.find("EZ") != std::string::npos;
  flags.has_hr = mods.find("HR") != std::string::npos;
  flags.has_dt = mods.find("DT") != std::string::npos || mods.find("NC") != std::string::npos;
  flags.has_ht = mods.find("HT") != std::string::npos;
  return flags;
}

utils::ModsValidationResult utils::validate_mods(const std::string& mods) {
  ModsValidationResult result;

  if (mods.empty()) {
    result.normalized = "";
    return result;
  }

  std::string upper = mods;
  std::transform(upper.begin(), upper.end(), upper.begin(),
    [](unsigned char c) { return std::toupper(c); });

  // NM = NoMod, not a real mod
  if (upper == "NM") {
    result.normalized = "";
    result.is_nomod = true;
    return result;
  }

  upper.erase(std::remove_if(upper.begin(), upper.end(),
    [](char c) { return c == ' ' || c == '+'; }), upper.end());

  std::vector<std::string> found_mods;
  std::set<std::string> seen_mods;

  for (size_t i = 0; i < upper.length(); ) {
    // 3-char mods first (SV2)
    if (i + 2 < upper.length()) {
      std::string mod3 = upper.substr(i, 3);
      if (std::find(VALID_MODS.begin(), VALID_MODS.end(), mod3) != VALID_MODS.end()) {
        if (seen_mods.find(mod3) == seen_mods.end()) {
          found_mods.push_back(mod3);
          seen_mods.insert(mod3);
        }
        i += 3;
        continue;
      }
    }

    if (i + 1 < upper.length()) {
      std::string mod2 = upper.substr(i, 2);
      bool is_valid = std::find(VALID_MODS.begin(), VALID_MODS.end(), mod2) != VALID_MODS.end();

      if (!is_valid) {
        result.invalid.push_back(mod2);
      } else if (seen_mods.find(mod2) == seen_mods.end()) {
        found_mods.push_back(mod2);
        seen_mods.insert(mod2);
      }
      i += 2;
    } else {
      // Leftover single character
      result.invalid.push_back(std::string(1, upper[i]));
      i += 1;
    }
  }

  bool has_hr = seen_mods.count("HR") > 0;
  bool has_ez = seen_mods.count("EZ") > 0;
  bool has_dt = seen_mods.count("DT") > 0 || seen_mods.count("NC") > 0;
  bool has_ht = seen_mods.count("HT") > 0;

  if (has_hr && has_ez) {
    result.has_incompatible = true;
    result.incompatible_msg = "HR and EZ cannot be used together";
  } else if (has_dt && has_ht) {
    result.has_incompatible = true;
    result.incompatible_msg = "DT/NC and HT cannot be used together";
  }

  // Build normalized string
  for (const auto& mod : found_mods) {
    result.normalized += mod;
  }

  return result;
}

std::string utils::extract_mods_from_content(const std::string& content) {
  size_t plus_pos = content.find('+');
  if (plus_pos == std::string::npos) {
    return "";
  }

  // Find end of mods (next space or end of string)
  size_t end_pos = content.find(' ', plus_pos);
  if (end_pos == std::string::npos) {
    end_pos = content.length();
  }

  return content.substr(plus_pos + 1, end_pos - plus_pos - 1);
}

std::string utils::sanitize_filename(const std::string& filename) {
  std::string result;
  result.reserve(filename.size() * 2);  // May expand due to multi-byte chars
  for (char c : filename) {
    switch (c) {
      case '"':  result += "\xE2\x80\x9D"; break;  // " -> "
      case '/':  result += "\xE2\x88\x95"; break;  // / -> ∕
      case '\\': result += "\xE2\x88\x96"; break;  // \ -> ∖
      case ':':  result += "\xEF\xBC\x9A"; break;  // : -> ：
      case '*':  result += "\xE2\x9C\xB1"; break;  // * -> ✱
      case '?':  result += "\xEF\xBC\x9F"; break;  // ? -> ？
      case '<':  result += "\xEF\xBC\x9C"; break;  // < -> ＜
      case '>':  result += "\xEF\xBC\x9E"; break;  // > -> ＞
      case '|':  result += "\xEF\xBD\x9C"; break;  // | -> ｜
      default:   result += c; break;
    }
  }
  return result;
}

std::string utils::get_rank_emoji(const std::string& rank) {
  static const std::unordered_map<std::string, std::string> rank_emojis = {
    {"XH", "<:rankingSSH:1320169012810514532>"},
    {"X",  "<:rankingSS:1320169011552313404>"},
    {"SH", "<:rankingSH:1320169010814210048>"},
    {"S",  "<:rankingS:1320169009434132501>"},
    {"A",  "<:rankingA:1320169005894787162>"},
    {"B",  "<:rankingB:1320169007396704286>"},
    {"C",  "<:rankingC:1320169008491585607>"},
    {"D",  "<:rankingD:1320169004011819008>"},
    {"F",  "<:rankingD:1320169004011819008>"}
  };

  auto it = rank_emojis.find(rank);
  return it != rank_emojis.end() ? it->second : rank;
}

std::string utils::cyrillic_tolower(const std::string& str) {
  std::string result;
  result.reserve(str.size());

  for (size_t i = 0; i < str.size(); ) {
    unsigned char c = str[i];

    // Check for 2-byte UTF-8 sequence (Cyrillic is in this range)
    if ((c & 0xE0) == 0xC0 && i + 1 < str.size()) {
      unsigned char c2 = str[i + 1];

      // Russian uppercase А-Я (U+0410-U+042F) -> lowercase а-я (U+0430-U+044F)
      // UTF-8: D0 90 - D0 AF (А-Я except Ё)
      // UTF-8: D0 81 (Ё) -> D1 91 (ё)
      if (c == 0xD0) {
        if (c2 >= 0x90 && c2 <= 0x9F) {
          // А-П (U+0410-U+041F) -> а-п (U+0430-U+043F)
          result += static_cast<char>(0xD0);
          result += static_cast<char>(c2 + 0x20);
          i += 2;
          continue;
        } else if (c2 >= 0xA0 && c2 <= 0xAF) {
          // Р-Я (U+0420-U+042F) -> р-я (U+0440-U+044F)
          result += static_cast<char>(0xD1);
          result += static_cast<char>(c2 - 0x20);
          i += 2;
          continue;
        } else if (c2 == 0x81) {
          // Ё (U+0401) -> ё (U+0451)
          result += static_cast<char>(0xD1);
          result += static_cast<char>(0x91);
          i += 2;
          continue;
        }
      }

      // Not a Russian uppercase letter, copy as-is
      result += str[i];
      result += str[i + 1];
      i += 2;
    } else if ((c & 0x80) == 0) {
      // ASCII character - use standard tolower
      result += static_cast<char>(std::tolower(c));
      ++i;
    } else {
      // Other multi-byte sequence, copy as-is
      result += str[i];
      ++i;
    }
  }

  return result;
}

bool utils::starts_with_command(const std::string& content, const std::string& prefix) {
  if (content.length() < prefix.length()) return false;

  // Convert both to lowercase for comparison
  std::string content_lower = cyrillic_tolower(content.substr(0, prefix.length() + 10)); // Extra chars for safety
  std::string prefix_lower = cyrillic_tolower(prefix);

  // Check if content starts with prefix
  if (content_lower.find(prefix_lower) != 0) return false;

  // Make sure command is followed by space or end of string
  if (content.length() > prefix.length()) {
    size_t prefix_byte_len = prefix.length();
    // Find actual byte position after prefix in original content
    // by matching the lowercased prefix length
    if (content[prefix_byte_len] != ' ' && content[prefix_byte_len] != '\t') {
      // Check if this is just end of string or part of another word
      // For prefixes like "!rs", "!rs123" should not match
      // But we need to be careful with UTF-8
      return false;
    }
  }

  return true;
}
