#include <http_server.h>
#include <beatmap_downloader.h>
#include <database.h>
#include <utils.h>
#include <debug_settings.h>
#include <services/message_crawler_service.h>

#include <crow.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

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
      download_limiter_(std::make_unique<RateLimiter>(5, std::chrono::seconds(60))) {

  auto& app = *app_;
  app.signal_clear(); // avoid Crow overriding our SIGINT/SIGTERM handlers

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

  // Debug settings - GET current state
  CROW_ROUTE(app, "/debug")
  ([]() {
    auto& settings = debug::Settings::instance();

    crow::json::wvalue response;
    response["verbose_osu_api"] = settings.verbose_osu_api.load();
    response["verbose_rosu_pp"] = settings.verbose_rosu_pp.load();
    response["max_response_log_length"] = static_cast<int64_t>(settings.max_response_log_length.load());

    return crow::response(200, response);
  });

  // Debug settings - POST to toggle
  CROW_ROUTE(app, "/debug").methods("POST"_method)
  ([](const crow::request& req) {
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
      return crow::response(400, std::string(R"({"error": ")") + e.what() + "\"}");
    }
  });

  // Commands documentation endpoint
  CROW_ROUTE(app, "/osu/commands")
  ([]() {
    crow::json::wvalue response;

    // Create array of commands
    crow::json::wvalue::list commands_list;

    // !rs command
    {
      crow::json::wvalue cmd;
      cmd["name"] = "!rs";
      cmd["aliases"] = crow::json::wvalue::list({"!rs", "!кы"});
      cmd["description"] = "Show recent or best scores for a player";
      cmd["usage"] = "!rs[:mode] [user] [-p] [-b] [-i INDEX] [-m MODE]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = ":mode";
      p1["description"] = "Specify game mode (osu, taiko, catch, mania)";
      p1["example"] = "!rs:taiko";
      params.push_back(std::move(p1));

      crow::json::wvalue p2;
      p2["flag"] = "user";
      p2["description"] = "Target user (username, Discord mention, or empty for self)";
      p2["example"] = "!rs peppy  OR  !rs <@123456789>";
      params.push_back(std::move(p2));

      crow::json::wvalue p3;
      p3["flag"] = "-p";
      p3["description"] = "Show only passed scores (exclude fails)";
      p3["example"] = "!rs -p";
      params.push_back(std::move(p3));

      crow::json::wvalue p4;
      p4["flag"] = "-b";
      p4["description"] = "Show best scores (top 100) instead of recent";
      p4["example"] = "!rs -b";
      params.push_back(std::move(p4));

      crow::json::wvalue p5;
      p5["flag"] = "-i INDEX";
      p5["description"] = "Show specific score by index (1-based)";
      p5["example"] = "!rs -i 5";
      params.push_back(std::move(p5));

      crow::json::wvalue p6;
      p6["flag"] = "-m MODE";
      p6["description"] = "Override game mode (osu, taiko, catch/fruits, mania)";
      p6["example"] = "!rs -m taiko";
      params.push_back(std::move(p6));

      cmd["parameters"] = std::move(params);

      crow::json::wvalue::list examples;
      examples.push_back("!rs");
      examples.push_back("!rs -p");
      examples.push_back("!rs peppy -i 3");
      examples.push_back("!rs:taiko -b");
      examples.push_back("!rs <@123456789> -p");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !m / !map command
    {
      crow::json::wvalue cmd;
      cmd["name"] = "!m";
      cmd["aliases"] = crow::json::wvalue::list({"!m", "!map"});
      cmd["description"] = "Show detailed information about the current beatmap";
      cmd["usage"] = "!m [+MODS]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = "+MODS";
      p1["description"] = "Calculate difficulty with specific mods";
      p1["example"] = "!m +HDDT";
      params.push_back(std::move(p1));

      cmd["parameters"] = std::move(params);

      crow::json::wvalue::list examples;
      examples.push_back("!m");
      examples.push_back("!m +HDDT");
      examples.push_back("!m +HR");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !sim command
    {
      crow::json::wvalue cmd;
      cmd["name"] = "!sim";
      cmd["aliases"] = crow::json::wvalue::list({"!sim"});
      cmd["description"] = "Simulate a score with specific accuracy and parameters";
      cmd["usage"] = "!sim[:mode] ACCURACY% [+MODS] [-c COMBO] [-n100 X] [-n50 X] [-n0 X] [-r RATIO]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = ":mode";
      p1["description"] = "Specify game mode (osu, taiko, catch, mania)";
      p1["example"] = "!sim:taiko";
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
      examples.push_back("!sim 99%");
      examples.push_back("!sim 100% +HDDT");
      examples.push_back("!sim:taiko 99.5% +HR");
      examples.push_back("!sim 99% -n100 5 -c 1500");
      examples.push_back("!sim:mania 98% -r 0.95");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !lb command
    {
      crow::json::wvalue cmd;
      cmd["name"] = "!lb";
      cmd["aliases"] = crow::json::wvalue::list({"!lb", "!leaderboard"});
      cmd["description"] = "Show server leaderboard for current beatmap";
      cmd["usage"] = "!lb [+MODS]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = "+MODS";
      p1["description"] = "Filter by specific mods";
      p1["example"] = "!lb +HDDT";
      params.push_back(std::move(p1));

      cmd["parameters"] = std::move(params);

      crow::json::wvalue::list examples;
      examples.push_back("!lb");
      examples.push_back("!lb +HDDT");
      examples.push_back("!lb +HR");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !c / !compare command
    {
      crow::json::wvalue cmd;
      cmd["name"] = "!c";
      cmd["aliases"] = crow::json::wvalue::list({"!c", "!compare"});
      cmd["description"] = "Show all scores for a player on current beatmap";
      cmd["usage"] = "!c [user] [+MODS]";

      crow::json::wvalue::list params;

      crow::json::wvalue p1;
      p1["flag"] = "user";
      p1["description"] = "Target user (username, Discord mention, or empty for self)";
      p1["example"] = "!c peppy  OR  !c <@123456789>";
      params.push_back(std::move(p1));

      crow::json::wvalue p2;
      p2["flag"] = "+MODS";
      p2["description"] = "Filter by specific mods";
      p2["example"] = "!c +HDDT";
      params.push_back(std::move(p2));

      cmd["parameters"] = std::move(params);

      crow::json::wvalue::list examples;
      examples.push_back("!c");
      examples.push_back("!c peppy");
      examples.push_back("!c +HDDT");
      examples.push_back("!c <@123456789> +HR");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !bg command
    {
      crow::json::wvalue cmd;
      cmd["name"] = "!bg";
      cmd["aliases"] = crow::json::wvalue::list({"!bg"});
      cmd["description"] = "Get background image from current beatmap";
      cmd["usage"] = "!bg";
      cmd["parameters"] = crow::json::wvalue::list();

      crow::json::wvalue::list examples;
      examples.push_back("!bg");
      cmd["examples"] = std::move(examples);

      commands_list.push_back(std::move(cmd));
    }

    // !song / !audio command
    {
      crow::json::wvalue cmd;
      cmd["name"] = "!song";
      cmd["aliases"] = crow::json::wvalue::list({"!song", "!audio"});
      cmd["description"] = "Get audio file from current beatmap";
      cmd["usage"] = "!song";
      cmd["parameters"] = crow::json::wvalue::list();

      crow::json::wvalue::list examples;
      examples.push_back("!song");
      examples.push_back("!audio");
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
      crow::json::wvalue error;
      error["error"] = "Failed to retrieve file inventory";
      error["details"] = e.what();
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
      return crow::response(500, fmt::format("Error: {}", e.what()));
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
      return crow::response(500, fmt::format("Error: {}", e.what()));
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

  // GET /osu/api/guild - Guild info
  CROW_ROUTE(app, "/osu/api/guild")
  ([]() {
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
      error["error"] = "Failed to retrieve guild info";
      error["details"] = e.what();
      return crow::response(500, error);
    }
  });

  // GET /osu/api/users - List of users with message counts
  CROW_ROUTE(app, "/osu/api/users")
  ([this]() {
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
      error["error"] = "Failed to retrieve users";
      error["details"] = e.what();
      return crow::response(500, error);
    }
  });

  // GET /osu/api/channels - List of channels with message counts
  CROW_ROUTE(app, "/osu/api/channels")
  ([this]() {
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
      error["error"] = "Failed to retrieve channels";
      error["details"] = e.what();
      return crow::response(500, error);
    }
  });

  // GET /osu/api/wordstats - JSON with top words
  CROW_ROUTE(app, "/osu/api/wordstats")
  ([this](const crow::request& req) {
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
      crow::json::wvalue error;
      error["error"] = "Failed to retrieve word statistics";
      error["details"] = e.what();
      return crow::response(500, error);
    }
  });

  // GET /osu/api/phrasestats - JSON with top phrases
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
      crow::json::wvalue error;
      error["error"] = "Failed to retrieve phrase statistics";
      error["details"] = e.what();
      return crow::response(500, error);
    }
  });

  // GET /osu/api/crawl-status - Current crawl progress
  CROW_ROUTE(app, "/osu/api/crawl-status")
  ([this]() {
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
      crow::json::wvalue error;
      error["error"] = "Failed to retrieve crawl status";
      error["details"] = e.what();
      return crow::response(500, error);
    }
  });

  // POST /osu/api/refresh-stats - Trigger immediate stats recalculation
  CROW_ROUTE(app, "/osu/api/refresh-stats").methods("POST"_method)
  ([this]() {
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
      crow::json::wvalue error;
      error["error"] = "Failed to trigger stats refresh";
      error["details"] = e.what();
      return crow::response(500, error);
    }
  });

  // GET /osu/wordstats - HTML page displaying stats
  CROW_ROUTE(app, "/osu/wordstats")
  ([]() {
    // Try multiple paths for static file
    std::vector<std::filesystem::path> paths = {
      "static/wordstats.html",
      "../static/wordstats.html",
      "/home/nisemonic/patchouli/bot/static/wordstats.html"
    };

    std::filesystem::path static_file;
    for (const auto& p : paths) {
      spdlog::debug("[HTTP] Checking path: {} exists={}", p.string(), std::filesystem::exists(p));
      if (std::filesystem::exists(p)) {
        static_file = p;
        break;
      }
    }

    if (static_file.empty()) {
      spdlog::warn("[HTTP] wordstats.html not found in any path");
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

  // GET /osu/phrasestats - HTML page displaying phrase stats
  CROW_ROUTE(app, "/osu/phrasestats")
  ([]() {
    std::vector<std::filesystem::path> paths = {
      "static/phrasestats.html",
      "../static/phrasestats.html",
      "/home/nisemonic/patchouli/bot/static/phrasestats.html"
    };

    std::filesystem::path static_file;
    for (const auto& p : paths) {
      if (std::filesystem::exists(p)) {
        static_file = p;
        break;
      }
    }

    if (static_file.empty()) {
      spdlog::warn("[HTTP] phrasestats.html not found in any path");
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
