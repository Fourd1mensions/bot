#pragma once

#include <beatmap_downloader.h>
#include <requests.h>
#include <dpp/dpp.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>

namespace services {

/**
 * Service for proactively caching beatmaps mentioned in chat.
 * Runs background downloads and reports failures to a designated channel.
 */
class BeatmapCacheService {
public:
  explicit BeatmapCacheService(BeatmapDownloader& downloader, Request& request, dpp::cluster& bot);
  ~BeatmapCacheService();

  /**
   * Queue a beatmapset for background download.
   * Thread-safe, deduplicates requests.
   */
  void queue_download(uint32_t beatmapset_id);

  /**
   * Queue a beatmap for background download (resolves beatmapset_id via API).
   * Thread-safe, deduplicates requests.
   */
  void queue_download_by_beatmap_id(uint32_t beatmap_id);

  /**
   * Set the Discord channel ID for error reports.
   */
  void set_error_channel(dpp::snowflake channel_id);

  /**
   * Start the background worker thread.
   */
  void start();

  /**
   * Stop the background worker thread.
   */
  void stop();

  /**
   * Get statistics
   */
  size_t get_queue_size() const;
  size_t get_total_downloaded() const { return total_downloaded_.load(); }
  size_t get_total_failed() const { return total_failed_.load(); }

  /**
   * Get downloads in time period
   */
  size_t get_downloads_last_hour() const;
  size_t get_downloads_last_24h() const;

  /**
   * Get formatted status string for Discord presence
   */
  std::string get_status_string() const;

private:
  void worker_thread();
  void process_download(uint32_t beatmapset_id);
  void send_error_report(uint32_t beatmapset_id,
                         const std::vector<::MirrorAttemptResult>& attempts,
                         std::chrono::milliseconds total_time);

  BeatmapDownloader& downloader_;
  Request& request_;
  dpp::cluster& bot_;
  dpp::snowflake error_channel_id_{0};

  std::queue<uint32_t> download_queue_;
  std::unordered_set<uint32_t> queued_ids_;  // For deduplication
  std::unordered_set<uint32_t> recently_failed_;  // Don't retry immediately
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;

  std::thread worker_;
  std::atomic<bool> running_{false};

  std::atomic<size_t> total_downloaded_{0};
  std::atomic<size_t> total_failed_{0};

  // Track download timestamps for time-based stats (fallback if DB unavailable)
  mutable std::mutex stats_mutex_;
  mutable std::deque<std::chrono::steady_clock::time_point> download_timestamps_;
  void record_download(uint32_t beatmapset_id);
  void cleanup_old_timestamps() const;
};

} // namespace services
