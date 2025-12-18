#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <mutex>

namespace fs = std::filesystem;

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

private:
  fs::path data_dir_;
  fs::path osz_dir_;
  fs::path osu_files_dir_;
  fs::path extracts_dir_;
  std::vector<std::string> mirrors_;
  std::string last_used_mirror_;
  std::mutex download_mutex_;  // Protects concurrent downloads

  // Helper functions
  bool download_osz_with_fallback(uint32_t beatmapset_id, const fs::path& dest_path);
  bool try_download_from_mirror(const std::string& mirror_url, uint32_t beatmapset_id,
                                 const fs::path& dest_path);
  void demote_mirror(size_t index);  // Move failed mirror to end of list
  bool extract_osz(const fs::path& osz_path, const fs::path& extract_dir);

  void ensure_directories();
};
