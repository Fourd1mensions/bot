#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <mutex>

namespace fs = std::filesystem;

namespace services {
class WebhookService;
}

/**
 * Result of a single download attempt to a mirror
 */
struct MirrorAttemptResult {
  std::string mirror_url;
  int status_code = 0;
  std::string error_message;
  std::chrono::milliseconds duration{0};
  int64_t bytes_downloaded = 0;
};

class BeatmapDownloader {
public:
  BeatmapDownloader();
  BeatmapDownloader(const std::vector<std::string>& mirrors);

  // Downloads .osz file for a given beatmapset_id
  // Returns true if successful, false otherwise
  bool download_osz(uint32_t beatmapset_id);

  /**
   * Download with detailed attempt information for error reporting.
   * @param beatmapset_id The beatmapset to download
   * @param attempts Output vector of attempt results (filled even on success)
   * @return true if download succeeded
   */
  bool download_osz_with_attempts(uint32_t beatmapset_id, std::vector<MirrorAttemptResult>& attempts);

  // Downloads individual .osu file for a given beatmap_id
  // Returns path to downloaded file if successful, std::nullopt otherwise
  std::optional<fs::path> download_osu_file(uint32_t beatmap_id);

  // Create temporary extract and return extract_id
  // Extract lives for 24 hours by default
  std::optional<std::string> create_extract(uint32_t beatmapset_id);

  // Get path to extracted beatmapset directory
  std::optional<fs::path> get_extract_path(const std::string& extract_id);

  // Get path to .osz file
  std::optional<fs::path> get_osz_path(uint32_t beatmapset_id) const;

  // Set mirror URLs (in priority order)
  void set_mirrors(const std::vector<std::string>& mirrors);

  // Check if beatmapset .osz exists
  bool beatmapset_exists(uint32_t beatmapset_id) const;

  // Check if individual .osu file exists in cache
  bool beatmap_osu_exists(uint32_t beatmap_id) const;

  // Get path to cached .osu file
  std::optional<fs::path> get_osu_file_path(uint32_t beatmap_id) const;

  // Get the URL of the mirror used for the last successful download (or "cache" if already present)
  std::string get_last_used_mirror() const { return last_used_mirror_; }

  // Get full download URL for a beatmapset from the first available mirror
  std::string get_mirror_url(uint32_t beatmapset_id) const;

  // Validate and clean up database entries where files don't exist on disk
  void cleanup_missing_files();

  // Clean up expired extracts from database and filesystem
  void cleanup_expired_extracts();

  // Find audio/background files in an extract
  std::optional<std::string> find_audio_in_extract(const fs::path& extract_path) const;
  std::optional<std::string> find_background_in_extract(const fs::path& extract_path) const;

  // Build footer text with mirror info and cache time
  std::string build_download_footer(uint32_t beatmapset_id) const;

  // Check if mirrors are in cooldown (all mirrors failed recently)
  bool are_mirrors_in_cooldown() const;

  // Reset mirror cooldown (call after a successful download or when cooldown expires)
  void reset_mirror_cooldown();

  // Set webhook service for notifications
  void set_webhook_service(services::WebhookService* service);

  // Request shutdown - abort ongoing downloads
  void request_shutdown();

  // Check if shutdown was requested
  bool is_shutdown_requested() const;

private:
  fs::path data_dir_;
  fs::path osz_dir_;
  fs::path osu_files_dir_;
  fs::path extracts_dir_;
  std::vector<std::string> mirrors_;
  std::string last_used_mirror_;
  std::mutex download_mutex_;  // Protects concurrent downloads

  // Mirror cooldown tracking
  mutable std::mutex cooldown_mutex_;
  std::chrono::steady_clock::time_point mirror_cooldown_until_;
  int consecutive_all_mirrors_failed_ = 0;
  static constexpr int kMaxConsecutiveFailures = 3;  // Enter cooldown after 3 failures
  static constexpr std::chrono::seconds kCooldownDuration{120};  // 2 minute cooldown
  bool cooldown_webhook_sent_ = false;  // Ensure webhook sent only once per cooldown

  // Webhook service for notifications
  services::WebhookService* webhook_service_ = nullptr;
  void send_cooldown_notification();

  // Shutdown flag
  std::atomic<bool> shutdown_requested_{false};

  // Helper functions
  bool download_osz_with_fallback(uint32_t beatmapset_id, const fs::path& dest_path);
  bool try_download_from_mirror(const std::string& mirror_url, uint32_t beatmapset_id,
                                 const fs::path& dest_path);
  void demote_mirror(size_t index);  // Move failed mirror to end of list
  bool extract_osz(const fs::path& osz_path, const fs::path& extract_dir);

  void ensure_directories();
};
