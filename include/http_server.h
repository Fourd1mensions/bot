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
#include <vector>

#include <crow.h>

class BeatmapDownloader;

namespace services {
class MessageCrawlerService;
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

  HttpServer(const HttpServer&)            = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&)                 = delete;
  HttpServer& operator=(HttpServer&&)      = delete;

private:
  void run();
  std::string get_client_ip(const crow::request& req) const;

  std::string                      host_;
  uint16_t                         port_;
  std::unique_ptr<crow::SimpleApp> app_;
  std::unique_ptr<std::thread>     server_thread_;
  std::unique_ptr<BeatmapDownloader> downloader_;
  std::unique_ptr<RateLimiter>     download_limiter_;  // Rate limit for downloads
  std::atomic<bool>                running_{false};
  std::atomic<int64_t>             start_time_;

  // Optional crawler service for word stats endpoints
  services::MessageCrawlerService* crawler_service_{nullptr};
};
