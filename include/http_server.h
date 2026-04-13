#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <crow.h>

struct Config;
class BeatmapDownloader;

namespace services {
class MessageCrawlerService;
class UserSettingsService;
class EmbedTemplateService;
class MusicPlayerService;
}

/**
 * Simple IP-based rate limiter using sliding window
 */
class RateLimiter {
public:
  explicit RateLimiter(size_t max_requests, std::chrono::seconds window)
      : max_requests_(max_requests), window_(window) {}

  /**
   * Check if request is allowed and record it if so
   * @return true if allowed, false if rate limited
   */
  bool allow(const std::string& client_id);

  /**
   * Get remaining requests for a client
   */
  size_t remaining(const std::string& client_id);

private:
  void cleanup_old_requests(const std::string& client_id);

  size_t max_requests_;
  std::chrono::seconds window_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> requests_;
};

class HttpServer {
public:
  explicit HttpServer(const std::string& host = "127.0.0.1", uint16_t port = 8080,
                      const std::vector<std::string>& mirrors = {});
  ~HttpServer();

  void start();
  void stop();

  // Set crawler service for stats endpoints
  void set_crawler_service(services::MessageCrawlerService* service);

  // Set user settings service for presets endpoint
  void set_user_settings_service(services::UserSettingsService* service);

  // Set embed template service for template editor
  void set_template_service(services::EmbedTemplateService* service);

  // Set music player service for music API endpoints
  void set_music_service(services::MusicPlayerService* service);

  // Set config for OAuth2 and admin checks
  void set_config(const Config* config);

  HttpServer(const HttpServer&)            = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&)                 = delete;
  HttpServer& operator=(HttpServer&&)      = delete;

private:
  void run();
  std::string get_client_ip(const crow::request& req) const;

  // WebSocket music state push
  struct WsConnectionData {
    uint64_t guild_id = 0;
    std::string discord_id;
  };
  void broadcast_music_state(uint64_t guild_id);
  std::string build_state_json(uint64_t guild_id);
  std::mutex ws_mutex_;
  std::unordered_map<uint64_t, std::unordered_set<crow::websocket::connection*>> ws_guild_connections_;

  std::string                      host_;
  uint16_t                         port_;
  std::unique_ptr<crow::SimpleApp> app_;
  std::unique_ptr<std::thread>     server_thread_;
  std::unique_ptr<BeatmapDownloader> downloader_;
  std::unique_ptr<RateLimiter>     download_limiter_;  // Rate limit for downloads
  std::unique_ptr<RateLimiter>     template_save_limiter_;  // Rate limit for custom template saves
  std::unique_ptr<RateLimiter>     music_search_limiter_;   // Rate limit for music search (yt-dlp fork)
  std::unique_ptr<RateLimiter>     music_play_limiter_;     // Rate limit for music play (yt-dlp fork)
  std::atomic<bool>                running_{false};
  std::atomic<int64_t>             start_time_;

  // Optional crawler service for word stats endpoints
  services::MessageCrawlerService* crawler_service_{nullptr};

  // Optional user settings service for presets management
  services::UserSettingsService* user_settings_service_{nullptr};

  // Optional embed template service for template editing
  services::EmbedTemplateService* template_service_{nullptr};

  // Music player service for voice channel audio playback
  services::MusicPlayerService* music_service_{nullptr};

  // Config for OAuth2 credentials and admin users
  const Config* config_{nullptr};
};
