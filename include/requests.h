#pragma once

#include <string>
#include <thread>
#include <chrono>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

struct Config {
  std::string api_v1_key, client_id, client_secret, auth_code, access_token,
      refresh_token, redirect_uri;
  size_t expires_in;
};

class Request {
private:
  // if refresh token doesn't work use authorization code
  // https://osu.ppy.sh/oauth/authorize?response_type=code&client_id=34987&redirect_uri=https://bot.xrcsm.dev/auth/osu&scope=public
  Config config;
  bool is_refresh_needed = false;

  bool save_config();
public:
  static std::string read_config(const std::string_view key);
  // "authorization_code" or "refresh_token"
  bool set_tokens(const std::string_view grant_type); 
  bool check_token();

  std::string get_user(const std::string_view username) const;
  std::string get_user_score(const std::string_view beatmap,
                             const std::string_view user) const;
  auto get_user_scores(const std::string_view beatmap,
                       const std::string_view user) const;
  std::string get_userid_v1(const std::string_view username);
  std::string get_beatmap(const std::string_view beatmap) const;

  Request() { 
    config.api_v1_key = read_config("API_V1_KEY");
    config.client_id = read_config("CLIENT_ID");
    config.client_secret = read_config("CLIENT_SECRET");
    config.auth_code = read_config("AUTH_CODE");
    config.access_token = read_config("ACCESS_TOKEN");
    config.refresh_token = read_config("REFRESH_TOKEN");
    config.redirect_uri = read_config("REDIRECT_URI");

    std::jthread([&]() {
      while(true) {
        if(!check_token()) break;
        std::this_thread::sleep_for(std::chrono::seconds(300));
      }
    }).detach();

  }
};
