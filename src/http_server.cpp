#include <http_server.h>
#include <beatmap_downloader.h>
#include <cache.h>
#include <database.h>
#include <utils.h>
#include <debug_settings.h>
#include <services/message_crawler_service.h>
#include <services/user_settings_service.h>
#include <services/embed_template_service.h>
#include <services/music_player_service.h>

#include <crow.h>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include <string>
#include <unistd.h>
#include <openssl/rand.h>

namespace {

struct SystemMetrics {
  double   cpu_percent = 0.0;
  uint64_t memory_total_kb = 0;
  uint64_t memory_free_kb = 0;
  uint64_t memory_available_kb = 0;
  double   load_avg_1min = 0.0;
  double   load_avg_5min = 0.0;
  double   load_avg_15min = 0.0;
  uint64_t disk_total_kb = 0;
  uint64_t disk_free_kb = 0;
  uint64_t disk_available_kb = 0;
  int      process_pid = 0;
  uint64_t process_threads = 0;
  uint64_t process_memory_kb = 0;
};

SystemMetrics get_system_metrics() {
  SystemMetrics metrics;

  // Memory info
  std::ifstream meminfo("/proc/meminfo");
  if (meminfo.is_open()) {
    std::string line;
    while (std::getline(meminfo, line)) {
      std::istringstream iss(line);
      std::string        key;
      uint64_t           value;
      std::string        unit;

      if (iss >> key >> value >> unit) {
        if (key == "MemTotal:")
          metrics.memory_total_kb = value;
        else if (key == "MemFree:")
          metrics.memory_free_kb = value;
        else if (key == "MemAvailable:")
          metrics.memory_available_kb = value;
      }
    }
  }

  // CPU usage (snapshot)
  std::ifstream stat("/proc/stat");
  if (stat.is_open()) {
    std::string line;
    if (std::getline(stat, line)) {
      std::istringstream iss(line);
      std::string        cpu_label;
      uint64_t user, nice, system, idle;

      if (iss >> cpu_label >> user >> nice >> system >> idle) {
        uint64_t total = user + nice + system + idle;
        uint64_t active = user + nice + system;
        if (total > 0) {
          metrics.cpu_percent = (static_cast<double>(active) / total) * 100.0;
        }
      }
    }
  }

  // Load average
  std::ifstream loadavg("/proc/loadavg");
  if (loadavg.is_open()) {
    loadavg >> metrics.load_avg_1min >> metrics.load_avg_5min >> metrics.load_avg_15min;
  }

  // Disk usage (root filesystem)
  std::ifstream mounts("/proc/mounts");
  if (mounts.is_open()) {
    std::string line;
    while (std::getline(mounts, line)) {
      std::istringstream iss(line);
      std::string device, mount_point, fs_type;
      if (iss >> device >> mount_point >> fs_type) {
        if (mount_point == "/") {
          std::ifstream statvfs_file("/proc/self/mountinfo");
          // Simplified: use df-like calculation
          struct statvfs {
            unsigned long f_bsize;
            unsigned long f_blocks;
            unsigned long f_bfree;
            unsigned long f_bavail;
          };
          break;
        }
      }
    }
  }

  // Process info
  metrics.process_pid = getpid();

  // Thread count
  std::ifstream status("/proc/self/status");
  if (status.is_open()) {
    std::string line;
    while (std::getline(status, line)) {
      std::istringstream iss(line);
      std::string key;
      if (iss >> key) {
        if (key == "Threads:") {
          iss >> metrics.process_threads;
        } else if (key == "VmRSS:") {
          iss >> metrics.process_memory_kb;
        }
      }
    }
  }

  return metrics;
}

using json = nlohmann::json;

std::string extract_cookie(const std::string& cookie_header, const std::string& name) {
  std::string prefix = name + "=";
  size_t pos = 0;
  while ((pos = cookie_header.find(prefix, pos)) != std::string::npos) {
    if (pos == 0 || cookie_header[pos - 1] == ' ' || cookie_header[pos - 1] == ';') {
      size_t start = pos + prefix.size();
      size_t end = cookie_header.find(';', start);
      return cookie_header.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    pos += 1;
  }
  return "";
}

// Generate cryptographically secure random token (32 bytes = 256 bits)
std::string generate_secure_token() {
  unsigned char buffer[32];
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

std::string generate_session_token() {
  return generate_secure_token();
}

struct SessionInfo {
  std::string discord_id;
  std::string role;       // "admin" or "member"
  std::string username;
  std::string avatar;
  std::string access_token;  // Discord OAuth access token (for API calls)
};

// Returns SessionInfo if session is valid, nullopt otherwise.
std::optional<SessionInfo> get_session(const crow::request& req) {
  auto cookie_header = req.get_header_value("Cookie");
  auto token = extract_cookie(cookie_header, "session");
  if (token.empty()) return std::nullopt;

  auto& mc = cache::MemcachedCache::instance();
  auto session_data = mc.get("web_session:" + token);
  if (!session_data) return std::nullopt;

  auto j = json::parse(*session_data, nullptr, false);
  if (j.is_discarded()) return std::nullopt;

  SessionInfo info;
  info.discord_id = j.value("discord_id", "");
  if (info.discord_id.empty()) return std::nullopt;

  info.role = j.value("role", "");
  info.username = j.value("username", "");
  info.avatar = j.value("avatar", "");
  info.access_token = j.value("access_token", "");

  if (info.role.empty()) return std::nullopt;
  return info;
}

// Constants for custom template role checking
constexpr uint64_t CUSTOM_TEMPLATE_REQUIRED_ROLE = 1233831412088438876ULL;
constexpr uint64_t CUSTOM_TEMPLATE_GUILD_ID = 1030424871173361704ULL;

// Super admin who can revert template changes
constexpr uint64_t SUPER_ADMIN_ID = 249958340690575360ULL;

// Check if user is super admin (can revert template changes)
bool is_super_admin(const std::string& discord_id) {
  try {
    return std::stoull(discord_id) == SUPER_ADMIN_ID;
  } catch (...) {
    return false;
  }
}

// Check if user has permission to edit custom templates
// Returns true if user has the required role or higher
// Results are cached for 5 minutes to avoid hitting Discord API rate limits
bool can_edit_custom_templates(const std::string& discord_id, const std::string& bot_token) {
  if (bot_token.empty()) {
    spdlog::warn("[CustomTemplate] Bot token not available for role check");
    return false;
  }

  // Check cache first
  auto& mc = cache::MemcachedCache::instance();
  std::string cache_key = "custom_tmpl_perm:" + discord_id;
  auto cached = mc.get(cache_key);
  if (cached) {
    return *cached == "1";
  }

  // Get member roles from Discord API
  auto response = cpr::Get(
    cpr::Url{fmt::format("https://discord.com/api/v10/guilds/{}/members/{}",
                         CUSTOM_TEMPLATE_GUILD_ID, discord_id)},
    cpr::Header{{"Authorization", "Bot " + bot_token}},
    cpr::Timeout{10000}
  );

  if (response.status_code != 200) {
    spdlog::warn("[CustomTemplate] Failed to fetch member {} roles: HTTP {}", discord_id, response.status_code);
    // Cache negative result for 1 minute on API failure
    mc.set(cache_key, "0", std::chrono::seconds(60));
    return false;
  }

  try {
    auto member = json::parse(response.text);
    if (!member.contains("roles") || !member["roles"].is_array()) {
      mc.set(cache_key, "0", std::chrono::seconds(300));
      return false;
    }

    // Get guild roles to check positions
    auto roles_response = cpr::Get(
      cpr::Url{fmt::format("https://discord.com/api/v10/guilds/{}/roles", CUSTOM_TEMPLATE_GUILD_ID)},
      cpr::Header{{"Authorization", "Bot " + bot_token}},
      cpr::Timeout{10000}
    );

    if (roles_response.status_code != 200) {
      spdlog::warn("[CustomTemplate] Failed to fetch guild roles: HTTP {}", roles_response.status_code);
      mc.set(cache_key, "0", std::chrono::seconds(60));
      return false;
    }

    auto guild_roles = json::parse(roles_response.text);
    if (!guild_roles.is_array()) {
      mc.set(cache_key, "0", std::chrono::seconds(300));
      return false;
    }

    // Find the position of the required role
    int required_role_position = -1;
    std::unordered_map<std::string, int> role_positions;

    for (const auto& role : guild_roles) {
      std::string role_id = role.value("id", "");
      int position = role.value("position", 0);
      role_positions[role_id] = position;

      if (role_id == std::to_string(CUSTOM_TEMPLATE_REQUIRED_ROLE)) {
        required_role_position = position;
      }
    }

    if (required_role_position < 0) {
      spdlog::warn("[CustomTemplate] Required role {} not found in guild", CUSTOM_TEMPLATE_REQUIRED_ROLE);
      mc.set(cache_key, "0", std::chrono::seconds(300));
      return false;
    }

    // Check if user has the required role or any role with higher position
    for (const auto& user_role : member["roles"]) {
      std::string role_id = user_role.get<std::string>();
      auto it = role_positions.find(role_id);
      if (it != role_positions.end() && it->second >= required_role_position) {
        // Cache positive result for 5 minutes
        mc.set(cache_key, "1", std::chrono::seconds(300));
        return true;
      }
    }

    // Cache negative result for 5 minutes
    mc.set(cache_key, "0", std::chrono::seconds(300));
    return false;
  } catch (const std::exception& e) {
    spdlog::error("[CustomTemplate] Error parsing role response: {}", e.what());
    mc.set(cache_key, "0", std::chrono::seconds(60));
    return false;
  }
}

} // namespace

// RateLimiter implementation
bool RateLimiter::allow(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  cleanup_old_requests(client_id);

  auto& client_requests = requests_[client_id];
  if (client_requests.size() >= max_requests_) {
    return false;
  }

  client_requests.push_back(std::chrono::steady_clock::now());
  return true;
}

size_t RateLimiter::remaining(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  cleanup_old_requests(client_id);

  auto it = requests_.find(client_id);
  if (it == requests_.end()) {
    return max_requests_;
  }
  return max_requests_ > it->second.size() ? max_requests_ - it->second.size() : 0;
}

void RateLimiter::cleanup_old_requests(const std::string& client_id) {
  auto it = requests_.find(client_id);
  if (it == requests_.end()) return;

  auto& client_requests = it->second;
  auto now = std::chrono::steady_clock::now();
  auto cutoff = now - window_;

  while (!client_requests.empty() && client_requests.front() < cutoff) {
    client_requests.pop_front();
  }

  // Remove empty entries to prevent memory growth
  if (client_requests.empty()) {
    requests_.erase(it);
  }
}

HttpServer::HttpServer(const std::string& host, uint16_t port,
                       const std::vector<std::string>& mirrors)
    : host_(host), port_(port), app_(std::make_unique<crow::SimpleApp>()),
      downloader_(mirrors.empty() ? std::make_unique<BeatmapDownloader>()
                                   : std::make_unique<BeatmapDownloader>(mirrors)),
      download_limiter_(std::make_unique<RateLimiter>(5, std::chrono::seconds(60))),
      template_save_limiter_(std::make_unique<RateLimiter>(10, std::chrono::seconds(60))),
      music_search_limiter_(std::make_unique<RateLimiter>(5, std::chrono::seconds(10))),
      music_play_limiter_(std::make_unique<RateLimiter>(3, std::chrono::seconds(10))) {

  auto& app = *app_;
  app.signal_clear(); // avoid Crow overriding our SIGINT/SIGTERM handlers

  // Mustache templates compiled once, reused for all requests
  static const auto page_tmpl = crow::mustache::compile(R"({{^session}}<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{{title}}</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Outfit:wght@400;600&family=IBM+Plex+Sans:wght@400;500&display=swap" rel="stylesheet">
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{min-height:100vh;display:flex;align-items:center;justify-content:center;
  background:#050507;color:#e2e0e7;font-family:'IBM Plex Sans',sans-serif}
.card{text-align:center;max-width:420px;padding:2.5rem;
  border-radius:16px;background:rgba(255,255,255,.03);
  border:1px solid rgba(167,139,250,.15)}
h1{font-family:'Outfit',sans-serif;font-size:1.5rem;margin-bottom:.75rem;color:#a78bfa}
p{font-size:.95rem;line-height:1.5;opacity:.7;margin-bottom:1rem}
a.inv{color:#a78bfa;font-weight:500}
a.btn{display:inline-block;margin-top:.5rem;padding:.75rem 2rem;border-radius:8px;
  background:#a78bfa;color:#050507;font-weight:600;text-decoration:none;
  transition:background .2s}
a.btn:hover{background:#c4b5fd}
</style></head><body>
<div class="card">
<h1>Server Membership Required</h1>
<p>You need to be a member of the Discord server to access this page.</p>
<a class="btn" href="https://discord.gg/MV8uVdubeN" target="_blank">Join Server</a>
<p style="margin-top:1.5rem;font-size:.85rem">Already joined? <a class="inv" href="/osu/settings">Log in</a></p>
</div></body></html>{{/session}}{{#session}}{{{content}}}{{/session}})");

  // Helper: serve a protected page with server-side session injection
  // Unauthorized → invite page; authorized → page content with __session injected
  auto serve_page = [](const crow::request& req,
                       const std::vector<std::filesystem::path>& paths,
                       const std::string& title) -> crow::response {
    crow::json::wvalue ctx;
    ctx["title"] = title;

    auto session = get_session(req);
    if (!session) {
      ctx["session"] = false;
      auto body = page_tmpl.render(ctx);
      crow::response res(403);
      res.body = body.dump();
      res.set_header("Content-Type", "text/html; charset=utf-8");
      return res;
    }

    // Find the static file
    std::filesystem::path static_file;
    for (const auto& p : paths) {
      if (std::filesystem::exists(p)) { static_file = p; break; }
    }
    if (static_file.empty()) return crow::response(404, "Page not found");

    std::ifstream file(static_file);
    if (!file) return crow::response(500, "Failed to read page");

    std::stringstream buf;
    buf << file.rdbuf();
    std::string html = buf.str();

    // Inject session data as <script> right after <head> so JS has it immediately
    // Use nlohmann::json for proper escaping (prevents XSS via username etc.)
    json session_json;
    session_json["discord_id"] = session->discord_id;
    session_json["username"] = session->username;
    session_json["avatar"] = session->avatar;
    session_json["role"] = session->role;
    std::string session_script = "<script>window.__session=" + session_json.dump() + ";</script>";

    auto head_pos = html.find("<head>");
    if (head_pos == std::string::npos) head_pos = html.find("<HEAD>");
    if (head_pos != std::string::npos) {
      html.insert(head_pos + 6, "\n" + session_script);
    }

    ctx["session"] = true;
    ctx["content"] = html;
    auto body = page_tmpl.render(ctx);
    crow::response res(200);
    res.body = body.dump();
    res.set_header("Content-Type", "text/html; charset=utf-8");
    return res;
  };

  CROW_ROUTE(app, "/status")
  ([this]() {
    auto now = std::chrono::system_clock::now();
    auto uptime_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch())
            .count() -
        start_time_.load();

    auto metrics = get_system_metrics();

    uint64_t memory_used_kb = metrics.memory_total_kb - metrics.memory_available_kb;
    double   memory_percent = 0.0;
    if (metrics.memory_total_kb > 0) {
      memory_percent = (static_cast<double>(memory_used_kb) /
                       metrics.memory_total_kb) * 100.0;
    }

    crow::json::wvalue response;
    response["status"] = "healthy";
    response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
                                now.time_since_epoch())
                                .count();
    response["uptime_seconds"] = uptime_seconds;

    // System metrics
    response["system"]["cpu"]["percent"] = metrics.cpu_percent;
    response["system"]["load_average"]["1min"] = metrics.load_avg_1min;
    response["system"]["load_average"]["5min"] = metrics.load_avg_5min;
    response["system"]["load_average"]["15min"] = metrics.load_avg_15min;

    response["system"]["memory"]["total_mb"] = metrics.memory_total_kb / 1024.0;
    response["system"]["memory"]["used_mb"] = memory_used_kb / 1024.0;
    response["system"]["memory"]["free_mb"] = metrics.memory_free_kb / 1024.0;
    response["system"]["memory"]["available_mb"] = metrics.memory_available_kb / 1024.0;
    response["system"]["memory"]["percent"] = memory_percent;

    // Process metrics
    response["process"]["pid"] = metrics.process_pid;
    response["process"]["threads"] = static_cast<int64_t>(metrics.process_threads);
    response["process"]["memory_mb"] = metrics.process_memory_kb / 1024.0;

    // Server info
    response["server"]["host"] = host_;
    response["server"]["port"] = port_;

    return crow::response(200, response);
  });

  // Debug settings - GET current state (admin only)
  CROW_ROUTE(app, "/osu/api/debug")
  ([](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, "Unauthorized");
    if (session->role != "admin") return crow::response(403, "Admin access required");

    auto& settings = debug::Settings::instance();

    crow::json::wvalue response;
    response["verbose_osu_api"] = settings.verbose_osu_api.load();
    response["verbose_rosu_pp"] = settings.verbose_rosu_pp.load();
    response["max_response_log_length"] = static_cast<int64_t>(settings.max_response_log_length.load());

    return crow::response(200, response);
  });

  // Debug settings - POST to toggle (admin only)
  CROW_ROUTE(app, "/osu/api/debug").methods("POST"_method)
  ([](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, "Unauthorized");
    if (session->role != "admin") return crow::response(403, "Admin access required");

    auto& settings = debug::Settings::instance();

    try {
      auto body = crow::json::load(req.body);
      if (!body) {
        return crow::response(400, R"({"error": "Invalid JSON"})");
      }

      crow::json::wvalue response;
      response["updated"] = crow::json::wvalue::list();

      if (body.has("verbose_osu_api")) {
        bool val = body["verbose_osu_api"].b();
        settings.verbose_osu_api.store(val);
        spdlog::info("[DEBUG] verbose_osu_api set to {}", val);
        response["verbose_osu_api"] = val;
      }

      if (body.has("verbose_rosu_pp")) {
        bool val = body["verbose_rosu_pp"].b();
        settings.verbose_rosu_pp.store(val);
        spdlog::info("[DEBUG] verbose_rosu_pp set to {}", val);
        response["verbose_rosu_pp"] = val;
      }

      if (body.has("max_response_log_length")) {
        size_t val = static_cast<size_t>(body["max_response_log_length"].i());
        settings.max_response_log_length.store(val);
        spdlog::info("[DEBUG] max_response_log_length set to {}", val);
        response["max_response_log_length"] = static_cast<int64_t>(val);
      }

      return crow::response(200, response);
    } catch (const std::exception& e) {
      spdlog::warn("[DEBUG] Invalid request: {}", e.what());
      return crow::response(400, R"({"error": "Invalid request"})");
    }
  });

  // Commands documentation endpoint
  CROW_ROUTE(app, "/osu/commands")
  ([this]() {
    crow::json::wvalue response;

    // Create array of commands
    std::string p = config_ ? config_->command_prefix : "!";
    crow::json::wvalue::list commands_list;

    // !rs command
    {
      crow::json::wvalue cmd;
      cmd["name"] = p + "rs";
      cmd["aliases"] = crow::json::wvalue::list({p + "rs", p + "кы"});
      cmd["description"] = "Show recent or best scores for a player";
      cmd["usage"] = p + "rs[:mode] [user] [-p] [-b] [-i INDEX] [-m MODE]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = ":mode";
      p1["description"] = "Specify game mode (osu, taiko, catch, mania)";
      p1["example"] = p + "rs:taiko";
      params.push_back(std::move(p1));

      crow::json::wvalue p2;
      p2["flag"] = "user";
      p2["description"] = "Target user (username, Discord mention, or empty for self)";
      p2["example"] = p + "rs peppy  OR  " + p + "rs <@123456789>";
      params.push_back(std::move(p2));

      crow::json::wvalue p3;
      p3["flag"] = "-p";
      p3["description"] = "Show only passed scores (exclude fails)";
      p3["example"] = p + "rs -p";
      params.push_back(std::move(p3));

      crow::json::wvalue p4;
      p4["flag"] = "-b";
      p4["description"] = "Show best scores (top 100) instead of recent";
      p4["example"] = p + "rs -b";
      params.push_back(std::move(p4));

      crow::json::wvalue p5;
      p5["flag"] = "-i INDEX";
      p5["description"] = "Show specific score by index (1-based)";
      p5["example"] = p + "rs -i 5";
      params.push_back(std::move(p5));

      crow::json::wvalue p6;
      p6["flag"] = "-m MODE";
      p6["description"] = "Override game mode (osu, taiko, catch/fruits, mania)";
      p6["example"] = p + "rs -m taiko";
      params.push_back(std::move(p6));

      cmd["parameters"] = std::move(params);

      crow::json::wvalue::list examples;
      examples.push_back(p + "rs");
      examples.push_back(p + "rs -p");
      examples.push_back(p + "rs peppy -i 3");
      examples.push_back(p + "rs:taiko -b");
      examples.push_back(p + "rs <@123456789> -p");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !m / !map command
    {
      crow::json::wvalue cmd;
      cmd["name"] = p + "m";
      cmd["aliases"] = crow::json::wvalue::list({p + "m", p + "map"});
      cmd["description"] = "Show detailed information about the current beatmap";
      cmd["usage"] = p + "m [+MODS]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = "+MODS";
      p1["description"] = "Calculate difficulty with specific mods";
      p1["example"] = p + "m +HDDT";
      params.push_back(std::move(p1));

      cmd["parameters"] = std::move(params);

      crow::json::wvalue::list examples;
      examples.push_back(p + "m");
      examples.push_back(p + "m +HDDT");
      examples.push_back(p + "m +HR");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !sim command
    {
      crow::json::wvalue cmd;
      cmd["name"] = p + "sim";
      cmd["aliases"] = crow::json::wvalue::list({p + "sim"});
      cmd["description"] = "Simulate a score with specific accuracy and parameters";
      cmd["usage"] = p + "sim[:mode] ACCURACY% [+MODS] [-c COMBO] [-n100 X] [-n50 X] [-n0 X] [-r RATIO]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = ":mode";
      p1["description"] = "Specify game mode (osu, taiko, catch, mania)";
      p1["example"] = p + "sim:taiko";
      params.push_back(std::move(p1));

      crow::json::wvalue p2;
      p2["flag"] = "ACCURACY%";
      p2["description"] = "Target accuracy (required)";

      p2["example"] = "99.5%";
      params.push_back(std::move(p2));

      crow::json::wvalue p3;
      p3["flag"] = "+MODS";
      p3["description"] = "Mods to apply";
      p3["example"] = "+HDDT";
      params.push_back(std::move(p3));

      crow::json::wvalue p4;
      p4["flag"] = "-c COMBO";
      p4["description"] = "Specify combo (max combo if omitted)";
      p4["example"] = "-c 1500";
      params.push_back(std::move(p4));

      crow::json::wvalue p5;
      p5["flag"] = "-n100 X";
      p5["description"] = "Number of 100s";
      p5["example"] = "-n100 5";
      params.push_back(std::move(p5));

      crow::json::wvalue p6;
      p6["flag"] = "-n50 X";
      p6["description"] = "Number of 50s";
      p6["example"] = "-n50 2";
      params.push_back(std::move(p6));

      crow::json::wvalue p7;
      p7["flag"] = "-n0 X";
      p7["description"] = "Number of misses";
      p7["example"] = "-n0 3";
      params.push_back(std::move(p7));

      crow::json::wvalue p8;
      p8["flag"] = "-r RATIO";
      p8["description"] = "Ratio for mania (320/300 ratio)";
      p8["example"] = "-r 0.95";
      params.push_back(std::move(p8));

      cmd["parameters"] = std::move(params);

      crow::json::wvalue::list examples;
      examples.push_back(p + "sim 99%");
      examples.push_back(p + "sim 100% +HDDT");
      examples.push_back(p + "sim:taiko 99.5% +HR");
      examples.push_back(p + "sim 99% -n100 5 -c 1500");
      examples.push_back(p + "sim:mania 98% -r 0.95");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !lb command
    {
      crow::json::wvalue cmd;
      cmd["name"] = p + "lb";
      cmd["aliases"] = crow::json::wvalue::list({p + "lb", p + "leaderboard"});
      cmd["description"] = "Show server leaderboard for current beatmap";
      cmd["usage"] = p + "lb [+MODS]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = "+MODS";
      p1["description"] = "Filter by specific mods";
      p1["example"] = p + "lb +HDDT";
      params.push_back(std::move(p1));

      cmd["parameters"] = std::move(params);

      crow::json::wvalue::list examples;
      examples.push_back(p + "lb");
      examples.push_back(p + "lb +HDDT");
      examples.push_back(p + "lb +HR");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !c / !compare command
    {
      crow::json::wvalue cmd;
      cmd["name"] = p + "c";
      cmd["aliases"] = crow::json::wvalue::list({p + "c", p + "compare"});
      cmd["description"] = "Show all scores for a player on current beatmap";
      cmd["usage"] = p + "c [user] [+MODS]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = "user";
      p1["description"] = "Target user (username, Discord mention, or empty for self)";
      p1["example"] = p + "c peppy  OR  " + p + "c <@123456789>";
      params.push_back(std::move(p1));

      crow::json::wvalue p2;
      p2["flag"] = "+MODS";
      p2["description"] = "Filter by specific mods";
      p2["example"] = p + "c +HDDT";
      params.push_back(std::move(p2));

      cmd["parameters"] = std::move(params);

      crow::json::wvalue::list examples;
      examples.push_back(p + "c");
      examples.push_back(p + "c peppy");
      examples.push_back(p + "c +HDDT");
      examples.push_back(p + "c <@123456789> +HR");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !bg command
    {
      crow::json::wvalue cmd;
      cmd["name"] = p + "bg";
      cmd["aliases"] = crow::json::wvalue::list({p + "bg"});
      cmd["description"] = "Get background image from current beatmap";
      cmd["usage"] = p + "bg";
      cmd["parameters"] = crow::json::wvalue::list();

      crow::json::wvalue::list examples;
      examples.push_back(p + "bg");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !song / !audio command
    {
      crow::json::wvalue cmd;
      cmd["name"] = p + "song";
      cmd["aliases"] = crow::json::wvalue::list({p + "song", p + "audio"});
      cmd["description"] = "Get audio file from current beatmap";
      cmd["usage"] = p + "song";
      cmd["parameters"] = crow::json::wvalue::list();

      crow::json::wvalue::list examples;
      examples.push_back(p + "song");
      examples.push_back(p + "audio");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // /set command
    {
      crow::json::wvalue cmd;
      cmd["name"] = "/set";
      cmd["aliases"] = crow::json::wvalue::list({"/set"});
      cmd["description"] = "Link your Discord account to osu! account";
      cmd["usage"] = "/set";
      cmd["parameters"] = crow::json::wvalue::list();

      crow::json::wvalue::list examples;
      examples.push_back("/set");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    response["commands"] = std::move(commands_list);
    response["total_commands"] = static_cast<int>(8);
    response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

    return crow::response(200, response);
  });

  // File inventory endpoint
  CROW_ROUTE(app, "/osu/files")
  ([this]() {
    try {
      auto& db = db::Database::instance();

      crow::json::wvalue response;

      // Get all .osz files
      auto osz_files = db.get_all_beatmap_files();
      crow::json::wvalue::list osz_list;
      int64_t total_osz_size = 0;
      int64_t total_osz_accesses = 0;

      for (const auto& file : osz_files) {
        crow::json::wvalue item;
        item["beatmapset_id"] = file.beatmapset_id;
        item["path"] = file.osz_path;
        item["source"] = file.mirror_hostname.value_or("cache");

        // Get file size from filesystem if not in database
        int64_t file_size = 0;
        if (std::filesystem::exists(file.osz_path)) {
          file_size = std::filesystem::file_size(file.osz_path);
        }
        item["size_bytes"] = file_size;
        item["size_mb"] = static_cast<double>(file_size) / (1024.0 * 1024.0);
        total_osz_size += file_size;

        if (file.last_accessed) {
          auto time_t = std::chrono::system_clock::to_time_t(*file.last_accessed);
          item["last_accessed"] = time_t;
        }

        if (file.created_at) {
          auto time_t = std::chrono::system_clock::to_time_t(*file.created_at);
          item["created_at"] = time_t;
        }

        osz_list.push_back(std::move(item));
      }

      response["osz_files"] = std::move(osz_list);
      response["osz_count"] = static_cast<int>(osz_files.size());
      response["osz_total_size_bytes"] = total_osz_size;
      response["osz_total_size_mb"] = static_cast<double>(total_osz_size) / (1024.0 * 1024.0);
      response["osz_total_size_gb"] = static_cast<double>(total_osz_size) / (1024.0 * 1024.0 * 1024.0);

      // Get statistics via raw SQL query (using storage_stats view)
      try {
        auto stats = db.execute([](pqxx::connection& conn) {
          pqxx::work txn(conn);
          auto result = txn.exec("SELECT * FROM storage_stats ORDER BY file_type");

          crow::json::wvalue::list stats_list;
          for (const auto& row : result) {
            crow::json::wvalue stat;
            stat["file_type"] = row["file_type"].c_str();
            stat["file_count"] = row["file_count"].as<int64_t>();
            stat["total_size_bytes"] = row["total_size_bytes"].as<int64_t>();
            stat["total_size_mb"] = row["total_size_mb"].as<double>();
            stat["total_size_gb"] = row["total_size_gb"].as<double>();

            if (!row["oldest_file"].is_null()) {
              stat["oldest_file"] = row["oldest_file"].c_str();
            }
            if (!row["newest_file"].is_null()) {
              stat["newest_file"] = row["newest_file"].c_str();
            }
            if (!row["total_accesses"].is_null()) {
              stat["total_accesses"] = row["total_accesses"].as<int64_t>();
            }

            stats_list.push_back(std::move(stat));
          }

          return stats_list;
        });

        response["storage_stats"] = std::move(stats);
      } catch (const std::exception& e) {
        spdlog::warn("[HTTP] Failed to get storage stats: {}", e.what());
        response["storage_stats"] = crow::json::wvalue::list();
      }

      // Get pending extracts
      try {
        auto pending_removals = db.get_all_pending_removals();
        response["active_extracts_count"] = static_cast<int>(pending_removals.size());
      } catch (const std::exception& e) {
        spdlog::warn("[HTTP] Failed to get active extracts count: {}", e.what());
        response["active_extracts_count"] = 0;
      }

      response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

      return crow::response(200, response);

    } catch (const std::exception& e) {
      spdlog::error("[API] file-inventory error: {}", e.what());
      crow::json::wvalue error;
      error["error"] = "Internal server error";
      return crow::response(500, error);
    }
  });

  // Serve background image from beatmapset
  CROW_ROUTE(app, "/osu/bg/<int>")
  ([this](int beatmapset_id) {
    try {
      // Check if beatmapset exists
      auto osz_path = downloader_->get_osz_path(beatmapset_id);
      if (!osz_path) {
        return crow::response(404, "Beatmapset not found. Please use the bot to download it first.");
      }

      // Create or get extract
      auto extract_id = downloader_->create_extract(beatmapset_id);
      if (!extract_id) {
        return crow::response(500, "Failed to extract beatmapset");
      }

      auto extract_path = downloader_->get_extract_path(*extract_id);
      if (!extract_path) {
        return crow::response(500, "Failed to get extract path");
      }

      // Find background image (common patterns: .jpg, .jpeg, .png files, usually largest image)
      std::vector<std::pair<std::filesystem::path, uintmax_t>> image_files;
      for (const auto& entry : std::filesystem::directory_iterator(*extract_path)) {
        if (!entry.is_regular_file()) continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
          image_files.emplace_back(entry.path(), entry.file_size());
        }
      }

      if (image_files.empty()) {
        return crow::response(404, "No background image found in beatmapset");
      }

      // Get the largest image (usually the background)
      auto bg_path = std::max_element(image_files.begin(), image_files.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; })->first;

      // Read and serve the file
      std::ifstream file(bg_path, std::ios::binary);
      if (!file) {
        return crow::response(500, "Failed to read background file");
      }

      std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      crow::response res(200, content);

      std::string ext = bg_path.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

      if (ext == ".jpg" || ext == ".jpeg") {
        res.set_header("Content-Type", "image/jpeg");
      } else if (ext == ".png") {
        res.set_header("Content-Type", "image/png");
      }

      res.set_header("Cache-Control", "public, max-age=86400");
      return res;

    } catch (const std::exception& e) {
      spdlog::error("[HTTP] bg error: {}", e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // Serve audio file from beatmapset
  CROW_ROUTE(app, "/osu/audio/<int>")
  ([this](int beatmapset_id) {
    try {
      // Check if beatmapset exists
      auto osz_path = downloader_->get_osz_path(beatmapset_id);
      if (!osz_path) {
        return crow::response(404, "Beatmapset not found. Please use the bot to download it first.");
      }

      // Create or get extract
      auto extract_id = downloader_->create_extract(beatmapset_id);
      if (!extract_id) {
        return crow::response(500, "Failed to extract beatmapset");
      }

      auto extract_path = downloader_->get_extract_path(*extract_id);
      if (!extract_path) {
        return crow::response(500, "Failed to get extract path");
      }

      // Find audio file (.mp3, .ogg)
      std::filesystem::path audio_path;
      for (const auto& entry : std::filesystem::directory_iterator(*extract_path)) {
        if (!entry.is_regular_file()) continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".mp3" || ext == ".ogg") {
          audio_path = entry.path();
          break;
        }
      }

      if (audio_path.empty()) {
        return crow::response(404, "No audio file found in beatmapset");
      }

      // Read and serve the file
      std::ifstream file(audio_path, std::ios::binary);
      if (!file) {
        return crow::response(500, "Failed to read audio file");
      }

      std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      crow::response res(200, content);

      std::string ext = audio_path.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

      if (ext == ".mp3") {
        res.set_header("Content-Type", "audio/mpeg");
      } else if (ext == ".ogg") {
        res.set_header("Content-Type", "audio/ogg");
      }

      res.set_header("Cache-Control", "public, max-age=86400");
      res.set_header("Accept-Ranges", "bytes");
      return res;

    } catch (const std::exception& e) {
      spdlog::error("[HTTP] audio error: {}", e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // Download .osz files directly
  // If cached locally - serve directly
  // If not cached - redirect to mirror and cache in background
  CROW_ROUTE(app, "/osu/d/<int>")
  ([this](const crow::request& req, int beatmapset_id) {
    std::string client_ip = get_client_ip(req);

    // Get .osz file path
    auto osz_path = downloader_->get_osz_path(beatmapset_id);

    // If not found locally, redirect to mirror and cache in background
    if (!osz_path) {
      // Check rate limit for redirects (to prevent abuse)
      if (!download_limiter_->allow(client_ip)) {
        size_t remaining = download_limiter_->remaining(client_ip);
        crow::response res(429, "Rate limit exceeded. Maximum 5 downloads per minute.");
        res.set_header("Retry-After", "60");
        res.set_header("X-RateLimit-Limit", "5");
        res.set_header("X-RateLimit-Remaining", std::to_string(remaining));
        res.set_header("X-RateLimit-Reset", "60");
        return res;
      }

      // Get mirror URL for redirect
      std::string mirror_url = downloader_->get_mirror_url(beatmapset_id);

      spdlog::info("[HTTP] Redirecting client {} to mirror for beatmapset {}", client_ip, beatmapset_id);

      // Start background download to cache the file
      std::thread([this, beatmapset_id]() {
        spdlog::info("[HTTP] Background caching beatmapset {}", beatmapset_id);
        if (downloader_->download_osz(beatmapset_id)) {
          spdlog::info("[HTTP] Successfully cached beatmapset {}", beatmapset_id);
        } else {
          spdlog::warn("[HTTP] Failed to cache beatmapset {}", beatmapset_id);
        }
      }).detach();

      // Redirect user to mirror
      crow::response res(302);
      res.set_header("Location", mirror_url);
      res.set_header("Cache-Control", "no-cache");
      return res;
    }

    if (!std::filesystem::exists(*osz_path) || !std::filesystem::is_regular_file(*osz_path)) {
      return crow::response(404, "Beatmapset file not found on disk");
    }

    // Read file from local cache
    std::ifstream file(*osz_path, std::ios::binary);
    if (!file) {
      return crow::response(500, "Failed to read beatmapset file");
    }

    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // Create response with proper headers for download
    crow::response res;
    res.body = std::string(buffer.begin(), buffer.end());
    res.code = 200;
    res.set_header("Content-Type", "application/x-osu-beatmap-archive");
    res.set_header("Content-Disposition",
                   "attachment; filename=\"" + std::to_string(beatmapset_id) + ".osz\"");
    res.set_header("Content-Length", std::to_string(buffer.size()));
    res.set_header("X-Cache", "HIT");

    return res;
  });

  // ============================================================================
  // Word statistics and crawl status endpoints (MUST be before generic routes)
  // ============================================================================

  // GET /osu/api/guild - Guild info (requires authentication)
  CROW_ROUTE(app, "/osu/api/guild")
  ([](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }
    try {
      // Get guild ID from config
      dpp::snowflake guild_id = utils::read_field("GUILD_ID", "config.json");
      if (guild_id == 0) {
        crow::json::wvalue error;
        error["error"] = "No guild configured";
        return crow::response(404, error);
      }

      auto guild_opt = db::Database::instance().get_guild_info(guild_id);
      if (!guild_opt) {
        crow::json::wvalue error;
        error["error"] = "Guild info not cached yet";
        return crow::response(404, error);
      }

      const auto& guild = *guild_opt;
      crow::json::wvalue response;
      response["guild_id"] = guild.guild_id.str();
      response["name"] = guild.name;
      response["icon_hash"] = guild.icon_hash;
      response["member_count"] = static_cast<int64_t>(guild.member_count);

      return crow::response(200, response);
    } catch (const std::exception& e) {
      crow::json::wvalue error;
      error["error"] = "Internal server error";
      return crow::response(500, error);
    }
  });

  // GET /osu/api/users - List of users with message counts (requires authentication)
  CROW_ROUTE(app, "/osu/api/users")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }
    try {
      if (!crawler_service_) {
        crow::json::wvalue error;
        error["error"] = "Crawler service not available";
        return crow::response(503, error);
      }

      auto authors = crawler_service_->get_message_authors();

      crow::json::wvalue response;
      crow::json::wvalue::list users_list;

      for (const auto& author : authors) {
        crow::json::wvalue user_obj;
        user_obj["user_id"] = author.author_id.str();
        user_obj["message_count"] = static_cast<int64_t>(author.message_count);
        user_obj["username"] = author.username;
        user_obj["display_name"] = author.display_name;
        user_obj["avatar_hash"] = author.avatar_hash;
        users_list.push_back(std::move(user_obj));
      }

      response["users"] = std::move(users_list);
      response["count"] = static_cast<int64_t>(authors.size());
      response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();

      return crow::response(200, response);
    } catch (const std::exception& e) {
      crow::json::wvalue error;
      error["error"] = "Internal server error";
      return crow::response(500, error);
    }
  });

  // GET /osu/api/channels - List of channels with message counts (requires authentication)
  CROW_ROUTE(app, "/osu/api/channels")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }
    try {
      if (!crawler_service_) {
        crow::json::wvalue error;
        error["error"] = "Crawler service not available";
        return crow::response(503, error);
      }

      auto channels = crawler_service_->get_message_channels();

      crow::json::wvalue response;
      crow::json::wvalue::list channels_list;

      for (const auto& channel : channels) {
        crow::json::wvalue channel_obj;
        channel_obj["channel_id"] = channel.channel_id.str();
        channel_obj["message_count"] = static_cast<int64_t>(channel.message_count);
        channel_obj["channel_name"] = channel.channel_name;
        channels_list.push_back(std::move(channel_obj));
      }

      response["channels"] = std::move(channels_list);
      response["count"] = static_cast<int64_t>(channels.size());
      response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();

      return crow::response(200, response);
    } catch (const std::exception& e) {
      crow::json::wvalue error;
      error["error"] = "Internal server error";
      return crow::response(500, error);
    }
  });

  // GET /osu/api/wordstats - JSON with top words (requires authentication)
  CROW_ROUTE(app, "/osu/api/wordstats")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }
    try {
      auto limit_param = req.url_params.get("limit");
      auto lang_param = req.url_params.get("lang");
      auto user_param = req.url_params.get("user_id");
      auto channel_param = req.url_params.get("channel_id");
      auto stopwords_param = req.url_params.get("exclude_stopwords");

      size_t limit = limit_param ? std::stoul(limit_param) : 100;
      std::string language = lang_param ? lang_param : "";
      bool exclude_stopwords = stopwords_param && std::string(stopwords_param) == "true";

      if (limit > 1000) limit = 1000;
      if (limit < 1) limit = 1;

      if (!crawler_service_) {
        crow::json::wvalue error;
        error["error"] = "Crawler service not available";
        return crow::response(503, error);
      }

      std::vector<db::WordStatEntry> words;

      // Handle filtering combinations
      if (user_param && channel_param) {
        // Both user and channel specified
        dpp::snowflake user_id(std::stoull(user_param));
        dpp::snowflake channel_id(std::stoull(channel_param));
        words = crawler_service_->get_user_top_words(user_id, limit, language, exclude_stopwords, channel_id);
      } else if (user_param) {
        // Only user specified
        dpp::snowflake user_id(std::stoull(user_param));
        words = crawler_service_->get_user_top_words(user_id, limit, language, exclude_stopwords);
      } else if (channel_param) {
        // Only channel specified
        dpp::snowflake channel_id(std::stoull(channel_param));
        words = crawler_service_->get_channel_top_words(channel_id, limit, language, exclude_stopwords);
      } else {
        // No filter - global stats
        words = crawler_service_->get_top_words(limit, language, exclude_stopwords);
      }

      crow::json::wvalue response;
      crow::json::wvalue::list words_list;

      size_t total_count = 0;
      for (const auto& entry : words) {
        total_count += entry.count;
      }

      for (const auto& entry : words) {
        crow::json::wvalue word_obj;
        word_obj["word"] = entry.word;
        word_obj["count"] = static_cast<int64_t>(entry.count);
        word_obj["language"] = entry.language;
        word_obj["percentage"] = total_count > 0
            ? (static_cast<double>(entry.count) / total_count * 100.0) : 0.0;
        words_list.push_back(std::move(word_obj));
      }

      // Get unique word count with same filters
      size_t unique_count = crawler_service_->get_unique_word_count(language, exclude_stopwords);

      response["words"] = std::move(words_list);
      response["total_words"] = static_cast<int64_t>(total_count);
      response["unique_words"] = static_cast<int64_t>(unique_count);
      response["count"] = static_cast<int64_t>(words.size());
      response["language_filter"] = language.empty() ? "all" : language;
      response["user_filter"] = user_param ? user_param : "all";
      response["channel_filter"] = channel_param ? channel_param : "all";
      response["exclude_stopwords"] = exclude_stopwords;
      response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();

      return crow::response(200, response);
    } catch (const std::exception& e) {
      spdlog::error("[API] wordstats error: {}", e.what());
      crow::json::wvalue error;
      error["error"] = "Internal server error";
      return crow::response(500, error);
    }
  });

  // GET /osu/api/phrasestats - JSON with top phrases (requires authentication)
  // Parameters:
  //   limit - number of results (default: 100, max: 1000)
  //   lang - language filter ("ru", "en", or empty for all)
  //   sort - sort mode ("count", "pmi", "npmi", "llr", "uniqueness", "trending")
  //   type - phrase type ("all", "bigram", "trigram")
  //   min_count - minimum phrase count (default: 5)
  //   user_id - filter by user
  //   channel_id - filter by channel
  //   filter - special filters ("new" for phrases appeared in last 7 days)
  CROW_ROUTE(app, "/osu/api/phrasestats")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }
    try {
      auto limit_param = req.url_params.get("limit");
      auto lang_param = req.url_params.get("lang");
      auto sort_param = req.url_params.get("sort");
      auto type_param = req.url_params.get("type");
      auto min_count_param = req.url_params.get("min_count");
      auto user_param = req.url_params.get("user_id");
      auto channel_param = req.url_params.get("channel_id");
      auto filter_param = req.url_params.get("filter");

      size_t limit = limit_param ? std::stoul(limit_param) : 100;
      std::string language = lang_param ? lang_param : "";
      std::string sort_mode = sort_param ? sort_param : "count";
      size_t min_count = min_count_param ? std::stoul(min_count_param) : 5;
      std::string filter = filter_param ? filter_param : "";

      // Legacy support: sort=pmi means sort_mode="pmi"
      bool sort_by_pmi = (sort_mode == "pmi");

      int word_count_filter = 0;
      if (type_param) {
        std::string type_str = type_param;
        if (type_str == "bigram") word_count_filter = 2;
        else if (type_str == "trigram") word_count_filter = 3;
      }

      if (limit > 1000) limit = 1000;
      if (limit < 1) limit = 1;

      if (!crawler_service_) {
        crow::json::wvalue error;
        error["error"] = "Crawler service not available";
        return crow::response(503, error);
      }

      std::vector<db::PhraseStatEntry> phrases;

      // Handle filtering combinations
      if (user_param && channel_param) {
        // Both user and channel specified
        dpp::snowflake user_id(std::stoull(user_param));
        dpp::snowflake channel_id(std::stoull(channel_param));
        phrases = crawler_service_->get_user_top_phrases(user_id, limit, language, sort_by_pmi, word_count_filter, min_count, channel_id, sort_mode);
      } else if (user_param) {
        // Only user specified
        dpp::snowflake user_id(std::stoull(user_param));
        phrases = crawler_service_->get_user_top_phrases(user_id, limit, language, sort_by_pmi, word_count_filter, min_count, 0, sort_mode);
      } else if (channel_param) {
        // Only channel specified
        dpp::snowflake channel_id(std::stoull(channel_param));
        phrases = crawler_service_->get_channel_top_phrases(channel_id, limit, language, sort_by_pmi, word_count_filter, min_count);
      } else {
        // No filter - global stats
        phrases = crawler_service_->get_top_phrases(limit, language, sort_by_pmi, word_count_filter, min_count);
      }

      // Apply special filters
      if (filter == "new") {
        std::erase_if(phrases, [](const db::PhraseStatEntry& e) {
          return !e.is_new;
        });
      }

      crow::json::wvalue response;
      crow::json::wvalue::list phrases_list;

      size_t total_count = 0;
      for (const auto& entry : phrases) {
        total_count += entry.count;
      }

      for (const auto& entry : phrases) {
        crow::json::wvalue phrase_obj;
        phrase_obj["phrase"] = entry.phrase;
        phrase_obj["count"] = static_cast<int64_t>(entry.count);
        phrase_obj["word_count"] = entry.word_count;
        phrase_obj["language"] = entry.language;

        // PMI score
        if (entry.pmi_score) {
          phrase_obj["pmi"] = *entry.pmi_score;
        }

        // NPMI score (normalized PMI, [-1, 1])
        if (entry.npmi_score) {
          phrase_obj["npmi"] = *entry.npmi_score;
        }

        // LLR score (log-likelihood ratio)
        if (entry.llr_score) {
          phrase_obj["llr"] = *entry.llr_score;
        }

        // Uniqueness score (for user-specific queries)
        if (entry.uniqueness_score) {
          phrase_obj["uniqueness"] = *entry.uniqueness_score;
        }

        // Trend score (growth rate over last week)
        if (entry.trend_score) {
          phrase_obj["trend_score"] = *entry.trend_score;
        }

        // First seen timestamp
        if (entry.first_seen) {
          auto time_t_val = std::chrono::system_clock::to_time_t(*entry.first_seen);
          std::tm tm = *std::gmtime(&time_t_val);
          char buffer[32];
          std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
          phrase_obj["first_seen"] = std::string(buffer);
        }

        // Is new (appeared in last 7 days)
        phrase_obj["is_new"] = entry.is_new;

        phrase_obj["percentage"] = total_count > 0
            ? (static_cast<double>(entry.count) / total_count * 100.0) : 0.0;
        phrases_list.push_back(std::move(phrase_obj));
      }

      size_t unique_count = (user_param || channel_param) ? phrases.size() : crawler_service_->get_unique_phrase_count(language, word_count_filter);

      response["phrases"] = std::move(phrases_list);
      response["total_phrases"] = static_cast<int64_t>(total_count);
      response["unique_phrases"] = static_cast<int64_t>(unique_count);
      response["count"] = static_cast<int64_t>(phrases.size());
      response["language_filter"] = language.empty() ? "all" : language;
      response["type_filter"] = type_param ? type_param : "all";
      response["user_filter"] = user_param ? user_param : "all";
      response["channel_filter"] = channel_param ? channel_param : "all";
      response["sort_by"] = sort_mode;
      response["filter"] = filter.empty() ? "none" : filter;
      response["min_count"] = static_cast<int64_t>(min_count);
      response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();

      return crow::response(200, response);
    } catch (const std::exception& e) {
      spdlog::error("[API] phrasestats error: {}", e.what());
      crow::json::wvalue error;
      error["error"] = "Internal server error";
      return crow::response(500, error);
    }
  });

  // GET /osu/api/crawl-status - Current crawl progress (requires authentication)
  CROW_ROUTE(app, "/osu/api/crawl-status")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }
    try {
      if (!crawler_service_) {
        crow::json::wvalue error;
        error["error"] = "Crawler service not available";
        return crow::response(503, error);
      }

      auto summary = crawler_service_->get_crawl_summary();
      auto channels = crawler_service_->get_channel_progress();

      crow::json::wvalue response;
      response["is_active"] = crawler_service_->is_running();
      response["total_channels"] = static_cast<int64_t>(summary.total_channels);
      response["completed_channels"] = static_cast<int64_t>(summary.completed_channels);
      response["total_messages"] = static_cast<int64_t>(summary.total_messages);
      response["progress_percent"] = summary.total_channels > 0
          ? (static_cast<double>(summary.completed_channels) / summary.total_channels * 100.0) : 0.0;

      crow::json::wvalue::list channel_list;
      for (const auto& ch : channels) {
        crow::json::wvalue ch_obj;
        ch_obj["channel_id"] = ch.channel_id.str();
        ch_obj["messages_crawled"] = static_cast<int64_t>(ch.total_messages);
        ch_obj["initial_complete"] = ch.initial_crawl_complete;
        if (ch.last_crawl != std::chrono::system_clock::time_point{}) {
          ch_obj["last_crawl"] = std::chrono::duration_cast<std::chrono::seconds>(
              ch.last_crawl.time_since_epoch()).count();
        }
        channel_list.push_back(std::move(ch_obj));
      }

      response["channels"] = std::move(channel_list);
      response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();

      return crow::response(200, response);
    } catch (const std::exception& e) {
      spdlog::error("[API] crawl-status error: {}", e.what());
      crow::json::wvalue error;
      error["error"] = "Internal server error";
      return crow::response(500, error);
    }
  });

  // POST /osu/api/refresh-stats - Trigger immediate stats recalculation (admin only)
  CROW_ROUTE(app, "/osu/api/refresh-stats").methods("POST"_method)
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, "Unauthorized");
    if (session->role != "admin") return crow::response(403, "Admin access required");

    try {
      if (!crawler_service_) {
        crow::json::wvalue error;
        error["error"] = "Crawler service not available";
        return crow::response(503, error);
      }

      crawler_service_->trigger_stats_refresh();

      crow::json::wvalue response;
      response["success"] = true;
      response["message"] = "Stats refresh triggered";
      return crow::response(200, response);
    } catch (const std::exception& e) {
      spdlog::error("[API] refresh-stats error: {}", e.what());
      crow::json::wvalue error;
      error["error"] = "Internal server error";
      return crow::response(500, error);
    }
  });

  // GET /osu/wordstats - server-rendered HTML page
  CROW_ROUTE(app, "/osu/wordstats")
  ([serve_page](const crow::request& req) {
    return serve_page(req, {
      "static/wordstats.html", "../static/wordstats.html",
      "/home/nisemonic/patchouli/bot/static/wordstats.html"
    }, "Word Stats — Patchouli");
  });

  // GET /osu/phrasestats - server-rendered HTML page
  CROW_ROUTE(app, "/osu/phrasestats")
  ([serve_page](const crow::request& req) {
    return serve_page(req, {
      "static/phrasestats.html", "../static/phrasestats.html",
      "/home/nisemonic/patchouli/bot/static/phrasestats.html"
    }, "Phrase Stats — Patchouli");
  });

  // Discord OAuth2: redirect to Discord authorize page
  CROW_ROUTE(app, "/osu/auth/discord")
  ([this]() {
    if (!config_ || config_->discord_client_id.empty()) {
      return crow::response(500, "Discord OAuth2 not configured");
    }

    auto state = generate_session_token();
    auto& mc = cache::MemcachedCache::instance();
    if (!mc.set("oauth_state:" + state, "1", std::chrono::seconds(300))) {
      spdlog::error("[AUTH] Failed to store OAuth state in cache");
      return crow::response(503, "Service temporarily unavailable");
    }

    std::string redirect_uri = config_->public_url + "/osu/auth/discord/callback";
    std::string url = fmt::format(
      "https://discord.com/api/oauth2/authorize?client_id={}&redirect_uri={}&response_type=code&scope=identify%20guilds&state={}",
      config_->discord_client_id, utils::url_encode(redirect_uri), state);

    crow::response res(302);
    res.set_header("Location", url);
    return res;
  });

  // Discord OAuth2: callback after Discord authorization
  CROW_ROUTE(app, "/osu/auth/discord/callback")
  ([this](const crow::request& req) {
    if (!config_) {
      return crow::response(500, "Not configured");
    }

    auto code_param = req.url_params.get("code");
    auto state_param = req.url_params.get("state");
    auto error_param = req.url_params.get("error");

    if (error_param) {
      spdlog::info("[AUTH] Discord OAuth2 denied: {}", error_param);
      crow::response res(302);
      res.set_header("Location", "/osu/settings?error=access_denied");
      return res;
    }

    if (!code_param || !state_param) {
      return crow::response(400, "Missing code or state parameter");
    }

    std::string code = code_param;
    std::string state = state_param;

    // Verify CSRF state
    auto& mc = cache::MemcachedCache::instance();
    auto state_check = mc.get("oauth_state:" + state);
    if (!state_check) {
      return crow::response(400, "Invalid or expired state");
    }
    mc.del("oauth_state:" + state);

    // Exchange code for token
    std::string redirect_uri = config_->public_url + "/osu/auth/discord/callback";
    auto token_response = cpr::Post(
      cpr::Url{"https://discord.com/api/v10/oauth2/token"},
      cpr::Header{{"Content-Type", "application/x-www-form-urlencoded"}},
      cpr::Timeout{10000},
      cpr::Payload{
        {"client_id", config_->discord_client_id},
        {"client_secret", config_->discord_client_secret},
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", redirect_uri}
      });

    if (token_response.status_code != 200) {
      spdlog::error("[AUTH] Discord token exchange failed: {}", token_response.status_code);
      return crow::response(500, "Failed to authenticate with Discord");
    }

    auto token_json = json::parse(token_response.text, nullptr, false);
    if (token_json.is_discarded() || !token_json.contains("access_token")) {
      spdlog::error("[AUTH] Invalid token response from Discord");
      return crow::response(500, "Invalid response from Discord");
    }

    std::string access_token = token_json["access_token"].get<std::string>();

    // Get user info
    auto user_response = cpr::Get(
      cpr::Url{"https://discord.com/api/v10/users/@me"},
      cpr::Header{{"Authorization", "Bearer " + access_token}},
      cpr::Timeout{10000});

    if (user_response.status_code != 200) {
      spdlog::error("[AUTH] Discord user info failed: {}", user_response.status_code);
      return crow::response(500, "Failed to get user info from Discord");
    }

    auto user_json = json::parse(user_response.text, nullptr, false);
    if (user_json.is_discarded() || !user_json.contains("id")) {
      spdlog::error("[AUTH] Invalid user response from Discord");
      return crow::response(500, "Invalid user response from Discord");
    }

    std::string discord_id = user_json["id"].get<std::string>();
    std::string username = user_json.contains("username") && user_json["username"].is_string()
                           ? user_json["username"].get<std::string>() : "";
    std::string global_name = user_json.contains("global_name") && user_json["global_name"].is_string()
                              ? user_json["global_name"].get<std::string>() : "";
    std::string avatar = user_json.contains("avatar") && user_json["avatar"].is_string()
                         ? user_json["avatar"].get<std::string>() : "";

    // Determine role: admin > member > denied
    bool is_admin = std::find(config_->admin_users.begin(), config_->admin_users.end(), discord_id)
                    != config_->admin_users.end();

    bool is_member = is_admin; // Admins are always considered members
    if (!is_member && !config_->guild_id.empty()) {
      // Check guild membership via Discord API
      auto guilds_response = cpr::Get(
        cpr::Url{"https://discord.com/api/v10/users/@me/guilds"},
        cpr::Header{{"Authorization", "Bearer " + access_token}},
        cpr::Timeout{10000});

      if (guilds_response.status_code == 200) {
        auto guilds_json = json::parse(guilds_response.text, nullptr, false);
        if (!guilds_json.is_discarded() && guilds_json.is_array()) {
          for (const auto& guild : guilds_json) {
            if (guild.contains("id") && guild["id"].is_string() &&
                guild["id"].get<std::string>() == config_->guild_id) {
              is_member = true;
              break;
            }
          }
        }
      } else {
        spdlog::error("[AUTH] Failed to fetch guilds for {} ({}): {}", username, discord_id, guilds_response.status_code);
        crow::response res(302);
        res.set_header("Location", "/osu/settings?error=guild_check_failed");
        return res;
      }
    }

    if (!is_member) {
      spdlog::info("[AUTH] Non-member login attempt: {} ({})", username, discord_id);
      crow::response res(302);
      res.set_header("Location", "/osu/settings?error=not_member");
      return res;
    }

    std::string role = is_admin ? "admin" : "member";

    // Create session
    auto session_token = generate_session_token();
    json session_data;
    session_data["discord_id"] = discord_id;
    session_data["username"] = global_name.empty() ? username : global_name;
    session_data["avatar"] = avatar;
    session_data["role"] = role;
    session_data["access_token"] = access_token;

    if (!mc.set("web_session:" + session_token, session_data.dump(), std::chrono::seconds(604800))) {
      spdlog::error("[AUTH] Failed to store session in cache");
      return crow::response(503, "Service temporarily unavailable");
    }

    spdlog::info("[AUTH] {} login: {} ({})", role, username, discord_id);

    crow::response res(302);
    res.set_header("Location", "/osu/settings");
    res.set_header("Set-Cookie",
      fmt::format("session={}; HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=604800", session_token));
    return res;
  });

  // Discord OAuth2: get current session user info
  CROW_ROUTE(app, "/osu/auth/discord/me")
  ([](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }

    json resp;
    resp["discord_id"] = session->discord_id;
    resp["username"] = session->username;
    resp["avatar"] = session->avatar;
    resp["role"] = session->role;
    // Super admin can revert template changes (don't expose the actual ID)
    resp["can_revert"] = is_super_admin(session->discord_id);

    crow::response res(200, resp.dump());
    res.set_header("Content-Type", "application/json");
    return res;
  });

  // Discord OAuth2: logout
  CROW_ROUTE(app, "/osu/auth/discord/logout").methods("POST"_method)
  ([](const crow::request& req) {
    auto cookie_header = req.get_header_value("Cookie");
    auto token = extract_cookie(cookie_header, "session");

    if (!token.empty()) {
      auto& mc = cache::MemcachedCache::instance();
      mc.del("web_session:" + token);
    }

    crow::response res(200);
    res.set_header("Set-Cookie", "session=; HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=0");
    return res;
  });

  // ============================================================================
  // osu! OAuth2 for account linking
  // ============================================================================

  // osu! OAuth2: redirect to osu! authorize page
  CROW_ROUTE(app, "/osu/auth/osu")
  ([this](const crow::request& req) {
    // Require Discord session first
    auto session = get_session(req);
    if (!session) {
      crow::response res(302);
      res.set_header("Location", "/osu/settings?error=login_required");
      return res;
    }

    if (!config_ || config_->osu_oauth_client_id.empty()) {
      return crow::response(500, "osu! OAuth2 not configured");
    }

    auto state = generate_session_token();
    auto& mc = cache::MemcachedCache::instance();

    // Store state with discord_id so we know who to link
    json state_data;
    state_data["discord_id"] = session->discord_id;
    if (!mc.set("osu_oauth_state:" + state, state_data.dump(), std::chrono::seconds(300))) {
      spdlog::error("[AUTH] Failed to store osu OAuth state in cache");
      return crow::response(503, "Service temporarily unavailable");
    }

    std::string redirect_uri = config_->public_url + "/osu/auth/osu/callback";
    std::string url = fmt::format(
      "https://osu.ppy.sh/oauth/authorize?client_id={}&redirect_uri={}&response_type=code&scope=identify&state={}",
      config_->osu_oauth_client_id, utils::url_encode(redirect_uri), state);

    crow::response res(302);
    res.set_header("Location", url);
    return res;
  });

  // osu! OAuth2: callback after osu! authorization
  CROW_ROUTE(app, "/osu/auth/osu/callback")
  ([this](const crow::request& req) {
    if (!config_) {
      return crow::response(500, "Not configured");
    }

    auto code_param = req.url_params.get("code");
    auto state_param = req.url_params.get("state");
    auto error_param = req.url_params.get("error");

    if (error_param) {
      spdlog::info("[AUTH] osu! OAuth2 denied: {}", error_param);
      crow::response res(302);
      res.set_header("Location", "/osu/settings?error=osu_access_denied");
      return res;
    }

    if (!code_param || !state_param) {
      return crow::response(400, "Missing code or state parameter");
    }

    std::string code = code_param;
    std::string state = state_param;

    // Verify CSRF state and get discord_id
    auto& mc = cache::MemcachedCache::instance();
    auto state_check = mc.get("osu_oauth_state:" + state);
    if (!state_check) {
      return crow::response(400, "Invalid or expired state");
    }
    mc.del("osu_oauth_state:" + state);

    auto state_json = json::parse(*state_check, nullptr, false);
    if (state_json.is_discarded() || !state_json.contains("discord_id")) {
      return crow::response(400, "Invalid state data");
    }
    std::string discord_id = state_json["discord_id"].get<std::string>();
    bool is_direct_link = state_json.contains("link_token");
    std::string link_token = state_json.value("link_token", "");

    // Exchange code for token
    std::string redirect_uri = config_->public_url + "/osu/auth/osu/callback";
    auto token_response = cpr::Post(
      cpr::Url{"https://osu.ppy.sh/oauth/token"},
      cpr::Header{{"Content-Type", "application/x-www-form-urlencoded"}},
      cpr::Timeout{10000},
      cpr::Payload{
        {"client_id", config_->osu_oauth_client_id},
        {"client_secret", config_->osu_oauth_client_secret},
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", redirect_uri}
      });

    if (token_response.status_code != 200) {
      spdlog::error("[AUTH] osu! token exchange failed: {} - {}", token_response.status_code, token_response.text);
      crow::response res(302);
      res.set_header("Location", "/osu/settings?error=osu_token_failed");
      return res;
    }

    auto token_json = json::parse(token_response.text, nullptr, false);
    if (token_json.is_discarded() || !token_json.contains("access_token")) {
      spdlog::error("[AUTH] Invalid token response from osu!");
      crow::response res(302);
      res.set_header("Location", "/osu/settings?error=osu_invalid_response");
      return res;
    }

    std::string access_token = token_json["access_token"].get<std::string>();

    // Get osu! user info
    auto user_response = cpr::Get(
      cpr::Url{"https://osu.ppy.sh/api/v2/me"},
      cpr::Header{{"Authorization", "Bearer " + access_token}},
      cpr::Timeout{10000});

    if (user_response.status_code != 200) {
      spdlog::error("[AUTH] osu! user info failed: {}", user_response.status_code);
      crow::response res(302);
      res.set_header("Location", "/osu/settings?error=osu_user_failed");
      return res;
    }

    auto osu_user_json = json::parse(user_response.text, nullptr, false);
    if (osu_user_json.is_discarded() || !osu_user_json.contains("id")) {
      spdlog::error("[AUTH] Invalid user response from osu!");
      crow::response res(302);
      res.set_header("Location", "/osu/settings?error=osu_invalid_user");
      return res;
    }

    int64_t osu_user_id = osu_user_json["id"].get<int64_t>();
    std::string osu_username = osu_user_json.value("username", "");

    // Save link to database and cache username (OAuth = true)
    try {
      auto& db = db::Database::instance();
      db.set_user_mapping(dpp::snowflake(std::stoull(discord_id)), osu_user_id, true);

      // Cache username for display (7 days)
      if (!osu_username.empty()) {
        mc.set("osu_username:" + std::to_string(osu_user_id), osu_username, std::chrono::seconds(604800));
      }

      // Delete link token if used
      if (!link_token.empty()) {
        mc.del("osu_link_token:" + link_token);
      }

      spdlog::info("[AUTH] Linked Discord {} to osu! {} ({})", discord_id, osu_user_id, osu_username);
    } catch (const std::exception& e) {
      spdlog::error("[AUTH] Failed to save osu! link: {}", e.what());
      crow::response res(302);
      res.set_header("Location", "/osu/settings?error=link_failed");
      return res;
    }

    // For direct links, show a simple success page
    if (is_direct_link) {
      std::string html = R"(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Account Linked - Patchouli</title>
<style>
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #09090b; color: #ececef; display: flex; align-items: center; justify-content: center; min-height: 100vh; margin: 0; }
.box { text-align: center; padding: 3rem; }
.icon { font-size: 4rem; margin-bottom: 1rem; }
h1 { font-size: 1.5rem; margin-bottom: 0.5rem; color: #4ade80; }
p { color: #a0a0ab; margin-bottom: 2rem; }
.user { color: #ff66aa; font-weight: 600; }
</style>
</head><body>
<div class="box">
<div class="icon">✓</div>
<h1>Account Linked!</h1>
<p>Your Discord is now linked to <span class="user">)" + utils::html_escape(osu_username) + R"(</span></p>
<p>You can close this page and return to Discord.</p>
</div>
</body></html>)";
      crow::response res(200, html);
      res.set_header("Content-Type", "text/html; charset=utf-8");
      return res;
    }

    crow::response res(302);
    res.set_header("Location", "/osu/settings?osu_linked=1");
    return res;
  });

  // GET /osu/api/me/osu - get current user's linked osu! account
  CROW_ROUTE(app, "/osu/api/me/osu")
  ([](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }

    try {
      auto& db = db::Database::instance();
      auto osu_id = db.get_osu_user_id(dpp::snowflake(std::stoull(session->discord_id)));

      crow::json::wvalue result;
      if (osu_id) {
        result["linked"] = true;
        result["osu_user_id"] = *osu_id;

        // Try to get cached username
        auto& mc = cache::MemcachedCache::instance();
        auto cached_name = mc.get("osu_username:" + std::to_string(*osu_id));
        if (cached_name) {
          result["osu_username"] = *cached_name;
        }
      } else {
        result["linked"] = false;
      }

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      spdlog::error("[API] me/osu error: {}", e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // DELETE /osu/api/me/osu - unlink osu! account
  CROW_ROUTE(app, "/osu/api/me/osu").methods("DELETE"_method)
  ([](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }

    try {
      auto& db = db::Database::instance();
      bool removed = db.remove_user_mapping(dpp::snowflake(std::stoull(session->discord_id)));

      crow::json::wvalue result;
      result["ok"] = removed;

      if (removed) {
        spdlog::info("[AUTH] Discord {} unlinked osu! account", session->discord_id);
      }

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      spdlog::error("[API] me/osu delete error: {}", e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // GET /osu/link/<token> - Direct osu! OAuth link (no Discord session required)
  CROW_ROUTE(app, "/osu/link/<string>")
  ([this](const std::string& token) {
    if (!config_ || config_->osu_oauth_client_id.empty()) {
      return crow::response(500, "osu! OAuth2 not configured");
    }

    // Verify token exists and get discord_id
    auto& mc = cache::MemcachedCache::instance();
    auto token_data = mc.get("osu_link_token:" + token);
    if (!token_data) {
      return crow::response(400, "Link expired or invalid. Please request a new link via /set command.");
    }

    auto token_json = json::parse(*token_data, nullptr, false);
    if (token_json.is_discarded() || !token_json.contains("discord_id")) {
      return crow::response(400, "Invalid token data");
    }

    // Generate OAuth state with token reference
    auto state = generate_session_token();

    json state_data;
    state_data["discord_id"] = token_json["discord_id"];
    state_data["link_token"] = token;  // Remember which token was used
    if (!mc.set("osu_oauth_state:" + state, state_data.dump(), std::chrono::seconds(300))) {
      spdlog::error("[AUTH] Failed to store osu OAuth state");
      return crow::response(503, "Service temporarily unavailable");
    }

    std::string redirect_uri = config_->public_url + "/osu/auth/osu/callback";
    std::string url = fmt::format(
      "https://osu.ppy.sh/oauth/authorize?client_id={}&redirect_uri={}&response_type=code&scope=identify&state={}",
      config_->osu_oauth_client_id, utils::url_encode(redirect_uri), state);

    crow::response res(302);
    res.set_header("Location", url);
    return res;
  });

  // GET /osu/settings - HTML page for settings (replaces /osu/presets)
  CROW_ROUTE(app, "/osu/settings")
  ([]() {
    std::vector<std::filesystem::path> paths = {
      "static/presets.html",
      "../static/presets.html",
      "/home/nisemonic/patchouli/bot/static/presets.html"
    };

    std::filesystem::path static_file;
    for (const auto& p : paths) {
      if (std::filesystem::exists(p)) {
        static_file = p;
        break;
      }
    }

    if (static_file.empty()) {
      return crow::response(404, "Page not found");
    }

    std::ifstream file(static_file);
    if (!file) {
      return crow::response(500, "Failed to read page");
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    crow::response res(200, buffer.str());
    res.set_header("Content-Type", "text/html; charset=utf-8");
    return res;
  });

  // Redirect old /osu/presets to /osu/settings
  CROW_ROUTE(app, "/osu/presets")
  ([]() {
    crow::response res(301);
    res.set_header("Location", "/osu/settings");
    return res;
  });

  // GET /osu/api/presets - list all user presets
  CROW_ROUTE(app, "/osu/api/presets")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }
    if (session->role != "admin") {
      return crow::response(403, "Admin access required");
    }

    if (!user_settings_service_) {
      return crow::response(503, "Service unavailable");
    }

    try {
      auto& db = db::Database::instance();
      auto presets = db.get_all_embed_presets();

      crow::json::wvalue result;
      crow::json::wvalue::list items;

      for (const auto& [discord_id, preset_str] : presets) {
        crow::json::wvalue item;
        item["discord_id"] = std::to_string(discord_id);
        item["preset"] = preset_str;

        // Try to get username from discord_users cache
        try {
          auto user = db.get_discord_user(dpp::snowflake(discord_id));
          if (user) {
            item["username"] = user->global_name.empty() ? user->username : user->global_name;
          } else {
            item["username"] = "";
          }
        } catch (...) {
          item["username"] = "";
        }

        items.push_back(std::move(item));
      }

      result["presets"] = std::move(items);
      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      return crow::response(500, "Internal server error");
    }
  });

  // POST /osu/api/presets - update user preset (admin only)
  CROW_ROUTE(app, "/osu/api/presets").methods("POST"_method)
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }
    if (session->role != "admin") {
      return crow::response(403, "Admin access required");
    }

    if (!user_settings_service_) {
      return crow::response(503, "Service unavailable");
    }

    try {
      auto body = crow::json::load(req.body);
      if (!body) {
        return crow::response(400, "Invalid JSON");
      }

      std::string discord_id_str = body["discord_id"].s();
      std::string preset_str = body["preset"].s();

      if (discord_id_str.empty() || preset_str.empty()) {
        return crow::response(400, "Missing discord_id or preset");
      }

      if (preset_str != "compact" && preset_str != "classic" && preset_str != "extended" && preset_str != "custom") {
        return crow::response(400, "Invalid preset. Must be: compact, classic, extended, custom");
      }

      uint64_t discord_id = std::stoull(discord_id_str);
      auto preset = services::embed_preset_from_string(preset_str);
      user_settings_service_->set_preset(dpp::snowflake(discord_id), preset);

      crow::json::wvalue result;
      result["ok"] = true;
      result["discord_id"] = discord_id_str;
      result["preset"] = preset_str;

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      return crow::response(500, "Internal server error");
    }
  });

  // DELETE /osu/api/presets - delete user preset (admin only)
  CROW_ROUTE(app, "/osu/api/presets").methods("DELETE"_method)
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }
    if (session->role != "admin") {
      return crow::response(403, "Admin access required");
    }

    if (!user_settings_service_) {
      return crow::response(503, "Service unavailable");
    }

    try {
      auto body = crow::json::load(req.body);
      if (!body) {
        return crow::response(400, "Invalid JSON");
      }

      std::string discord_id_str = body["discord_id"].s();
      if (discord_id_str.empty()) {
        return crow::response(400, "Missing discord_id");
      }

      uint64_t discord_id = std::stoull(discord_id_str);
      user_settings_service_->remove_preset(dpp::snowflake(discord_id));

      crow::json::wvalue result;
      result["ok"] = true;
      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      return crow::response(500, "Internal server error");
    }
  });

  // GET /osu/api/templates - get all command templates with configs (admin only)
  CROW_ROUTE(app, "/osu/api/templates")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }
    if (session->role != "admin") {
      return crow::response(403, "Admin access required");
    }

    if (!template_service_) {
      return crow::response(503, "Service unavailable");
    }

    try {
      auto commands = services::EmbedTemplateService::get_all_commands();

      crow::json::wvalue result;
      crow::json::wvalue::list commands_list;

      for (const auto& cmd : commands) {
        crow::json::wvalue cmd_obj;
        cmd_obj["id"] = cmd.command_id;
        cmd_obj["label"] = cmd.label;
        cmd_obj["has_presets"] = cmd.has_presets;

        // Field names
        crow::json::wvalue::list fields_list;
        for (const auto& f : cmd.field_names) {
          fields_list.push_back(f);
        }
        cmd_obj["fields"] = std::move(fields_list);

        // Placeholders
        crow::json::wvalue::list ph_list;
        for (const auto& ph : cmd.placeholders) {
          crow::json::wvalue ph_obj;
          ph_obj["name"] = ph.name;
          ph_obj["description"] = ph.description;
          ph_list.push_back(std::move(ph_obj));
        }
        cmd_obj["placeholders"] = std::move(ph_list);

        // Templates (current values from cache)
        crow::json::wvalue templates_obj;
        if (cmd.has_presets) {
          for (const auto& preset : {"compact", "classic", "extended"}) {
            std::string key = cmd.command_id + ":" + preset;
            auto fields = template_service_->get_fields(key);
            crow::json::wvalue t;
            for (const auto& [field_name, tmpl_text] : fields) {
              t[field_name] = tmpl_text;
            }
            templates_obj[preset] = std::move(t);
          }
        } else {
          auto fields = template_service_->get_fields(cmd.command_id);
          crow::json::wvalue t;
          for (const auto& [field_name, tmpl_text] : fields) {
            t[field_name] = tmpl_text;
          }
          templates_obj["default"] = std::move(t);
        }
        cmd_obj["templates"] = std::move(templates_obj);

        commands_list.push_back(std::move(cmd_obj));
      }

      result["commands"] = std::move(commands_list);

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      return crow::response(500, "Internal server error");
    }
  });

  // POST /osu/api/templates - update a command template (admin only)
  CROW_ROUTE(app, "/osu/api/templates").methods("POST"_method)
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }
    if (session->role != "admin") {
      return crow::response(403, "Admin access required");
    }

    if (!template_service_) {
      return crow::response(503, "Service unavailable");
    }

    try {
      auto body = crow::json::load(req.body);
      if (!body) {
        return crow::response(400, "Invalid JSON");
      }

      if (!body.has("command")) {
        return crow::response(400, "Missing required field: command");
      }
      std::string command = body["command"].s();

      // Validate command exists
      auto commands = services::EmbedTemplateService::get_all_commands();
      const services::CommandTemplateConfig* cmd_config = nullptr;
      for (const auto& cmd : commands) {
        if (cmd.command_id == command) {
          cmd_config = &cmd;
          break;
        }
      }
      if (!cmd_config) {
        return crow::response(400, "Unknown command: " + command);
      }

      // Get preset (required for preset commands, optional for single-template)
      std::string preset = "default";
      if (cmd_config->has_presets) {
        if (!body.has("preset")) {
          return crow::response(400, "Missing required field: preset");
        }
        preset = body["preset"].s();
        if (preset != "compact" && preset != "classic" && preset != "extended") {
          return crow::response(400, "Invalid preset name");
        }
      }

      if (!body.has("fields")) {
        return crow::response(400, "Missing required field: fields");
      }

      // Build fields map from request
      services::TemplateFields fields;
      auto fields_json = body["fields"];
      for (const auto& field_name : cmd_config->field_names) {
        if (fields_json.has(field_name)) {
          std::string val = fields_json[field_name].s();
          fields[field_name] = val;
        }
      }

      // Validate templates
      auto issues = services::EmbedTemplateService::validate_fields(command, preset, fields);

      std::vector<services::ValidationIssue> errors, warnings;
      for (const auto& issue : issues) {
        if (issue.level == services::ValidationIssue::Level::Error)
          errors.push_back(issue);
        else
          warnings.push_back(issue);
      }

      auto issues_to_json = [](const std::vector<services::ValidationIssue>& list) {
        crow::json::wvalue::list arr;
        for (const auto& item : list) {
          crow::json::wvalue obj;
          obj["field"] = item.field;
          obj["message"] = item.message;
          obj["position"] = static_cast<int64_t>(item.position);
          arr.push_back(std::move(obj));
        }
        return arr;
      };

      // Block save if there are errors
      if (!errors.empty()) {
        crow::json::wvalue result;
        result["ok"] = false;
        result["errors"] = issues_to_json(errors);
        result["warnings"] = issues_to_json(warnings);
        crow::response res(400, result.dump());
        res.set_header("Content-Type", "application/json");
        return res;
      }

      std::string key = cmd_config->has_presets ? (command + ":" + preset) : command;

      // Get old fields for audit log
      auto old_fields = template_service_->get_fields(key);

      // Update template
      template_service_->set_fields(key, fields);

      // Audit log
      try {
        auto& db = db::Database::instance();
        nlohmann::json old_json, new_json;
        for (const auto& [k, v] : old_fields) old_json[k] = v;
        for (const auto& [k, v] : fields) new_json[k] = v;
        db.log_template_change(
            dpp::snowflake(std::stoull(session->discord_id)),
            session->username,
            "update",
            command,
            cmd_config->has_presets ? preset : "",
            old_json.dump(),
            new_json.dump()
        );
      } catch (const std::exception& e) {
        spdlog::error("[API] Failed to log template change: {}", e.what());
      }

      crow::json::wvalue result;
      result["ok"] = true;
      result["command"] = command;
      result["preset"] = preset;
      if (!warnings.empty()) {
        result["warnings"] = issues_to_json(warnings);
      }
      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      return crow::response(500, "Internal server error");
    }
  });

  // POST /osu/api/templates/reset - reset a command template to defaults
  CROW_ROUTE(app, "/osu/api/templates/reset").methods("POST"_method)
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }
    if (session->role != "admin") {
      return crow::response(403, "Admin access required");
    }

    if (!template_service_) {
      return crow::response(503, "Service unavailable");
    }

    try {
      auto body = crow::json::load(req.body);
      if (!body) {
        return crow::response(400, "Invalid JSON");
      }

      if (!body.has("command")) {
        return crow::response(400, "Missing required field: command");
      }
      std::string command = body["command"].s();

      // Validate command
      auto commands = services::EmbedTemplateService::get_all_commands();
      const services::CommandTemplateConfig* cmd_config = nullptr;
      for (const auto& cmd : commands) {
        if (cmd.command_id == command) {
          cmd_config = &cmd;
          break;
        }
      }
      if (!cmd_config) {
        return crow::response(400, "Unknown command: " + command);
      }

      std::string preset = "default";
      if (cmd_config->has_presets) {
        if (!body.has("preset")) {
          return crow::response(400, "Missing required field: preset");
        }
        preset = body["preset"].s();
        if (preset != "compact" && preset != "classic" && preset != "extended") {
          return crow::response(400, "Invalid preset name");
        }
      }

      std::string key = cmd_config->has_presets ? (command + ":" + preset) : command;

      // Get old fields for audit log
      auto old_fields = template_service_->get_fields(key);

      // Reset template
      template_service_->reset_to_default(key);

      // Audit log
      try {
        auto& db = db::Database::instance();
        nlohmann::json old_json;
        for (const auto& [k, v] : old_fields) old_json[k] = v;
        db.log_template_change(
            dpp::snowflake(std::stoull(session->discord_id)),
            session->username,
            "reset",
            command,
            cmd_config->has_presets ? preset : "",
            old_json.dump(),
            ""  // new_fields empty for reset
        );
      } catch (const std::exception& e) {
        spdlog::error("[API] Failed to log template reset: {}", e.what());
      }

      crow::json::wvalue result;
      result["ok"] = true;
      result["command"] = command;
      result["preset"] = preset;
      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      return crow::response(500, "Internal server error");
    }
  });

  // GET /osu/api/templates/audit - get template audit log (admin only)
  CROW_ROUTE(app, "/osu/api/templates/audit")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }
    if (session->role != "admin") {
      return crow::response(403, "Admin access required");
    }

    try {
      auto& db = db::Database::instance();

      // Parse query params
      size_t limit = 50;
      size_t offset = 0;
      std::string command_filter;

      auto limit_param = req.url_params.get("limit");
      if (limit_param) {
        auto val = static_cast<size_t>(std::stoull(limit_param));
        limit = std::min(size_t(100), std::max(size_t(1), val));
      }
      auto offset_param = req.url_params.get("offset");
      if (offset_param) {
        offset = static_cast<size_t>(std::stoull(offset_param));
      }
      auto command_param = req.url_params.get("command");
      if (command_param) {
        command_filter = command_param;
      }

      std::vector<db::TemplateAuditEntry> entries;
      if (!command_filter.empty()) {
        entries = db.get_template_audit_log_by_command(command_filter, limit);
      } else {
        entries = db.get_template_audit_log(limit, offset);
      }

      crow::json::wvalue::list entries_json;
      for (const auto& entry : entries) {
        crow::json::wvalue e;
        e["id"] = entry.id;
        e["discord_id"] = std::to_string(static_cast<uint64_t>(entry.discord_id));
        e["discord_username"] = entry.discord_username;
        e["action"] = entry.action;
        e["command_id"] = entry.command_id;
        e["preset"] = entry.preset;
        e["old_fields"] = entry.old_fields_json;
        e["new_fields"] = entry.new_fields_json;

        auto time_t_val = std::chrono::system_clock::to_time_t(entry.created_at);
        std::tm tm = *std::gmtime(&time_t_val);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        e["created_at"] = std::string(buf);

        entries_json.push_back(std::move(e));
      }

      crow::json::wvalue result;
      result["entries"] = std::move(entries_json);
      result["limit"] = static_cast<int64_t>(limit);
      result["offset"] = static_cast<int64_t>(offset);

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      spdlog::error("[API] templates/audit error: {}", e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // POST /osu/api/templates/audit/:id/revert - revert to a specific audit entry (super admin only)
  CROW_ROUTE(app, "/osu/api/templates/audit/<int>/revert").methods("POST"_method)
  ([this](const crow::request& req, int64_t audit_id) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }

    // Super admin check - must be admin AND super admin
    if (session->role != "admin") {
      return crow::response(403, "Admin access required");
    }
    if (!is_super_admin(session->discord_id)) {
      spdlog::warn("[API] Non-super-admin {} attempted revert", session->discord_id);
      return crow::response(403, "Insufficient permissions");
    }

    if (!template_service_) {
      return crow::response(503, "Service unavailable");
    }

    try {
      auto& db = db::Database::instance();

      // Get the audit entry
      auto entry = db.get_template_audit_entry(audit_id);
      if (!entry) {
        return crow::response(404, "Audit entry not found");
      }

      // Parse the old_fields to revert to
      if (entry->old_fields_json.empty()) {
        return crow::response(400, "Cannot revert: no previous state available");
      }

      nlohmann::json old_fields_obj;
      try {
        old_fields_obj = nlohmann::json::parse(entry->old_fields_json);
      } catch (const std::exception& e) {
        spdlog::error("[API] Failed to parse old_fields_json: {}", e.what());
        return crow::response(400, "Cannot revert: invalid old fields data");
      }

      // Convert JSON to TemplateFields
      services::TemplateFields revert_fields;
      for (auto& [key, value] : old_fields_obj.items()) {
        if (value.is_string()) {
          revert_fields[key] = value.get<std::string>();
        }
      }

      // Build the key
      std::string key = entry->command_id;
      if (!entry->preset.empty()) {
        key += ":" + entry->preset;
      }

      // Get current fields for audit log
      auto current_fields = template_service_->get_fields(key);

      // Apply the revert
      template_service_->set_fields(key, revert_fields);

      // Log the revert in audit
      nlohmann::json current_json, revert_json;
      for (const auto& [k, v] : current_fields) current_json[k] = v;
      for (const auto& [k, v] : revert_fields) revert_json[k] = v;

      db.log_template_change(
          dpp::snowflake(std::stoull(session->discord_id)),
          session->username,
          "revert",
          entry->command_id,
          entry->preset,
          current_json.dump(),
          revert_json.dump()
      );

      spdlog::info("[API] Super admin {} reverted template {}:{} to audit entry {}",
          session->username, entry->command_id, entry->preset, audit_id);

      crow::json::wvalue result;
      result["ok"] = true;
      result["reverted_to"] = audit_id;
      result["command"] = entry->command_id;
      result["preset"] = entry->preset;

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      spdlog::error("[API] templates/audit/{}/revert error: {}", audit_id, e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // GET /osu/api/my-settings - get current user's personal settings
  CROW_ROUTE(app, "/osu/api/my-settings")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }

    if (!user_settings_service_) {
      return crow::response(503, "Service unavailable");
    }

    try {
      auto preset = user_settings_service_->get_preset(dpp::snowflake(std::stoull(session->discord_id)));
      std::string preset_str = services::embed_preset_to_string(preset);

      crow::json::wvalue result;
      result["embed_preset"] = preset_str;

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      return crow::response(500, "Internal server error");
    }
  });

  // POST /osu/api/my-settings - update current user's personal settings
  CROW_ROUTE(app, "/osu/api/my-settings").methods("POST"_method)
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      return crow::response(401, "Unauthorized");
    }

    if (!user_settings_service_) {
      return crow::response(503, "Service unavailable");
    }

    try {
      auto body = crow::json::load(req.body);
      if (!body) {
        return crow::response(400, "Invalid JSON");
      }

      if (body.has("embed_preset")) {
        std::string preset_str = body["embed_preset"].s();
        if (preset_str != "compact" && preset_str != "classic" && preset_str != "extended" && preset_str != "custom") {
          return crow::response(400, "Invalid preset. Must be: compact, classic, extended, custom");
        }

        auto preset = services::embed_preset_from_string(preset_str);
        user_settings_service_->set_preset(dpp::snowflake(std::stoull(session->discord_id)), preset);
      }

      // Return current settings after update
      auto preset = user_settings_service_->get_preset(dpp::snowflake(std::stoull(session->discord_id)));
      std::string preset_str = services::embed_preset_to_string(preset);

      crow::json::wvalue result;
      result["ok"] = true;
      result["embed_preset"] = preset_str;

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      return crow::response(500, "Internal server error");
    }
  });

  // ============================================================================
  // User Custom Template Endpoints
  // ============================================================================

  // GET /osu/api/my-templates - get all custom templates for current user
  CROW_ROUTE(app, "/osu/api/my-templates")
  ([this](const crow::request& req) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }

    try {
      auto& db = db::Database::instance();
      auto templates = db.get_user_custom_templates(dpp::snowflake(std::stoull(session->discord_id)));

      // Check if user can edit custom templates
      bool can_edit = session->role == "admin" ||
        (config_ && can_edit_custom_templates(session->discord_id, config_->bot_token));

      crow::json::wvalue result;
      result["can_edit"] = can_edit;
      crow::json::wvalue::object templates_obj;
      for (const auto& [cmd_id, json_config] : templates) {
        templates_obj[cmd_id] = crow::json::load(json_config);
      }
      result["templates"] = std::move(templates_obj);

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      spdlog::error("[API] my-templates error: {}", e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // GET /osu/api/my-templates/<command> - get custom template for a specific command
  CROW_ROUTE(app, "/osu/api/my-templates/<string>")
  ([this](const crow::request& req, const std::string& command_id) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }

    // Validate command_id
    static const std::set<std::string> VALID_COMMANDS = {
      "rs", "compare", "map", "lb", "sim", "top", "profile", "osc"
    };
    if (VALID_COMMANDS.find(command_id) == VALID_COMMANDS.end()) {
      crow::json::wvalue error;
      error["error"] = "Invalid command ID";
      return crow::response(400, error);
    }

    try {
      auto& db = db::Database::instance();
      auto tmpl = db.get_user_custom_template(dpp::snowflake(std::stoull(session->discord_id)), command_id);

      crow::json::wvalue result;
      result["exists"] = tmpl.has_value();

      if (tmpl) {
        result["template"] = crow::json::load(*tmpl);
      }

      // Include allowed placeholders
      auto placeholders = services::EmbedTemplateService::get_allowed_placeholders(command_id);
      crow::json::wvalue::list ph_list;
      for (const auto& ph : placeholders) {
        ph_list.push_back(ph);
      }
      result["placeholders"] = std::move(ph_list);

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      spdlog::error("[API] my-templates/{} error: {}", command_id, e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // POST /osu/api/my-templates/<command> - save custom template for a command
  CROW_ROUTE(app, "/osu/api/my-templates/<string>").methods("POST"_method)
  ([this](const crow::request& req, const std::string& command_id) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }

    // Check if user has permission to edit custom templates
    if (session->role != "admin" && config_ && !can_edit_custom_templates(session->discord_id, config_->bot_token)) {
      crow::json::wvalue error;
      error["error"] = "Permission denied. You need a specific role to edit custom templates.";
      return crow::response(403, error);
    }

    // Rate limit: 10 saves per minute per user
    if (!template_save_limiter_->allow(session->discord_id)) {
      crow::json::wvalue error;
      error["error"] = "Rate limit exceeded. Maximum 10 saves per minute.";
      crow::response res(429, error);
      res.set_header("Retry-After", "60");
      return res;
    }

    // Validate command_id
    static const std::set<std::string> VALID_COMMANDS = {
      "rs", "compare", "map", "lb", "sim", "top", "profile", "osc"
    };
    if (VALID_COMMANDS.find(command_id) == VALID_COMMANDS.end()) {
      crow::json::wvalue error;
      error["error"] = "Invalid command ID";
      return crow::response(400, error);
    }

    try {
      auto body = nlohmann::json::parse(req.body);
      if (!body.contains("template")) {
        crow::json::wvalue error;
        error["error"] = "Missing 'template' field";
        return crow::response(400, error);
      }

      std::string json_config = body["template"].dump();

      // Validate template
      auto validation = services::EmbedTemplateService::validate_user_template(command_id, json_config);

      crow::json::wvalue result;
      result["ok"] = validation.valid;

      if (!validation.errors.empty()) {
        crow::json::wvalue::list errors_list;
        for (const auto& err : validation.errors) {
          errors_list.push_back(err);
        }
        result["errors"] = std::move(errors_list);
      }

      if (!validation.warnings.empty()) {
        crow::json::wvalue::list warnings_list;
        for (const auto& warn : validation.warnings) {
          warnings_list.push_back(warn);
        }
        result["warnings"] = std::move(warnings_list);
      }

      // Only save if valid
      if (validation.valid) {
        auto& db = db::Database::instance();
        db.set_user_custom_template(
          dpp::snowflake(std::stoull(session->discord_id)),
          command_id,
          json_config
        );
        spdlog::info("[API] User {} saved custom template for {}", session->discord_id, command_id);
      }

      crow::response res(validation.valid ? 200 : 400, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const nlohmann::json::exception& e) {
      crow::json::wvalue error;
      error["error"] = "Invalid JSON";
      error["details"] = e.what();
      return crow::response(400, error);
    } catch (const std::exception& e) {
      spdlog::error("[API] my-templates/{} POST error: {}", command_id, e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // DELETE /osu/api/my-templates/<command> - delete custom template for a command
  CROW_ROUTE(app, "/osu/api/my-templates/<string>").methods("DELETE"_method)
  ([this](const crow::request& req, const std::string& command_id) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }

    // Check if user has permission to edit custom templates
    if (session->role != "admin" && config_ && !can_edit_custom_templates(session->discord_id, config_->bot_token)) {
      crow::json::wvalue error;
      error["error"] = "Permission denied. You need a specific role to edit custom templates.";
      return crow::response(403, error);
    }

    // Validate command_id
    static const std::set<std::string> VALID_COMMANDS = {
      "rs", "compare", "map", "lb", "sim", "top", "profile", "osc"
    };
    if (VALID_COMMANDS.find(command_id) == VALID_COMMANDS.end()) {
      crow::json::wvalue error;
      error["error"] = "Invalid command ID";
      return crow::response(400, error);
    }

    try {
      auto& db = db::Database::instance();
      db.delete_user_custom_template(dpp::snowflake(std::stoull(session->discord_id)), command_id);

      crow::json::wvalue result;
      result["ok"] = true;
      result["message"] = "Template deleted";

      spdlog::info("[API] User {} deleted custom template for {}", session->discord_id, command_id);

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const std::exception& e) {
      spdlog::error("[API] my-templates/{} DELETE error: {}", command_id, e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // POST /osu/api/my-templates/<command>/init - initialize custom template from a preset
  CROW_ROUTE(app, "/osu/api/my-templates/<string>/init").methods("POST"_method)
  ([this](const crow::request& req, const std::string& command_id) {
    auto session = get_session(req);
    if (!session) {
      crow::json::wvalue error;
      error["error"] = "Authentication required";
      return crow::response(401, error);
    }

    // Check if user has permission to edit custom templates
    if (session->role != "admin" && config_ && !can_edit_custom_templates(session->discord_id, config_->bot_token)) {
      crow::json::wvalue error;
      error["error"] = "Permission denied. You need a specific role to edit custom templates.";
      return crow::response(403, error);
    }

    // Validate command_id
    static const std::set<std::string> VALID_COMMANDS = {
      "rs", "compare", "map", "lb", "sim", "top", "profile", "osc"
    };
    if (VALID_COMMANDS.find(command_id) == VALID_COMMANDS.end()) {
      crow::json::wvalue error;
      error["error"] = "Invalid command ID";
      return crow::response(400, error);
    }

    try {
      std::string from_preset = "classic";  // Default

      if (!req.body.empty()) {
        auto body = nlohmann::json::parse(req.body);
        if (body.contains("from_preset") && body["from_preset"].is_string()) {
          from_preset = body["from_preset"].get<std::string>();
        }
      }

      // Validate preset
      static const std::set<std::string> VALID_PRESETS = {"compact", "classic", "extended"};
      if (VALID_PRESETS.find(from_preset) == VALID_PRESETS.end()) {
        from_preset = "classic";
      }

      // Get template from the service
      if (!template_service_) {
        crow::json::wvalue error;
        error["error"] = "Template service not available";
        return crow::response(503, error);
      }

      // Build template key
      std::string tmpl_key = command_id;

      // Check if command has presets
      auto commands = services::EmbedTemplateService::get_all_commands();
      bool has_presets = false;
      for (const auto& cmd : commands) {
        if (cmd.command_id == command_id) {
          has_presets = cmd.has_presets;
          break;
        }
      }

      if (has_presets) {
        tmpl_key = command_id + ":" + from_preset;
      }

      // Get the JSON template or convert from legacy
      std::optional<std::string> json_tmpl = template_service_->get_json_template(tmpl_key);

      if (!json_tmpl) {
        // Fallback: get legacy fields and convert to simple flat JSON
        auto fields = template_service_->get_fields(tmpl_key);
        if (fields.empty()) {
          fields = services::EmbedTemplateService::get_default_fields(tmpl_key);
        }

        // Create a flat JSON structure from fields (matching frontend format)
        nlohmann::json j;
        for (const auto& [key, value] : fields) {
          j[key] = value;
        }
        json_tmpl = j.dump();
      }

      // Save as user's custom template
      auto& db = db::Database::instance();
      db.set_user_custom_template(
        dpp::snowflake(std::stoull(session->discord_id)),
        command_id,
        *json_tmpl
      );

      spdlog::info("[API] User {} initialized custom template for {} from {}",
        session->discord_id, command_id, from_preset);

      crow::json::wvalue result;
      result["ok"] = true;
      result["message"] = "Template initialized from " + from_preset;
      result["template"] = crow::json::load(*json_tmpl);

      crow::response res(200, result.dump());
      res.set_header("Content-Type", "application/json");
      return res;
    } catch (const nlohmann::json::exception& e) {
      crow::json::wvalue error;
      error["error"] = "Invalid JSON";
      return crow::response(400, error);
    } catch (const std::exception& e) {
      spdlog::error("[API] my-templates/{}/init error: {}", command_id, e.what());
      return crow::response(500, "Internal server error");
    }
  });

  // ==========================================================================
  // Music Player Endpoints (must be before catch-all /osu/<string>/<path>)
  // ==========================================================================

  // Helper: check if user is in music whitelist (captured by value in routes)
  auto is_music_allowed = [this](const std::string& discord_id) -> bool {
    if (!config_) return false;
    // Admins always allowed
    for (const auto& admin : config_->admin_users) {
      if (admin == discord_id) return true;
    }
    // Anyone with the required role (or higher) on the guild
    return can_edit_custom_templates(discord_id, config_->bot_token);
  };

  // Helper: check user is in the same voice channel as the bot.
  // Returns empty string if OK, or error message if not.
  auto check_voice_channel = [this](const std::string& discord_id, dpp::snowflake guild_id) -> std::string {
    if (!music_service_) return "";
    auto* guild = dpp::find_guild(guild_id);
    if (!guild) return "";  // can't verify, allow

    auto bot_id = music_service_->get_bot_id();
    auto bot_it = guild->voice_members.find(bot_id);
    if (bot_it == guild->voice_members.end() || bot_it->second.channel_id == 0) {
      return "";  // bot not in voice, no restriction
    }

    auto user_id = dpp::snowflake(discord_id);
    auto user_it = guild->voice_members.find(user_id);
    if (user_it == guild->voice_members.end() || user_it->second.channel_id != bot_it->second.channel_id) {
      return "You must be in the same voice channel as the bot";
    }
    return "";
  };

  // Serve music.html - server-rendered
  CROW_ROUTE(app, "/osu/music")
  ([serve_page](const crow::request& req) {
    return serve_page(req, {
      "static/music.html", "../static/music.html",
      "/home/nisemonic/patchouli/bot/static/music.html"
    }, "Music — Patchouli");
  });

  static const std::string MUSIC_ADMIN_ID = "249958340690575360";

  // GET /osu/api/music/guilds — list guilds the user AND bot share
  CROW_ROUTE(app, "/osu/api/music/guilds")
  ([this, is_music_allowed](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }

    // Collect bot's guild IDs
    std::unordered_set<std::string> bot_guild_ids;
    auto* guild_cache = dpp::get_guild_cache();
    if (guild_cache) {
      auto& container = guild_cache->get_container();
      std::shared_lock lock(guild_cache->get_mutex());
      for (auto& [id, guild_ptr] : container) {
        if (guild_ptr) bot_guild_ids.insert(std::to_string(id));
      }
    }

    // Fetch user's guilds via Discord API
    std::vector<crow::json::wvalue> guilds;
    if (!session->access_token.empty()) {
      auto resp = cpr::Get(
        cpr::Url{"https://discord.com/api/v10/users/@me/guilds"},
        cpr::Header{{"Authorization", "Bearer " + session->access_token}},
        cpr::Timeout{10000});

      if (resp.status_code == 200) {
        auto user_guilds = json::parse(resp.text, nullptr, false);
        if (user_guilds.is_array()) {
          for (auto& ug : user_guilds) {
            std::string gid = ug.value("id", "");
            if (bot_guild_ids.count(gid)) {
              // Look up full guild info from bot's cache
              auto* guild_ptr = dpp::find_guild(dpp::snowflake(gid));
              if (!guild_ptr) continue;
              crow::json::wvalue g;
              g["id"] = gid;
              g["name"] = guild_ptr->name;
              g["icon"] = guild_ptr->get_icon_url(256);
              guilds.push_back(std::move(g));
            }
          }
        }
      } else {
        spdlog::warn("[MUSIC] Discord API /users/@me/guilds failed: {} {}", resp.status_code, resp.text);
      }
    }

    // Fallback: if no guilds resolved (token missing, expired, or API error)
    if (guilds.empty()) {
      auto* guild_ptr = dpp::find_guild(1030424871173361704ULL);
      if (guild_ptr) {
        crow::json::wvalue g;
        g["id"] = "1030424871173361704";
        g["name"] = guild_ptr->name;
        g["icon"] = guild_ptr->get_icon_url(256);
        guilds.push_back(std::move(g));
      }
    }

    crow::json::wvalue res;
    res["guilds"] = std::move(guilds);
    res["is_admin"] = (session->discord_id == MUSIC_ADMIN_ID);
    return crow::response(200, res);
  });

  // GET /osu/api/music/state — current playback state
  CROW_ROUTE(app, "/osu/api/music/state")
  ([this, is_music_allowed](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }

    if (!music_service_) {
      return crow::response(503, R"({"error":"Music service unavailable"})");
    }

    auto* gid_param = req.url_params.get("guild_id");
    if (!gid_param) return crow::response(400, R"({"error":"Missing guild_id"})");
    auto guild_id = dpp::snowflake(std::string(gid_param));

    auto payload = build_state_json(static_cast<uint64_t>(guild_id));
    crow::response resp(200);
    resp.set_header("Content-Type", "application/json");
    resp.body = std::move(payload);
    return resp;
  });

  // WebSocket /osu/api/music/ws — real-time state push
  CROW_WEBSOCKET_ROUTE(app, "/osu/music-ws")
  .onaccept([this, is_music_allowed](const crow::request& req, void** userdata) -> bool {
    auto session = get_session(req);
    if (!session) return false;
    if (!is_music_allowed(session->discord_id)) return false;

    auto* gid_param = req.url_params.get("guild_id");
    if (!gid_param) return false;

    auto* data = new WsConnectionData();
    data->guild_id = std::stoull(std::string(gid_param));
    data->discord_id = session->discord_id;
    *userdata = data;
    return true;
  })
  .onopen([this](crow::websocket::connection& conn) {
    auto* data = static_cast<WsConnectionData*>(conn.userdata());
    if (!data) return;
    {
      std::lock_guard lock(ws_mutex_);
      ws_guild_connections_[data->guild_id].insert(&conn);
    }
    spdlog::info("[ws] Music WS opened for guild {} (user {})", data->guild_id, data->discord_id);
    // Send initial state
    try {
      conn.send_text(build_state_json(data->guild_id));
    } catch (const std::exception& e) {
      spdlog::warn("[ws] Failed to send initial state: {}", e.what());
    }
  })
  .onclose([this](crow::websocket::connection& conn, const std::string& reason) {
    auto* data = static_cast<WsConnectionData*>(conn.userdata());
    if (!data) return;
    {
      std::lock_guard lock(ws_mutex_);
      auto it = ws_guild_connections_.find(data->guild_id);
      if (it != ws_guild_connections_.end()) {
        it->second.erase(&conn);
        if (it->second.empty()) ws_guild_connections_.erase(it);
      }
    }
    spdlog::info("[ws] Music WS closed for guild {} (reason: {})", data->guild_id, reason);
    delete data;
  })
  .onmessage([](crow::websocket::connection&, const std::string&, bool) {
    // Server-push only; ignore client messages
  })
  .onerror([](crow::websocket::connection& conn, const std::string& error) {
    spdlog::warn("[ws] Music WS error: {}", error);
  });

  // POST /osu/api/music/play — add track to queue
  CROW_ROUTE(app, "/osu/api/music/play").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }

    if (!music_play_limiter_->allow(session->discord_id)) {
      return crow::response(429, R"({"error":"Too many requests, slow down"})");
    }

    if (!music_service_) {
      return crow::response(503, R"({"error":"Music service unavailable"})");
    }

    auto body = crow::json::load(req.body);
    if (!body || !body.has("url") || !body.has("guild_id")) {
      return crow::response(400, R"({"error":"Missing required fields"})");
    }

    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    if (guild_id == 0) return crow::response(400, R"({"error":"Invalid guild_id"})");

    // If bot is already in voice, user must be in the same channel
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    std::string url = body["url"].s();
    if (url.empty()) {
      return crow::response(400, R"({"error":"Empty URL"})");
    }

    auto user_id = dpp::snowflake(session->discord_id);

    // Use explicit channel_id if provided, otherwise detect from user's voice state
    dpp::snowflake voice_channel_id{0};
    if (body.has("channel_id")) {
      std::string ch_str = body["channel_id"].s();
      if (!ch_str.empty()) {
        voice_channel_id = dpp::snowflake(ch_str);
      }
    }

    if (voice_channel_id == 0) {
      auto* guild = dpp::find_guild(guild_id);
      if (guild) {
        auto it = guild->voice_members.find(user_id);
        if (it != guild->voice_members.end()) {
          voice_channel_id = it->second.channel_id;
        }
      }
    }

    if (voice_channel_id == 0) {
      return crow::response(400, R"({"error":"Select a voice channel"})");
    }

    // Verify user is actually in the target voice channel
    {
      auto* guild = dpp::find_guild(guild_id);
      if (guild) {
        auto it = guild->voice_members.find(user_id);
        if (it == guild->voice_members.end() || it->second.channel_id != voice_channel_id) {
          return crow::response(403, R"({"error":"You must be in the voice channel to start playback"})");
        }
      }
    }

    std::string requester = session->username;

    // Playlist support — detect and add all tracks
    if (services::MusicPlayerService::is_playlist_url(url)) {
      spdlog::info("[music-audit] {} ({}) play playlist url={} guild={}",
                   session->username, session->discord_id, url, std::string(body["guild_id"].s()));

      auto tracks = music_service_->fetch_playlist(url);
      if (tracks.empty()) {
        return crow::response(400, R"({"error":"Failed to load playlist or playlist is empty"})");
      }

      int added = 0;
      std::string first_title;
      for (auto& track : tracks) {
        track.requester = requester;
        auto r = music_service_->play(guild_id, voice_channel_id, track);
        if (r.success) {
          if (added == 0) first_title = r.title;
          added++;
        }
      }

      crow::json::wvalue res;
      res["success"] = added > 0;
      if (added > 0) {
        res["title"] = first_title;
        res["queue_position"] = added > 1 ? 1 : 0;
        res["tracks_added"] = added;
      } else {
        res["error"] = "Failed to add any tracks from playlist";
      }
      return crow::response(200, res);
    }

    // Check for osu! beatmap URL — extract audio from .osz instead of yt-dlp
    static const std::regex osu_set_regex(R"(https?://osu\.ppy\.sh/beatmapsets/(\d+))");
    static const std::regex osu_bm_regex(R"(https?://osu\.ppy\.sh/(?:beatmaps|b)/(\d+))");
    std::smatch osu_match;
    services::MusicPlayerService::PlayResult result;
    uint32_t beatmapset_id = 0;

    if (std::regex_search(url, osu_match, osu_set_regex)) {
      try { beatmapset_id = std::stoul(osu_match[1].str()); } catch (...) {}
    } else if (std::regex_search(url, osu_match, osu_bm_regex)) {
      uint32_t beatmap_id = 0;
      try { beatmap_id = std::stoul(osu_match[1].str()); } catch (...) {}

      if (beatmap_id > 0) {
        try {
          auto& db = db::Database::instance();
          auto set_opt = db.get_beatmapset_id(beatmap_id);
          if (set_opt) {
            beatmapset_id = static_cast<uint32_t>(*set_opt);
            spdlog::info("[music] Resolved beatmap {} -> beatmapset {} from DB", beatmap_id, beatmapset_id);
          }
        } catch (const std::exception& e) {
          spdlog::warn("[music] DB lookup failed for beatmap {}: {}", beatmap_id, e.what());
        }

        if (beatmapset_id == 0) {
          spdlog::info("[music] Resolving beatmap {} via HTTP redirect", beatmap_id);
          auto r = cpr::Head(cpr::Url{fmt::format("https://osu.ppy.sh/beatmaps/{}", beatmap_id)},
                             cpr::Timeout{10000});
          std::smatch redirect_match;
          std::string final_url = r.url.str();
          if (std::regex_search(final_url, redirect_match, osu_set_regex)) {
            try { beatmapset_id = std::stoul(redirect_match[1].str()); } catch (...) {}
            spdlog::info("[music] Resolved beatmap {} -> beatmapset {} via redirect", beatmap_id, beatmapset_id);
          }
        }
      }
    }

    if (beatmapset_id > 0) {
      spdlog::info("[music-audit] {} ({}) play osu beatmapset={} guild={}",
                   session->username, session->discord_id, beatmapset_id, std::string(body["guild_id"].s()));

      auto audio = downloader_->get_or_extract_audio(beatmapset_id);
      if (!audio) {
        return crow::response(500, R"({"error":"Failed to extract audio from beatmap"})");
      }

      services::TrackInfo track;
      track.url = url;
      track.audio_path = audio->audio_path;
      track.title = audio->title;
      track.duration_seconds = audio->duration_seconds;
      track.thumbnail = audio->thumbnail;
      track.osu_beatmapset_id = audio->beatmapset_id;
      track.requester = requester;

      result = music_service_->play(guild_id, voice_channel_id, track);
    } else {
      result = music_service_->play(guild_id, voice_channel_id, url, requester);
    }

    spdlog::info("[music-audit] {} ({}) play url={} guild={} success={} title={}",
                 session->username, session->discord_id, url, std::string(body["guild_id"].s()),
                 result.success, result.success ? result.title : result.error);

    crow::json::wvalue res;
    res["success"] = result.success;
    if (result.success) {
      res["title"] = result.title;
      res["queue_position"] = result.queue_position;
    } else {
      res["error"] = result.error;
    }
    return crow::response(200, res);
  });

  // POST /osu/api/music/skip
  CROW_ROUTE(app, "/osu/api/music/skip").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");
    auto body = crow::json::load(req.body);
    if (!body || !body.has("guild_id")) return crow::response(400, R"({"error":"Missing guild_id"})");
    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    spdlog::info("[music-audit] {} ({}) skip guild={}", session->username, session->discord_id, std::string(body["guild_id"].s()));
    bool ok = music_service_->skip(guild_id);
    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  // POST /osu/api/music/stop
  CROW_ROUTE(app, "/osu/api/music/stop").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");
    auto body = crow::json::load(req.body);
    if (!body || !body.has("guild_id")) return crow::response(400, R"({"error":"Missing guild_id"})");
    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    spdlog::info("[music-audit] {} ({}) stop guild={}", session->username, session->discord_id, std::string(body["guild_id"].s()));
    bool ok = music_service_->stop(guild_id);
    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  // POST /osu/api/music/pause
  CROW_ROUTE(app, "/osu/api/music/pause").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");
    auto body = crow::json::load(req.body);
    if (!body || !body.has("guild_id")) return crow::response(400, R"({"error":"Missing guild_id"})");
    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    spdlog::info("[music-audit] {} ({}) pause/resume guild={}", session->username, session->discord_id, std::string(body["guild_id"].s()));
    bool ok = music_service_->pause(guild_id);
    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  // POST /osu/api/music/volume
  CROW_ROUTE(app, "/osu/api/music/volume").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto body = crow::json::load(req.body);
    if (!body || !body.has("volume") || !body.has("guild_id")) {
      return crow::response(400, R"({"error":"Missing required fields"})");
    }

    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    int volume = static_cast<int>(body["volume"].i());
    spdlog::info("[music-audit] {} ({}) volume={} guild={}", session->username, session->discord_id, volume, std::string(body["guild_id"].s()));
    bool ok = music_service_->set_volume(guild_id, volume);

    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  // POST /osu/api/music/speed
  CROW_ROUTE(app, "/osu/api/music/speed").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto body = crow::json::load(req.body);
    if (!body || !body.has("speed") || !body.has("guild_id")) {
      return crow::response(400, R"({"error":"Missing required fields"})");
    }

    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    float speed = static_cast<float>(body["speed"].d());
    spdlog::info("[music-audit] {} ({}) speed={}x guild={}", session->username, session->discord_id, speed, std::string(body["guild_id"].s()));
    bool ok = music_service_->set_speed(guild_id, speed);

    crow::json::wvalue res;
    res["success"] = ok;
    if (!ok) res["error"] = "Speed must be between 0.5 and 2.0";
    return crow::response(200, res);
  });

  // POST /osu/api/music/nightcore
  CROW_ROUTE(app, "/osu/api/music/nightcore").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto body = crow::json::load(req.body);
    if (!body || !body.has("enabled") || !body.has("guild_id")) {
      return crow::response(400, R"({"error":"Missing required fields"})");
    }

    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    bool enabled = body["enabled"].b();
    spdlog::info("[music-audit] {} ({}) nightcore={} guild={}", session->username, session->discord_id, enabled, std::string(body["guild_id"].s()));
    bool ok = music_service_->set_nightcore(guild_id, enabled);

    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  // POST /osu/api/music/reverb
  CROW_ROUTE(app, "/osu/api/music/reverb").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto body = crow::json::load(req.body);
    if (!body || !body.has("enabled") || !body.has("guild_id")) {
      return crow::response(400, R"({"error":"Missing required fields"})");
    }

    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    bool enabled = body["enabled"].b();
    spdlog::info("[music-audit] {} ({}) reverb={} guild={}", session->username, session->discord_id, enabled, std::string(body["guild_id"].s()));
    bool ok = music_service_->set_reverb(guild_id, enabled);

    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  // POST /osu/api/music/echo
  CROW_ROUTE(app, "/osu/api/music/echo").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto body = crow::json::load(req.body);
    if (!body || !body.has("enabled") || !body.has("guild_id")) {
      return crow::response(400, R"({"error":"Missing required fields"})");
    }

    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    bool enabled = body["enabled"].b();
    spdlog::info("[music-audit] {} ({}) echo={} guild={}", session->username, session->discord_id, enabled, std::string(body["guild_id"].s()));
    bool ok = music_service_->set_echo(guild_id, enabled);

    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  // POST /osu/api/music/seek
  CROW_ROUTE(app, "/osu/api/music/seek").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto body = crow::json::load(req.body);
    if (!body || !body.has("position") || !body.has("guild_id")) {
      return crow::response(400, R"({"error":"Missing required fields"})");
    }

    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    int position = static_cast<int>(body["position"].i());
    spdlog::info("[music-audit] {} ({}) seek={}s guild={}", session->username, session->discord_id, position, std::string(body["guild_id"].s()));
    bool ok = music_service_->seek(guild_id, position);

    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  // POST /osu/api/music/remove
  CROW_ROUTE(app, "/osu/api/music/remove").methods("POST"_method)
  ([this, is_music_allowed, check_voice_channel](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto body = crow::json::load(req.body);
    if (!body || !body.has("index") || !body.has("guild_id")) {
      return crow::response(400, R"({"error":"Missing required fields"})");
    }

    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    auto vc_err = check_voice_channel(session->discord_id, guild_id);
    if (!vc_err.empty()) {
      crow::json::wvalue err_res;
      err_res["error"] = vc_err;
      return crow::response(403, err_res);
    }

    size_t index = static_cast<size_t>(body["index"].i());
    spdlog::info("[music-audit] {} ({}) remove index={} guild={}", session->username, session->discord_id, index, std::string(body["guild_id"].s()));
    bool ok = music_service_->remove(guild_id, index);

    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  // POST /osu/api/music/remove_history — remove a track from history (admin only)
  CROW_ROUTE(app, "/osu/api/music/remove_history").methods("POST"_method)
  ([this, is_music_allowed](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (session->discord_id != MUSIC_ADMIN_ID) {
      return crow::response(403, R"({"error":"Admin only"})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto body = crow::json::load(req.body);
    if (!body || !body.has("index") || !body.has("guild_id")) {
      return crow::response(400, R"({"error":"Missing required fields"})");
    }

    auto guild_id = dpp::snowflake(std::string(body["guild_id"].s()));
    size_t index = static_cast<size_t>(body["index"].i());
    spdlog::info("[music-audit] {} ({}) remove_history index={} guild={}", session->username, session->discord_id, index, std::string(body["guild_id"].s()));
    bool ok = music_service_->remove_history(guild_id, index);

    crow::json::wvalue res;
    res["success"] = ok;
    return crow::response(200, res);
  });

  CROW_ROUTE(app, "/osu/api/music/search")
  ([this, is_music_allowed](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (!music_search_limiter_->allow(session->discord_id)) {
      return crow::response(429, R"({"error":"Too many searches, slow down"})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto q = req.url_params.get("q");
    if (!q || std::string(q).empty()) {
      return crow::response(400, R"({"error":"Missing query parameter 'q'"})");
    }

    std::string query(q);
    spdlog::info("[music-audit] {} ({}) search q={}", session->username, session->discord_id, query);
    auto results = music_service_->search(query, 5);

    crow::json::wvalue res;
    std::vector<crow::json::wvalue> items;
    for (const auto& r : results) {
      crow::json::wvalue item;
      item["url"] = r.url;
      item["title"] = r.title;
      item["thumbnail"] = r.thumbnail;
      item["duration"] = r.duration_seconds;
      item["channel"] = r.channel;
      items.push_back(std::move(item));
    }
    res["results"] = std::move(items);
    return crow::response(200, res);
  });

  CROW_ROUTE(app, "/osu/api/music/cookies")
  ([this, is_music_allowed](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (session->discord_id != MUSIC_ADMIN_ID) {
      return crow::response(403, R"({"error":"Admin only"})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto path = music_service_->get_cookies_path();
    crow::json::wvalue res;

    if (path.empty() || !std::filesystem::exists(path)) {
      res["active"] = false;
      res["filename"] = "";
      res["size"] = 0;
    } else {
      res["active"] = true;
      res["filename"] = std::filesystem::path(path).filename().string();
      res["size"] = static_cast<int64_t>(std::filesystem::file_size(path));
    }
    return crow::response(200, res);
  });

  // POST /osu/api/music/cookies — upload cookies.txt
  CROW_ROUTE(app, "/osu/api/music/cookies").methods("POST"_method)
  ([this, is_music_allowed](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (session->discord_id != MUSIC_ADMIN_ID) {
      return crow::response(403, R"({"error":"Admin only"})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto body = crow::json::load(req.body);
    if (!body || !body.has("content")) {
      return crow::response(400, R"({"error":"Missing 'content' field"})");
    }

    std::string content = body["content"].s();
    if (content.empty()) {
      return crow::response(400, R"({"error":"Empty cookies content"})");
    }

    // Basic validation: Netscape cookies.txt should start with a comment or domain
    bool looks_valid = false;
    for (const char* p = content.c_str(); *p; ++p) {
      if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
      if (*p == '#' || *p == '.') { looks_valid = true; }
      break;
    }
    if (!looks_valid) {
      // Also accept lines starting with domain names (no leading dot)
      looks_valid = content.find('\t') != std::string::npos;
    }
    if (!looks_valid) {
      return crow::response(400, R"({"error":"Invalid cookies.txt format. Export using a Netscape cookies.txt extension."})");
    }

    // Write to data/cookies.txt
    std::filesystem::create_directories("data");
    std::string cookies_path = "data/cookies.txt";

    std::ofstream out(cookies_path, std::ios::binary);
    if (!out) {
      spdlog::error("[music] Failed to write cookies file: {}", cookies_path);
      return crow::response(500, R"({"error":"Failed to save cookies file"})");
    }
    out << content;
    out.close();

    // Save a backup so we can restore if yt-dlp corrupts the original
    try {
      std::filesystem::copy_file(cookies_path, cookies_path + ".bak",
          std::filesystem::copy_options::overwrite_existing);
    } catch (...) {}

    music_service_->set_cookies_path(cookies_path);
    spdlog::info("[music-audit] {} ({}) cookies_upload size={}", session->username, session->discord_id, content.size());

    crow::json::wvalue res;
    res["success"] = true;
    res["size"] = static_cast<int64_t>(content.size());
    return crow::response(200, res);
  });

  // DELETE /osu/api/music/cookies — remove cookies
  CROW_ROUTE(app, "/osu/api/music/cookies").methods("DELETE"_method)
  ([this, is_music_allowed](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }
    if (session->discord_id != MUSIC_ADMIN_ID) {
      return crow::response(403, R"({"error":"Admin only"})");
    }
    if (!music_service_) return crow::response(503, R"({"error":"Service unavailable"})");

    auto path = music_service_->get_cookies_path();
    if (!path.empty() && std::filesystem::exists(path)) {
      std::filesystem::remove(path);
    }
    music_service_->set_cookies_path("");
    spdlog::info("[music-audit] {} ({}) cookies_delete", session->username, session->discord_id);

    crow::json::wvalue res;
    res["success"] = true;
    return crow::response(200, res);
  });

  // GET /osu/api/music/channels — list voice channels
  CROW_ROUTE(app, "/osu/api/music/channels")
  ([this, is_music_allowed](const crow::request& req) {
    auto session = get_session(req);
    if (!session) return crow::response(401, R"({"error":"Unauthorized"})");
    if (!is_music_allowed(session->discord_id)) {
      return crow::response(403, R"({"error":"Access denied. Join https://discord.gg/MV8uVdubeN and ask for music player access."})");
    }

    auto* gid_param = req.url_params.get("guild_id");
    if (!gid_param) return crow::response(400, R"({"error":"Missing guild_id"})");
    auto guild_id = dpp::snowflake(std::string(gid_param));
    auto* guild = dpp::find_guild(guild_id);
    if (!guild) {
      return crow::response(500, R"({"error":"Guild not found in cache"})");
    }

    std::vector<crow::json::wvalue> channels;
    for (auto ch_id : guild->channels) {
      auto* ch = dpp::find_channel(ch_id);
      if (ch && (ch->get_type() == dpp::CHANNEL_VOICE || ch->get_type() == dpp::CHANNEL_STAGE)) {
        crow::json::wvalue item;
        item["id"] = std::to_string(ch_id);
        item["name"] = ch->name;
        item["type"] = ch->get_type() == dpp::CHANNEL_STAGE ? "stage" : "voice";
        channels.push_back(std::move(item));
      }
    }

    crow::json::wvalue res;
    res["channels"] = std::move(channels);

    // Also report which channel user is currently in
    auto user_id = dpp::snowflake(session->discord_id);
    auto vit = guild->voice_members.find(user_id);
    if (vit != guild->voice_members.end() && vit->second.channel_id != 0) {
      res["user_channel"] = std::to_string(vit->second.channel_id);
    } else {
      res["user_channel"] = nullptr;
    }

    return crow::response(200, res);
  });

  // ==========================================================================
  // Catch-all routes (must be LAST)
  // ==========================================================================

  // Serve files from beatmap extracts
  CROW_ROUTE(app, "/osu/<string>/<path>")
  ([this](const std::string& extract_id, const std::string& file_path) {
    // Get extract path
    auto extract_path = downloader_->get_extract_path(extract_id);

    if (!extract_path) {
      return crow::response(404, "Extract not found or expired");
    }

    // Decode URL-encoded filename
    std::string decoded_filename = utils::url_decode(file_path);

    // Build full file path
    std::filesystem::path full_path = *extract_path / decoded_filename;

    // Security check: ensure path is within extract directory
    auto canonical_extract = std::filesystem::canonical(*extract_path);
    try {
      auto canonical_file = std::filesystem::canonical(full_path);
      if (canonical_file.string().find(canonical_extract.string()) != 0) {
        return crow::response(403, "Access denied");
      }
    } catch (...) {
      return crow::response(404, "File not found");
    }

    if (!std::filesystem::exists(full_path) || !std::filesystem::is_regular_file(full_path)) {
      return crow::response(404, "File not found");
    }

    std::ifstream file(full_path, std::ios::binary);
    if (!file) {
      return crow::response(500, "Failed to read file");
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    crow::response res(200, content);

    // Set content type based on extension
    std::string ext = full_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".mp3") {
      res.set_header("Content-Type", "audio/mpeg");
    } else if (ext == ".ogg") {
      res.set_header("Content-Type", "audio/ogg");
    } else if (ext == ".wav") {
      res.set_header("Content-Type", "audio/wav");
    } else if (ext == ".jpg" || ext == ".jpeg") {
      res.set_header("Content-Type", "image/jpeg");
    } else if (ext == ".png") {
      res.set_header("Content-Type", "image/png");
    } else if (ext == ".osu") {
      res.set_header("Content-Type", "text/plain; charset=utf-8");
    } else {
      res.set_header("Content-Type", "application/octet-stream");
    }

    res.set_header("Cache-Control", "public, max-age=86400"); // 24 hours
    return res;
  });

  // Download .osz file
  CROW_ROUTE(app, "/osu/<int>/download")
  ([this](int beatmapset_id) {
    auto osz_path = downloader_->get_osz_path(beatmapset_id);

    if (!osz_path) {
      return crow::response(404, ".osz file not found");
    }

    std::ifstream file(*osz_path, std::ios::binary);
    if (!file) {
      return crow::response(500, "Failed to read .osz file");
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    crow::response res(200, content);

    res.set_header("Content-Type", "application/x-osu-beatmap-archive");
    res.set_header("Content-Disposition",
                   "attachment; filename=\"" + std::to_string(beatmapset_id) + ".osz\"");
    res.set_header("Cache-Control", "public, max-age=31536000");

    return res;
  });

  app.loglevel(crow::LogLevel::Warning);
}

void HttpServer::set_crawler_service(services::MessageCrawlerService* service) {
  crawler_service_ = service;
}

void HttpServer::set_user_settings_service(services::UserSettingsService* service) {
  user_settings_service_ = service;
}

void HttpServer::set_template_service(services::EmbedTemplateService* service) {
  template_service_ = service;
}

void HttpServer::set_music_service(services::MusicPlayerService* service) {
  music_service_ = service;
  if (music_service_) {
    music_service_->set_on_state_change([this](uint64_t gid) {
      broadcast_music_state(gid);
    });
  }
}

std::string HttpServer::build_state_json(uint64_t guild_id) {
  if (!music_service_) return R"({"error":"Music service unavailable"})";

  auto state = music_service_->get_state(dpp::snowflake(guild_id));
  json res;
  res["state"] = services::playback_state_to_string(state.state);
  res["volume"] = state.volume;
  res["speed"] = state.speed;
  res["nightcore"] = state.nightcore;
  res["reverb"] = state.reverb;
  res["echo"] = state.echo;
  res["pipeline_pending"] = state.pipeline_pending;
  res["error"] = state.error;
  res["elapsed"] = state.elapsed_seconds;
  res["bpm"] = state.detected_bpm;

  if (state.now_playing) {
    json np;
    np["title"] = state.now_playing->title;
    np["url"] = state.now_playing->url;
    np["duration"] = state.now_playing->duration_seconds;
    np["requester"] = state.now_playing->requester;
    np["thumbnail"] = state.now_playing->thumbnail;
    if (!state.now_playing->chapters.empty()) {
      json chapters = json::array();
      for (const auto& ch : state.now_playing->chapters) {
        chapters.push_back({{"title", ch.title}, {"start", ch.start_time}, {"end", ch.end_time}});
      }
      np["chapters"] = std::move(chapters);
    }
    res["now_playing"] = std::move(np);
  } else {
    res["now_playing"] = nullptr;
  }

  json queue_items = json::array();
  for (const auto& track : state.queue) {
    json item;
    item["title"] = track.title;
    item["url"] = track.url;
    item["duration"] = track.duration_seconds;
    item["requester"] = track.requester;
    item["thumbnail"] = track.thumbnail;
    queue_items.push_back(std::move(item));
  }
  res["queue"] = std::move(queue_items);

  json history_items = json::array();
  for (const auto& track : state.history) {
    json item;
    item["title"] = track.title;
    item["url"] = track.url;
    item["duration"] = track.duration_seconds;
    item["requester"] = track.requester;
    item["thumbnail"] = track.thumbnail;
    history_items.push_back(std::move(item));
  }
  res["history"] = std::move(history_items);

  return res.dump();
}

void HttpServer::broadcast_music_state(uint64_t guild_id) {
  std::string payload = build_state_json(guild_id);
  std::lock_guard lock(ws_mutex_);
  auto it = ws_guild_connections_.find(guild_id);
  if (it == ws_guild_connections_.end()) {
    spdlog::debug("[ws] No WS connections for guild {}", guild_id);
    return;
  }
  spdlog::debug("[ws] Broadcasting to {} connection(s) for guild {}", it->second.size(), guild_id);
  for (auto* conn : it->second) {
    try {
      conn->send_text(payload);
    } catch (const std::exception& e) {
      spdlog::warn("[ws] Failed to send to connection: {}", e.what());
    }
  }
}

void HttpServer::set_config(const Config* config) {
  config_ = config;
}

HttpServer::~HttpServer() {
  stop();
}

void HttpServer::start() {
  if (running_.exchange(true)) {
    spdlog::warn("HTTP server already running");
    return;
  }

  start_time_ = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  server_thread_ = std::make_unique<std::thread>([this] { run(); });
  SPDLOG_INFO("HTTP server started on {}:{}", host_, port_);
}

void HttpServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  // Clear the callback to avoid dangling reference
  if (music_service_) {
    music_service_->set_on_state_change(nullptr);
  }

  // Close all WebSocket connections
  {
    std::lock_guard lock(ws_mutex_);
    for (auto& [gid, conns] : ws_guild_connections_) {
      for (auto* conn : conns) {
        try { conn->close("server shutting down"); } catch (...) {}
      }
    }
    ws_guild_connections_.clear();
  }

  if (app_) {
    app_->stop();
  }

  if (server_thread_ && server_thread_->joinable()) {
    server_thread_->join();
  }

  SPDLOG_INFO("HTTP server stopped");
}

void HttpServer::run() {
  try {
    app_->port(port_).bindaddr(host_).multithreaded().run();
  } catch (const std::exception& e) {
    SPDLOG_ERROR("HTTP server error: {}", e.what());
    running_ = false;
  }
}

std::string HttpServer::get_client_ip(const crow::request& req) const {
  // Check X-Forwarded-For header (for reverse proxy setups)
  auto xff = req.get_header_value("X-Forwarded-For");
  if (!xff.empty()) {
    // Take the first IP in the chain (original client)
    auto comma_pos = xff.find(',');
    if (comma_pos != std::string::npos) {
      return xff.substr(0, comma_pos);
    }
    return xff;
  }

  // Check X-Real-IP header
  auto xri = req.get_header_value("X-Real-IP");
  if (!xri.empty()) {
    return xri;
  }

  // Fall back to remote IP
  return req.remote_ip_address;
}
