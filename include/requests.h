#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

struct Tokens {
  std::string api_v1_key, client_id, client_secret, auth_code, access_token,
      refresh_token;
  size_t expires_in;
};

class Request {
private:
  // if refresh token doesn't work use authorization code
  // https://osu.ppy.sh/oauth/authorize?response_type=code&client_id=34987&redirect_uri=https://bot.xrcsm.dev/auth/osu&scope=public

  Tokens tokens;

  bool save_tokens();

public:
  static std::string read_config(const std::string_view key);

  bool set_tokens(const std::string_view
                      grant_type); // "authorization_code"/"refresh_token"
  bool is_refresh_needed = false;

  std::string get_user(const std::string_view username);
  std::string get_user_score(const std::string_view beatmap,
                             const std::string_view user);
  std::string get_userid_v1(const std::string_view username);
  std::string get_beatmap(const std::string_view beatmap);

  Request() { // FIXME: from_file_tokens
    tokens.api_v1_key = read_config("API_V1_KEY");
    tokens.client_id = read_config("CLIENT_ID");
    tokens.client_secret = read_config("CLIENT_SECRET");
    tokens.auth_code = read_config("AUTH_CODE");
    tokens.access_token = read_config("ACCESS_TOKEN");
    tokens.refresh_token = read_config("REFRESH_TOKEN");

    std::string test = get_user("peppy");
    if (!test.empty()) {
      spdlog::info("Test request is success\n");
      return;
    }
    spdlog::warn("Test request failed, trying to refresh token\n");
    if (set_tokens("refresh_token")) {
      spdlog::info("Refresh token is success\n");
      return;
    }
    spdlog::warn("Refresh token is failed, trying to use auth code\n");
    if (set_tokens("authorization_code")) {
      spdlog::info("Refresh token by code is success\n");
      return;
    }
    spdlog::error(
        "Refresh token by code is failed\nPlease update authentification code");
    is_refresh_needed = true;
  }
};
