#include <beatmap_downloader.h>
#include <database.h>
#include <utils.h>
#include <services/webhook_service.h>

#include <cpr/cpr.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <zip.h>
#include <zlib.h>

#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace {
std::string extract_host(const std::string& url) {
  size_t start = url.find("://");
  start = start == std::string::npos ? 0 : start + 3;
  size_t end = url.find('/', start);
  if (end == std::string::npos) end = url.size();
  return url.substr(start, end - start);
}
} // namespace

BeatmapDownloader::BeatmapDownloader()
    : data_dir_(".data"),
      osz_dir_(data_dir_ / "osz"),
      osu_files_dir_(data_dir_ / "osu"),
      extracts_dir_(data_dir_ / "extracts") {
  // Default mirrors (chimu.moe is dead - NXDOMAIN)
  mirrors_ = {
    "https://api.nerinyan.moe/d",
    "https://catboy.best/d"
  };
  ensure_directories();
}

BeatmapDownloader::BeatmapDownloader(const std::vector<std::string>& mirrors)
    : data_dir_(".data"),
      osz_dir_(data_dir_ / "osz"),
      osu_files_dir_(data_dir_ / "osu"),
      extracts_dir_(data_dir_ / "extracts"),
      mirrors_(mirrors) {
  ensure_directories();
}

void BeatmapDownloader::set_mirrors(const std::vector<std::string>& mirrors) {
  mirrors_ = mirrors;
  spdlog::info("[DOWNLOAD] Updated mirror list ({} mirrors)", mirrors_.size());
}

std::string BeatmapDownloader::get_mirror_url(uint32_t beatmapset_id) const {
  if (mirrors_.empty()) {
    return fmt::format("https://catboy.best/d/{}", beatmapset_id);
  }
  return fmt::format("{}/{}", mirrors_[0], beatmapset_id);
}

void BeatmapDownloader::ensure_directories() {
  try {
    fs::create_directories(osz_dir_);
    fs::create_directories(osu_files_dir_);
    fs::create_directories(extracts_dir_);
  } catch (const fs::filesystem_error& e) {
    spdlog::error("Failed to create directories: {}", e.what());
  }
}

bool BeatmapDownloader::beatmapset_exists(uint32_t beatmapset_id) const {
  fs::path osz_path = osz_dir_ / (std::to_string(beatmapset_id) + ".osz");
  return fs::exists(osz_path);
}

std::optional<fs::path> BeatmapDownloader::get_osz_path(uint32_t beatmapset_id) const {
  fs::path osz_path = osz_dir_ / (std::to_string(beatmapset_id) + ".osz");
  if (fs::exists(osz_path)) {
    return osz_path;
  }
  return std::nullopt;
}

bool BeatmapDownloader::try_download_from_mirror(const std::string& mirror_url,
                                                  uint32_t beatmapset_id,
                                                  const fs::path& dest_path) {
  std::string url = mirror_url + "/" + std::to_string(beatmapset_id);
  spdlog::info("[DOWNLOAD] Trying: {}", url);

  // Use temporary file to avoid corruption during download
  fs::path temp_path = dest_path;
  temp_path += ".tmp";

  std::ofstream output(temp_path, std::ios::binary);
  if (!output) {
    spdlog::error("[DOWNLOAD] Failed to open file for writing: {}", temp_path.string());
    return false;
  }

  // 2 second timeout - fail fast and try next mirror
  auto response = cpr::Download(output, cpr::Url{url}, cpr::Timeout{30000});
  output.close();

  spdlog::info("[DOWNLOAD] HTTP response: status={}, bytes={}",
    response.status_code, response.downloaded_bytes);

  if (response.status_code == 200 && response.downloaded_bytes > 0) {
    // Verify Content-Length if available
    auto content_length_it = response.header.find("Content-Length");
    if (content_length_it != response.header.end()) {
      try {
        int64_t expected_size = std::stoll(content_length_it->second);
        if (response.downloaded_bytes != expected_size) {
          spdlog::warn("[DOWNLOAD] Size mismatch: downloaded {} bytes, expected {} bytes",
            response.downloaded_bytes, expected_size);
          fs::remove(temp_path);
          return false;
        }
      } catch (const std::exception& e) {
        spdlog::warn("[DOWNLOAD] Failed to parse Content-Length: {}", e.what());
      }
    }

    // Verify ZIP file integrity by trying to open it
    int err = 0;
    zip_t* archive = zip_open(temp_path.string().c_str(), ZIP_RDONLY, &err);
    if (!archive) {
      spdlog::error("[DOWNLOAD] Downloaded file is not a valid ZIP (error code {})", err);
      fs::remove(temp_path);
      return false;
    }
    zip_close(archive);

    // Move temp file to final destination atomically
    std::error_code ec;
    fs::rename(temp_path, dest_path, ec);
    if (ec) {
      spdlog::error("[DOWNLOAD] Failed to rename temp file: {}", ec.message());
      fs::remove(temp_path);
      return false;
    }

    spdlog::info("[DOWNLOAD] Successfully downloaded {} bytes from {}",
      response.downloaded_bytes, mirror_url);
    return true;
  }

  // Failed - try to read error response body before cleanup
  std::string error_msg;
  if (response.downloaded_bytes > 0 && response.downloaded_bytes < 1024) {
    std::ifstream error_file(temp_path);
    if (error_file) {
      std::stringstream buffer;
      buffer << error_file.rdbuf();
      std::string body = buffer.str();
      // Try to parse as JSON and extract "error" field
      try {
        auto json = nlohmann::json::parse(body);
        if (json.contains("error")) {
          error_msg = json["error"].get<std::string>();
        }
      } catch (...) {
        // Not JSON or no error field, use raw body
        error_msg = body;
      }
    }
  }
  fs::remove(temp_path);

  if (response.status_code == 404) {
    spdlog::warn("[DOWNLOAD] Beatmapset {} not found on {}", beatmapset_id, mirror_url);
  } else if (response.status_code == 429) {
    spdlog::warn("[DOWNLOAD] Rate limited by {}", mirror_url);
  } else if (response.status_code == 0) {
    spdlog::warn("[DOWNLOAD] Failed: {}", response.error.message);
  } else {
    spdlog::warn("[DOWNLOAD] Failed: HTTP {} - {}", response.status_code,
      error_msg.empty() ? response.error.message : error_msg);
  }

  return false;
}

void BeatmapDownloader::demote_mirror(size_t index) {
  if (index >= mirrors_.size() - 1) return;  // Already at end or invalid

  std::string failed_mirror = mirrors_[index];
  mirrors_.erase(mirrors_.begin() + index);
  mirrors_.push_back(failed_mirror);

  spdlog::debug("[DOWNLOAD] Demoted {} to end of mirror list", failed_mirror);
}

bool BeatmapDownloader::download_osz_with_attempts(uint32_t beatmapset_id,
                                                    std::vector<MirrorAttemptResult>& attempts) {
  spdlog::info("[DOWNLOAD] Downloading beatmapset {} (https://osu.ppy.sh/s/{})",
               beatmapset_id, beatmapset_id);

  // Check if mirrors are in cooldown
  if (are_mirrors_in_cooldown()) {
    MirrorAttemptResult cooldown_attempt;
    cooldown_attempt.mirror_url = "all";
    cooldown_attempt.error_message = "Mirrors in cooldown due to repeated failures";
    attempts.push_back(cooldown_attempt);
    spdlog::info("[DOWNLOAD] Mirrors in cooldown, skipping download for beatmapset {}", beatmapset_id);
    return false;
  }

  std::lock_guard<std::mutex> lock(download_mutex_);

  // Check if already exists
  if (beatmapset_exists(beatmapset_id)) {
    spdlog::info("[DOWNLOAD] Beatmapset {} already exists, skipping download", beatmapset_id);
    last_used_mirror_ = "cache";
    return true;
  }

  fs::path osz_path = osz_dir_ / (std::to_string(beatmapset_id) + ".osz");
  fs::path temp_path = osz_path;
  temp_path += ".tmp";

  const size_t max_attempts = mirrors_.size();
  size_t attempts_made = 0;

  for (size_t i = 0; i < mirrors_.size() && attempts_made < max_attempts; i++) {
    // Check for shutdown request
    if (is_shutdown_requested()) {
      spdlog::info("[DOWNLOAD] Aborting download due to shutdown request");
      fs::remove(temp_path);
      return false;
    }

    attempts_made++;
    const auto& mirror = mirrors_[i];
    std::string url = mirror + "/" + std::to_string(beatmapset_id);

    MirrorAttemptResult attempt;
    attempt.mirror_url = mirror;

    auto start_time = std::chrono::steady_clock::now();

    std::ofstream output(temp_path, std::ios::binary);
    if (!output) {
      attempt.error_message = "Failed to open temp file for writing";
      attempt.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start_time);
      attempts.push_back(attempt);
      continue;
    }

    auto response = cpr::Download(output, cpr::Url{url}, cpr::Timeout{30000});
    output.close();

    attempt.status_code = response.status_code;
    attempt.bytes_downloaded = response.downloaded_bytes;
    attempt.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);

    if (response.status_code == 200 && response.downloaded_bytes > 0) {
      // Verify ZIP
      int err = 0;
      zip_t* archive = zip_open(temp_path.string().c_str(), ZIP_RDONLY, &err);
      if (!archive) {
        attempt.error_message = fmt::format("Invalid ZIP file (error {})", err);
        attempts.push_back(attempt);
        fs::remove(temp_path);
        demote_mirror(i);
        i--;
        continue;
      }
      zip_close(archive);

      // Success - move to final path
      std::error_code ec;
      fs::rename(temp_path, osz_path, ec);
      if (ec) {
        attempt.error_message = fmt::format("Failed to rename: {}", ec.message());
        attempts.push_back(attempt);
        fs::remove(temp_path);
        continue;
      }

      attempts.push_back(attempt);
      last_used_mirror_ = extract_host(mirror);
      reset_mirror_cooldown();  // Success - reset failure counter

      // Register in database
      try {
        auto& db = db::Database::instance();
        int64_t file_size = std::filesystem::file_size(osz_path);
        db.register_beatmap_file(beatmapset_id, osz_path.string(),
                                last_used_mirror_ == "cache" ? std::nullopt : std::optional(last_used_mirror_),
                                file_size);
      } catch (const std::exception& e) {
        spdlog::warn("[DOWNLOAD] Failed to register in database: {}", e.what());
      }

      return true;
    }

    // Failed - try to read error response body before cleanup
    std::string error_msg;
    if (response.downloaded_bytes > 0 && response.downloaded_bytes < 1024) {
      std::ifstream error_file(temp_path);
      if (error_file) {
        std::stringstream buffer;
        buffer << error_file.rdbuf();
        std::string body = buffer.str();
        try {
          auto json = nlohmann::json::parse(body);
          if (json.contains("error")) {
            error_msg = json["error"].get<std::string>();
          }
        } catch (...) {
          error_msg = body;
        }
      }
    }
    fs::remove(temp_path);

    // Determine if this is a "not found" response (not a mirror failure)
    bool is_not_found = (response.status_code == 404) ||
                        (response.status_code == 500 && error_msg.find("not found") != std::string::npos);

    if (response.status_code == 404) {
      attempt.error_message = "Not found (404)";
    } else if (response.status_code == 429) {
      attempt.error_message = "Rate limited (429)";
    } else if (response.status_code == 0) {
      attempt.error_message = fmt::format("Connection error: {}", response.error.message);
    } else {
      attempt.error_message = fmt::format("HTTP {}: {}", response.status_code,
        error_msg.empty() ? response.error.message : error_msg);
    }

    spdlog::warn("[DOWNLOAD] Failed: {}", attempt.error_message);
    attempts.push_back(attempt);

    // Only demote mirror for actual failures, not "not found"
    if (!is_not_found) {
      demote_mirror(i);
      i--;
    }
  }

  // All mirrors failed for this beatmapset
  spdlog::warn("[DOWNLOAD] All {} mirrors failed for beatmapset {}", attempts.size(), beatmapset_id);

  // Check if any attempt was an actual failure (not just "not found")
  bool has_actual_failure = false;
  for (const auto& a : attempts) {
    bool is_not_found = a.error_message.find("Not found") != std::string::npos ||
                        a.error_message.find("not found") != std::string::npos;
    if (!is_not_found) {
      has_actual_failure = true;
      break;
    }
  }

  // Increment failure counter only for actual failures, not "not found"
  bool should_send_webhook = false;
  if (has_actual_failure) {
    std::lock_guard<std::mutex> lock(cooldown_mutex_);
    consecutive_all_mirrors_failed_++;
    if (consecutive_all_mirrors_failed_ >= kMaxConsecutiveFailures) {
      mirror_cooldown_until_ = std::chrono::steady_clock::now() + kCooldownDuration;
      spdlog::warn("[DOWNLOAD] {} consecutive failures, entering {}s cooldown",
        consecutive_all_mirrors_failed_, kCooldownDuration.count());

      // Send webhook only once per cooldown period
      if (!cooldown_webhook_sent_) {
        cooldown_webhook_sent_ = true;
        should_send_webhook = true;
      }
    }
  }

  // Send webhook outside of lock
  if (should_send_webhook) {
    send_cooldown_notification();
  }

  last_used_mirror_.clear();
  return false;
}

bool BeatmapDownloader::are_mirrors_in_cooldown() const {
  std::lock_guard<std::mutex> lock(cooldown_mutex_);
  if (consecutive_all_mirrors_failed_ >= kMaxConsecutiveFailures) {
    auto now = std::chrono::steady_clock::now();
    if (now < mirror_cooldown_until_) {
      return true;
    }
  }
  return false;
}

void BeatmapDownloader::reset_mirror_cooldown() {
  std::lock_guard<std::mutex> lock(cooldown_mutex_);
  consecutive_all_mirrors_failed_ = 0;
  cooldown_webhook_sent_ = false;
}

void BeatmapDownloader::set_webhook_service(services::WebhookService* service) {
  webhook_service_ = service;
}

void BeatmapDownloader::request_shutdown() {
  shutdown_requested_.store(true, std::memory_order_release);
  spdlog::info("[DOWNLOAD] Shutdown requested");
}

bool BeatmapDownloader::is_shutdown_requested() const {
  return shutdown_requested_.load(std::memory_order_acquire);
}

void BeatmapDownloader::send_cooldown_notification() {
  if (!webhook_service_) {
    return;
  }

  std::string mirrors_list;
  for (const auto& m : mirrors_) {
    mirrors_list += "• " + m + "\n";
  }

  webhook_service_->notify(
    services::WebhookChannel::MirrorErrors,
    services::NotificationLevel::Error,
    "Mirror Download Failure",
    fmt::format(
      "All beatmap mirrors failed **{}** times in a row.\n"
      "Entering **{}s cooldown** - downloads will use osu.ppy.sh fallback.\n\n"
      "**Mirrors:**\n{}",
      kMaxConsecutiveFailures, kCooldownDuration.count(), mirrors_list)
  );
}

bool BeatmapDownloader::download_osz_with_fallback(uint32_t beatmapset_id,
                                                     const fs::path& dest_path) {
  // Check if mirrors are in cooldown
  if (are_mirrors_in_cooldown()) {
    spdlog::info("[DOWNLOAD] Mirrors in cooldown, skipping download for beatmapset {}", beatmapset_id);
    return false;
  }

  spdlog::info("[DOWNLOAD] Trying {} mirrors for beatmapset {}",
    mirrors_.size(), beatmapset_id);

  const size_t max_attempts = mirrors_.size();
  size_t attempts_made = 0;

  for (size_t i = 0; i < mirrors_.size() && attempts_made < max_attempts; i++) {
    // Check for shutdown request
    if (is_shutdown_requested()) {
      spdlog::info("[DOWNLOAD] Aborting download due to shutdown request");
      return false;
    }

    attempts_made++;
    const auto& mirror = mirrors_[i];
    spdlog::info("[DOWNLOAD] Trying mirror {}/{}: {}",
      attempts_made, max_attempts, mirror);

    if (try_download_from_mirror(mirror, beatmapset_id, dest_path)) {
      last_used_mirror_ = extract_host(mirror);
      reset_mirror_cooldown();  // Success - reset failure counter
      return true;
    }

    // Demote failed mirror to end of list for future requests
    demote_mirror(i);
    i--;  // Adjust index since we removed current element
  }

  // All mirrors failed - increment failure counter and possibly enter cooldown
  bool should_send_webhook = false;
  {
    std::lock_guard<std::mutex> lock(cooldown_mutex_);
    consecutive_all_mirrors_failed_++;
    if (consecutive_all_mirrors_failed_ >= kMaxConsecutiveFailures) {
      mirror_cooldown_until_ = std::chrono::steady_clock::now() + kCooldownDuration;
      spdlog::warn("[DOWNLOAD] All mirrors failed {} times in a row, entering {}s cooldown",
        consecutive_all_mirrors_failed_, kCooldownDuration.count());

      // Send webhook only once per cooldown period
      if (!cooldown_webhook_sent_) {
        cooldown_webhook_sent_ = true;
        should_send_webhook = true;
      }
    }
  }

  // Send webhook outside of lock
  if (should_send_webhook) {
    send_cooldown_notification();
  }

  spdlog::error("[DOWNLOAD] All {} mirrors failed for beatmapset {}",
    mirrors_.size(), beatmapset_id);
  last_used_mirror_.clear();
  return false;
}

bool BeatmapDownloader::extract_osz(const fs::path& osz_path, const fs::path& extract_dir) {
  int err = 0;
  zip_t* archive = zip_open(osz_path.string().c_str(), ZIP_RDONLY, &err);

  if (!archive) {
    spdlog::error("[EXTRACT] Failed to open archive {}: error code {}", osz_path.string(), err);
    return false;
  }

  // Create extract directory
  fs::create_directories(extract_dir);

  // Extract all files
  int64_t num_entries = zip_get_num_entries(archive, 0);
  spdlog::info("[EXTRACT] Extracting {} files to {}", num_entries, extract_dir.string());

  for (int64_t i = 0; i < num_entries; i++) {
    const char* name = zip_get_name(archive, i, 0);
    if (!name) continue;

    fs::path file_path = extract_dir / name;

    // Skip directories
    if (name[strlen(name) - 1] == '/') {
      fs::create_directories(file_path);
      continue;
    }

    // Create parent directories
    fs::create_directories(file_path.parent_path());

    // Extract file
    zip_file_t* zf = zip_fopen_index(archive, i, 0);
    if (!zf) {
      spdlog::warn("[EXTRACT] Failed to open file {} in archive", name);
      continue;
    }

    std::ofstream output(file_path, std::ios::binary);
    if (!output) {
      spdlog::warn("[EXTRACT] Failed to create output file {}", file_path.string());
      zip_fclose(zf);
      continue;
    }

    char buffer[8192];
    int64_t bytes_read;
    while ((bytes_read = zip_fread(zf, buffer, sizeof(buffer))) > 0) {
      output.write(buffer, bytes_read);
    }

    output.close();
    zip_fclose(zf);
  }

  zip_close(archive);
  spdlog::info("[EXTRACT] Successfully extracted beatmapset to {}", extract_dir.string());
  return true;
}

bool BeatmapDownloader::download_osz(uint32_t beatmapset_id) {
  spdlog::info("[DOWNLOAD] Starting download for beatmapset {}", beatmapset_id);

  // Lock to prevent concurrent downloads of the same beatmapset
  std::lock_guard<std::mutex> lock(download_mutex_);

  // Check again after acquiring lock (another thread might have downloaded it)
  if (beatmapset_exists(beatmapset_id)) {
    spdlog::info("[DOWNLOAD] Beatmapset {} already exists, skipping download", beatmapset_id);
    last_used_mirror_ = "cache";
    return true;
  }

  fs::path osz_path = osz_dir_ / (std::to_string(beatmapset_id) + ".osz");
  spdlog::info("[DOWNLOAD] Downloading to {}", osz_path.string());

  if (!download_osz_with_fallback(beatmapset_id, osz_path)) {
    spdlog::error("[DOWNLOAD] Failed to download beatmapset {}", beatmapset_id);
    return false;
  }

  spdlog::info("[DOWNLOAD] Successfully downloaded beatmapset {}", beatmapset_id);

  // Register .osz file in database with file size
  try {
    auto& db = db::Database::instance();
    int64_t file_size = std::filesystem::file_size(osz_path);
    db.register_beatmap_file(beatmapset_id, osz_path.string(),
                            last_used_mirror_ == "cache" ? std::nullopt : std::optional(last_used_mirror_),
                            file_size);
  } catch (const std::exception& e) {
    spdlog::warn("[DOWNLOAD] Failed to register in database: {}", e.what());
  }

  return true;
}

// Extract management
std::optional<std::string> BeatmapDownloader::create_extract(uint32_t beatmapset_id) {
  try {
    auto& db = db::Database::instance();

    // Check if .osz exists
    auto osz_path = get_osz_path(beatmapset_id);
    if (!osz_path) {
      spdlog::warn("[EXTRACT] .osz file not found for beatmapset {}", beatmapset_id);
      return std::nullopt;
    }

    // Create unique extract directory
    std::string extract_id;
    fs::path extract_dir;

    // Generate extract_id and path
    extract_id = db.create_beatmap_extract(beatmapset_id, "", std::chrono::hours(24));
    extract_dir = extracts_dir_ / extract_id;

    // Extract .osz file
    if (!extract_osz(*osz_path, extract_dir)) {
      spdlog::error("[EXTRACT] Failed to extract beatmapset {}", beatmapset_id);
      db.remove_beatmap_extract(extract_id);
      return std::nullopt;
    }

    // Update database with actual extract path
    db.remove_beatmap_extract(extract_id);
    extract_id = db.create_beatmap_extract(beatmapset_id, extract_dir.string(), std::chrono::hours(24));

    // Update last accessed time
    db.update_file_access(beatmapset_id);

    spdlog::info("[EXTRACT] Created extract {} for beatmapset {} (TTL: 24h)", extract_id, beatmapset_id);
    return extract_id;
  } catch (const std::exception& e) {
    spdlog::error("[EXTRACT] Error creating extract: {}", e.what());
    return std::nullopt;
  }
}

std::optional<fs::path> BeatmapDownloader::get_extract_path(const std::string& extract_id) {
  try {
    auto& db = db::Database::instance();
    auto extract = db.get_beatmap_extract(extract_id);

    if (!extract) {
      return std::nullopt;
    }

    // Check if expired
    if (extract->expires_at < std::chrono::system_clock::now()) {
      spdlog::info("[EXTRACT] Extract {} has expired, removing", extract_id);
      db.remove_beatmap_extract(extract_id);
      fs::remove_all(extract->extract_path);
      return std::nullopt;
    }

    // Verify extract directory exists
    if (!fs::exists(extract->extract_path)) {
      spdlog::warn("[EXTRACT] Extract {} directory not found, removing from database", extract_id);
      db.remove_beatmap_extract(extract_id);
      return std::nullopt;
    }

    return fs::path(extract->extract_path);
  } catch (const std::exception& e) {
    spdlog::error("[EXTRACT] Error getting extract path: {}", e.what());
    return std::nullopt;
  }
}

void BeatmapDownloader::cleanup_expired_extracts() {
  try {
    auto& db = db::Database::instance();
    auto expired = db.cleanup_expired_extracts();

    spdlog::info("[CLEANUP] Cleaning up {} expired extracts", expired.size());

    for (const auto& extract : expired) {
      try {
        if (fs::exists(extract.extract_path)) {
          fs::remove_all(extract.extract_path);
          spdlog::debug("[CLEANUP] Removed extract directory: {}", extract.extract_path);
        }
      } catch (const std::exception& e) {
        spdlog::warn("[CLEANUP] Failed to remove extract {}: {}", extract.extract_id, e.what());
      }
    }

    if (!expired.empty()) {
      spdlog::info("[CLEANUP] Cleaned up {} expired extracts", expired.size());
    }
  } catch (const std::exception& e) {
    spdlog::error("[CLEANUP] Error during extract cleanup: {}", e.what());
  }
}

void BeatmapDownloader::cleanup_missing_files() {
  try {
    auto& db = db::Database::instance();

    spdlog::info("[CLEANUP] Starting cleanup of missing .osz files...");

    // Get all beatmap files from database
    auto all_files = db.get_all_beatmap_files();

    if (all_files.empty()) {
      spdlog::info("[CLEANUP] No beatmap files in database");
      return;
    }

    int total = all_files.size();
    int removed = 0;

    for (const auto& file : all_files) {
      // Check if .osz file exists
      if (!fs::exists(file.osz_path)) {
        spdlog::warn("[CLEANUP] Removing beatmapset {} from database (.osz missing)", file.beatmapset_id);
        db.remove_beatmap_file(file.beatmapset_id);
        removed++;
      }
    }

    if (removed > 0) {
      spdlog::info("[CLEANUP] Removed {} of {} database entries with missing .osz files", removed, total);
    } else {
      spdlog::info("[CLEANUP] All {} database entries have valid .osz files", total);
    }

  } catch (const std::exception& e) {
    spdlog::error("[CLEANUP] Error during cleanup: {}", e.what());
  }
}

// Find audio/background files in extract
std::optional<std::string> BeatmapDownloader::find_audio_in_extract(const fs::path& extract_path) const {
  std::vector<std::string> audio_extensions = {".mp3", ".ogg", ".wav"};
  fs::path best_path;
  uintmax_t best_size = 0;

  for (const auto& entry : fs::recursive_directory_iterator(extract_path)) {
    if (!entry.is_regular_file()) continue;

    auto ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    bool is_audio = std::any_of(audio_extensions.begin(), audio_extensions.end(),
      [&](const std::string& audio_ext) { return ext == audio_ext; });
    if (!is_audio) continue;

    std::error_code ec;
    auto size = fs::file_size(entry.path(), ec);
    if (ec) continue;

    if (size > best_size) {
      best_size = size;
      best_path = entry.path();
    }
  }

  if (best_path.empty()) {
    return std::nullopt;
  }

  // Return filename relative to extract_path
  return fs::relative(best_path, extract_path).string();
}

std::optional<std::string> BeatmapDownloader::find_background_in_extract(const fs::path& extract_path) const {
  // Parse .osu files to find background
  std::regex bg_regex(R"delim(0,0,"([^"]+)",)delim");
  std::string bg_filename;

  for (const auto& entry : fs::directory_iterator(extract_path)) {
    if (entry.path().extension() == ".osu") {
      std::ifstream file(entry.path());
      std::string line;
      bool in_events = false;

      while (std::getline(file, line)) {
        if (line.find("[Events]") != std::string::npos) {
          in_events = true;
          continue;
        }

        if (in_events && line.find('[') != std::string::npos) {
          break; // End of Events section
        }

        if (in_events) {
          std::smatch match;
          if (std::regex_search(line, match, bg_regex)) {
            bg_filename = match[1].str();
            break;
          }
        }
      }

      if (!bg_filename.empty()) {
        break;
      }
    }
  }

  if (!bg_filename.empty()) {
    fs::path bg_path = extract_path / bg_filename;
    if (fs::exists(bg_path)) {
      return bg_filename;
    }
  }

  // Fallback: find any image file
  std::vector<std::string> img_extensions = {".jpg", ".jpeg", ".png"};
  for (const auto& entry : fs::directory_iterator(extract_path)) {
    if (entry.is_regular_file()) {
      auto ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

      for (const auto& img_ext : img_extensions) {
        if (ext == img_ext) {
          return entry.path().filename().string();
        }
      }
    }
  }

  return std::nullopt;
}

// ============================================================================
// Individual .osu file download functions
// ============================================================================

bool BeatmapDownloader::beatmap_osu_exists(uint32_t beatmap_id) const {
  fs::path osu_path = osu_files_dir_ / (std::to_string(beatmap_id) + ".osu");
  return fs::exists(osu_path);
}

std::optional<fs::path> BeatmapDownloader::get_osu_file_path(uint32_t beatmap_id) const {
  fs::path osu_path = osu_files_dir_ / (std::to_string(beatmap_id) + ".osu");
  if (fs::exists(osu_path)) {
    return osu_path;
  }
  return std::nullopt;
}

std::optional<fs::path> BeatmapDownloader::download_osu_file(uint32_t beatmap_id) {
  // Check for shutdown request
  if (is_shutdown_requested()) {
    return std::nullopt;
  }

  // Lock to prevent concurrent downloads of the same beatmap
  std::lock_guard<std::mutex> lock(download_mutex_);

  // Check again after acquiring lock (another thread might have downloaded it)
  if (beatmap_osu_exists(beatmap_id)) {
    spdlog::info("[OSU_FILE] File already exists for beatmap {}", beatmap_id);
    last_used_mirror_ = "cache";
    return get_osu_file_path(beatmap_id);
  }

  fs::path osu_path = osu_files_dir_ / (std::to_string(beatmap_id) + ".osu");
  fs::path temp_path = osu_path;
  temp_path += ".tmp";

  // Download from osu.ppy.sh
  std::string url = "https://osu.ppy.sh/osu/" + std::to_string(beatmap_id);

  spdlog::info("[OSU_FILE] Downloading .osu file from: {}", url);

  std::ofstream output(temp_path, std::ios::binary);
  if (!output) {
    spdlog::error("[OSU_FILE] Failed to open file for writing: {}", temp_path.string());
    return std::nullopt;
  }

  auto response = cpr::Download(output, cpr::Url{url}, cpr::Timeout{15000});
  output.close();

  spdlog::info("[OSU_FILE] HTTP response: status={}, bytes={}",
    response.status_code, response.downloaded_bytes);

  if (response.status_code == 200 && response.downloaded_bytes > 0) {
    // Verify Content-Length if available
    auto content_length_it = response.header.find("Content-Length");
    if (content_length_it != response.header.end()) {
      try {
        int64_t expected_size = std::stoll(content_length_it->second);
        if (response.downloaded_bytes != expected_size) {
          spdlog::warn("[OSU_FILE] Size mismatch: downloaded {} bytes, expected {} bytes",
            response.downloaded_bytes, expected_size);
          fs::remove(temp_path);
          return std::nullopt;
        }
      } catch (const std::exception& e) {
        spdlog::warn("[OSU_FILE] Failed to parse Content-Length: {}", e.what());
      }
    }

    // Move temp file to final destination atomically
    std::error_code ec;
    fs::rename(temp_path, osu_path, ec);
    if (ec) {
      spdlog::error("[OSU_FILE] Failed to rename temp file: {}", ec.message());
      fs::remove(temp_path);
      return std::nullopt;
    }

    spdlog::info("[OSU_FILE] Successfully downloaded and verified {} bytes", response.downloaded_bytes);
    last_used_mirror_ = "osu.ppy.sh";

    // Get file size
    int64_t file_size = std::filesystem::file_size(osu_path);

    // Register in database - but we need beatmapset_id
    // We'll try to get it from cache or it will be set later when we know it
    try {
      auto& db = db::Database::instance();

      // Try to get beatmapset_id from cache
      auto beatmapset_id_opt = db.get_beatmapset_id(beatmap_id);

      if (beatmapset_id_opt) {
        db.register_osu_file(beatmap_id, *beatmapset_id_opt, osu_path.string(), file_size);
      } else {
        // We don't know beatmapset_id yet, will be updated later
        spdlog::debug("[OSU_FILE] Beatmapset ID not in cache for beatmap {}, will register later", beatmap_id);
      }
    } catch (const std::exception& e) {
      spdlog::warn("[OSU_FILE] Failed to register in database: {}", e.what());
    }

    return osu_path;
  }

  // Download failed - clean up temp file
  spdlog::error("[OSU_FILE] Failed to download .osu file for beatmap {}", beatmap_id);
  fs::remove(temp_path);
  return std::nullopt;
}

std::string BeatmapDownloader::build_download_footer(uint32_t beatmapset_id) const {
  try {
    auto& db = db::Database::instance();
    auto file_info = db.get_beatmap_file(beatmapset_id);

    if (file_info && file_info->created_at) {
      std::string time_ago = utils::format_time_ago(*file_info->created_at);
      std::string mirror = file_info->mirror_hostname.value_or("cache");

      if (mirror == "cache") {
        return fmt::format("cached • {}", time_ago);
      }
      return fmt::format("{} • {}", mirror, time_ago);
    }

    std::string mirror = last_used_mirror_;
    return mirror == "cache" ? "cached" : mirror;
  } catch (const std::exception& e) {
    return last_used_mirror_ == "cache" ? "cached" : "downloaded";
  }
}
