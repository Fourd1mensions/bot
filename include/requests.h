#pragma once

#include <utils.h>

#include <string>
#include <cpr/cpr.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

class Request {
private:
  // if refresh token doesn't work use authorization code
  // https://osu.ppy.sh/oauth/authorize?response_type=code&client_id=34987&redirect_uri=https://bot.xrcsm.dev/auth/osu&scope=public

  Config config;
  bool set_token();

  // Helper to execute API requests with automatic 401 retry
  template<typename Func>
  cpr::Response execute_with_retry(Func&& request_func) {
    cpr::Response r = request_func();

    // Handle 401 Unauthorized - token might have expired
    if (r.status_code == 401) {
      spdlog::warn("[API] Got 401, refreshing token and retrying");
      config.expires_at = 0; // Force token refresh
      if (set_token()) {
        // Retry the request with new token
        r = request_func();
      }
    }

    return r;
  }

  public:
  bool update_token();
  std::string get_user(const std::string_view username, const bool by_id = false);
  std::string get_user_beatmap_score(const std::string_view beatmap,
                                     const std::string_view user,
                                     const bool all = false);
  std::string get_userid_v1(const std::string_view username);
  std::string get_beatmap(const std::string_view beatmap);
  std::string get_beatmap_id_from_set(const std::string_view beatmapset_id);
  std::string get_beatmap_attributes(const std::string_view beatmap, uint32_t mods_bitset);
  std::string get_user_recent_scores(const std::string_view user_id,
                                     bool include_fails = false,
                                     const std::string& mode = "osu",
                                     int limit = 50,
                                     int offset = 0);
  std::string get_user_best_scores(const std::string_view user_id,
                                   const std::string& mode = "osu",
                                   int limit = 100,
                                   int offset = 0);
  std::string get_weather(const std::string_view city);
  Request();
};
