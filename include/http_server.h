#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include <crow.h>

class BeatmapDownloader;

class HttpServer {
public:
  explicit HttpServer(const std::string& host = "127.0.0.1", uint16_t port = 8080,
                      const std::vector<std::string>& mirrors = {});
  ~HttpServer();

  void start();
  void stop();

  HttpServer(const HttpServer&)            = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&)                 = delete;
  HttpServer& operator=(HttpServer&&)      = delete;

private:
  void run();

  std::string                      host_;
  uint16_t                         port_;
  std::unique_ptr<crow::SimpleApp> app_;
  std::unique_ptr<std::thread>     server_thread_;
  std::unique_ptr<BeatmapDownloader> downloader_;
  std::atomic<bool>                running_{false};
  std::atomic<int64_t>             start_time_;
};
