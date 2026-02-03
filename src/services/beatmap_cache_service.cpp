#include "services/beatmap_cache_service.h"
#include <database.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <filesystem>

using json = nlohmann::json;

namespace services {

BeatmapCacheService::BeatmapCacheService(BeatmapDownloader& downloader, Request& request, dpp::cluster& bot)
    : downloader_(downloader), request_(request), bot_(bot) {}

BeatmapCacheService::~BeatmapCacheService() {
  stop();
}

void BeatmapCacheService::set_error_channel(dpp::snowflake channel_id) {
  error_channel_id_ = channel_id;
}

void BeatmapCacheService::queue_download(uint32_t beatmapset_id) {
  // Skip if already cached
  if (downloader_.beatmapset_exists(beatmapset_id)) {
    spdlog::info("[CACHE] Beatmapset {} already cached, skipping", beatmapset_id);
    return;
  }

  std::lock_guard<std::mutex> lock(queue_mutex_);

  // Skip if already queued or recently failed
  if (queued_ids_.count(beatmapset_id) > 0) {
    spdlog::info("[CACHE] Beatmapset {} already in queue, skipping", beatmapset_id);
    return;
  }
  if (recently_failed_.count(beatmapset_id) > 0) {
    spdlog::info("[CACHE] Beatmapset {} recently failed, skipping", beatmapset_id);
    return;
  }

  download_queue_.push(beatmapset_id);
  queued_ids_.insert(beatmapset_id);

  spdlog::info("[CACHE] Queued beatmapset {} for background download (queue size: {})",
    beatmapset_id, download_queue_.size());

  queue_cv_.notify_one();
}

void BeatmapCacheService::queue_download_by_beatmap_id(uint32_t beatmap_id) {
  // First check database cache
  try {
    auto& db = db::Database::instance();
    auto beatmapset_opt = db.get_beatmapset_id(beatmap_id);
    if (beatmapset_opt) {
      spdlog::info("[CACHE] Resolved beatmap {} -> beatmapset {} from DB", beatmap_id, *beatmapset_opt);
      queue_download(static_cast<uint32_t>(*beatmapset_opt));
      return;
    }
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] DB lookup failed for beatmap {}: {}", beatmap_id, e.what());
  }

  // Not in DB, resolve via API in background
  std::thread([this, beatmap_id]() {
    spdlog::info("[CACHE] Resolving beatmap {} via API", beatmap_id);

    std::string response = request_.get_beatmap(std::to_string(beatmap_id));
    if (response.empty()) {
      spdlog::warn("[CACHE] API returned empty for beatmap {}", beatmap_id);
      return;
    }

    try {
      json j = json::parse(response);
      if (j.contains("beatmapset_id")) {
        uint32_t beatmapset_id = j["beatmapset_id"].get<uint32_t>();
        spdlog::info("[CACHE] Resolved beatmap {} -> beatmapset {} from API", beatmap_id, beatmapset_id);

        // Cache the mapping in database
        try {
          auto& db = db::Database::instance();
          db.cache_beatmap_id(beatmapset_id, beatmap_id);
        } catch (...) {}

        queue_download(beatmapset_id);
      }
    } catch (const json::exception& e) {
      spdlog::warn("[CACHE] Failed to parse API response for beatmap {}: {}", beatmap_id, e.what());
    }
  }).detach();
}

void BeatmapCacheService::start() {
  if (running_.exchange(true)) {
    return;
  }

  worker_ = std::thread([this] { worker_thread(); });
  spdlog::info("[CACHE] Background cache service started");
}

void BeatmapCacheService::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  queue_cv_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }

  spdlog::info("[CACHE] Background cache service stopped");
}

size_t BeatmapCacheService::get_queue_size() const {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return download_queue_.size();
}

void BeatmapCacheService::worker_thread() {
  while (running_) {
    uint32_t beatmapset_id = 0;

    {
      std::unique_lock<std::mutex> lock(queue_mutex_);

      queue_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
        return !download_queue_.empty() || !running_;
      });

      if (!running_) break;

      if (download_queue_.empty()) continue;

      beatmapset_id = download_queue_.front();
      download_queue_.pop();
      queued_ids_.erase(beatmapset_id);
    }

    if (beatmapset_id > 0) {
      process_download(beatmapset_id);
    }
  }
}

void BeatmapCacheService::process_download(uint32_t beatmapset_id) {
  spdlog::info("[CACHE] Processing background download for beatmapset {}", beatmapset_id);

  auto start_time = std::chrono::steady_clock::now();
  std::vector<MirrorAttemptResult> attempts;

  bool success = downloader_.download_osz_with_attempts(beatmapset_id, attempts);

  auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start_time);

  if (success) {
    total_downloaded_++;
    record_download(beatmapset_id);
    spdlog::info("[CACHE] Successfully cached beatmapset {} in {}ms",
      beatmapset_id, total_time.count());
  } else {
    total_failed_++;

    // Add to recently_failed to avoid immediate retry
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      recently_failed_.insert(beatmapset_id);
    }

    // Schedule removal from recently_failed after 5 minutes
    std::thread([this, beatmapset_id]() {
      std::this_thread::sleep_for(std::chrono::minutes(5));
      std::lock_guard<std::mutex> lock(queue_mutex_);
      recently_failed_.erase(beatmapset_id);
    }).detach();

    spdlog::warn("[CACHE] Failed to cache beatmapset {} after {} attempts in {}ms",
      beatmapset_id, attempts.size(), total_time.count());

    // Send error report to Discord
    send_error_report(beatmapset_id, attempts, total_time);
  }
}

void BeatmapCacheService::send_error_report(uint32_t beatmapset_id,
                                             const std::vector<::MirrorAttemptResult>& attempts,
                                             std::chrono::milliseconds total_time) {
  if (error_channel_id_ == 0) {
    spdlog::warn("[CACHE] Error channel not set, skipping error report");
    return;
  }

  dpp::embed embed;
  embed.set_title("Beatmap Download Failed");
  embed.set_color(0xFF4444);
  embed.set_url(fmt::format("https://osu.ppy.sh/beatmapsets/{}", beatmapset_id));

  embed.add_field("Beatmapset ID", std::to_string(beatmapset_id), true);
  embed.add_field("Total Time", fmt::format("{}ms", total_time.count()), true);
  embed.add_field("Attempts", std::to_string(attempts.size()), true);

  // Build attempts details
  std::string attempts_detail;
  for (size_t i = 0; i < attempts.size(); i++) {
    const auto& attempt = attempts[i];
    attempts_detail += fmt::format("**{}. {}**\n", i + 1, attempt.mirror_url);
    attempts_detail += fmt::format("  Status: {} | Time: {}ms\n",
      attempt.status_code, attempt.duration.count());
    if (!attempt.error_message.empty()) {
      attempts_detail += fmt::format("  Error: {}\n", attempt.error_message);
    }
    if (attempt.bytes_downloaded > 0) {
      attempts_detail += fmt::format("  Bytes: {}\n", attempt.bytes_downloaded);
    }
    attempts_detail += "\n";
  }

  if (attempts_detail.length() > 1024) {
    attempts_detail = attempts_detail.substr(0, 1020) + "...";
  }

  embed.add_field("Attempt Details", attempts_detail, false);

  embed.set_timestamp(time(nullptr));

  dpp::message msg;
  msg.set_channel_id(error_channel_id_);
  msg.add_embed(embed);

  bot_.message_create(msg, [beatmapset_id](const dpp::confirmation_callback_t& callback) {
    if (callback.is_error()) {
      spdlog::error("[CACHE] Failed to send error report for beatmapset {}: {}",
        beatmapset_id, callback.get_error().message);
    }
  });
}

void BeatmapCacheService::record_download(uint32_t beatmapset_id) {
  try {
    db::Database::instance().log_download(beatmapset_id);
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] Failed to log download: {}", e.what());
    // Fallback to in-memory
    std::lock_guard<std::mutex> lock(stats_mutex_);
    download_timestamps_.push_back(std::chrono::steady_clock::now());
  }
}

void BeatmapCacheService::cleanup_old_timestamps() const {
  // Cleanup in-memory fallback
  auto now = std::chrono::steady_clock::now();
  auto cutoff_24h = now - std::chrono::hours(24);
  while (!download_timestamps_.empty() && download_timestamps_.front() < cutoff_24h) {
    const_cast<std::deque<std::chrono::steady_clock::time_point>&>(download_timestamps_).pop_front();
  }

  // Also cleanup database periodically
  static auto last_db_cleanup = std::chrono::steady_clock::now();
  if (now - last_db_cleanup > std::chrono::hours(1)) {
    try {
      db::Database::instance().cleanup_old_downloads();
      last_db_cleanup = now;
    } catch (...) {}
  }
}

size_t BeatmapCacheService::get_downloads_last_hour() const {
  try {
    return db::Database::instance().get_downloads_since(std::chrono::hours(1));
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] Failed to get hourly stats from DB: {}", e.what());
    // Fallback to in-memory
    std::lock_guard<std::mutex> lock(stats_mutex_);
    cleanup_old_timestamps();
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::hours(1);
    size_t count = 0;
    for (const auto& ts : download_timestamps_) {
      if (ts >= cutoff) count++;
    }
    return count;
  }
}

size_t BeatmapCacheService::get_downloads_last_24h() const {
  try {
    return db::Database::instance().get_downloads_since(std::chrono::hours(24));
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] Failed to get daily stats from DB: {}", e.what());
    // Fallback to in-memory
    std::lock_guard<std::mutex> lock(stats_mutex_);
    cleanup_old_timestamps();
    return download_timestamps_.size();
  }
}

std::string BeatmapCacheService::get_status_string() const {
  size_t hour = get_downloads_last_hour();
  size_t day = get_downloads_last_24h();

  // Get total from database (actual cached files count)
  size_t total = 0;
  try {
    auto& db = db::Database::instance();
    total = db.count_beatmap_files();
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] Failed to get beatmap files count: {}", e.what());
    total = total_downloaded_.load();  // Fallback to session count
  }

  // Get disk usage info
  std::string disk_info;
  try {
    std::filesystem::path osz_dir = ".data/osz";
    if (std::filesystem::exists(osz_dir)) {
      // Calculate total size of osz directory
      uintmax_t used_bytes = 0;
      for (const auto& entry : std::filesystem::directory_iterator(osz_dir)) {
        if (entry.is_regular_file()) {
          used_bytes += entry.file_size();
        }
      }

      // Get free space on the filesystem
      auto space_info = std::filesystem::space(osz_dir);
      double used_gb = static_cast<double>(used_bytes) / (1024 * 1024 * 1024);
      double free_gb = static_cast<double>(space_info.available) / (1024 * 1024 * 1024);

      disk_info = fmt::format(" | {:.1f}G/{:.1f}G", used_gb, free_gb);
    }
  } catch (const std::exception& e) {
    spdlog::warn("[CACHE] Failed to get disk info: {}", e.what());
  }

  return fmt::format("{}h/{}d/{}t{}", hour, day, total, disk_info);
}

} // namespace services
