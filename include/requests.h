#pragma once

#include <utils.h>

#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

class Request {
private:
  // if refresh token doesn't work use authorization code
  // https://osu.ppy.sh/oauth/authorize?response_type=code&client_id=34987&redirect_uri=https://bot.xrcsm.dev/auth/osu&scope=public
  
  Config config;
  bool is_refresh_needed = false;

  public:
  bool set_tokens(); 
  bool check_token();

  std::string get_user(const std::string_view username, const bool by_id = false) const;
  std::string get_user_beatmap_score(const std::string_view beatmap,
                                     const std::string_view user,
                                     const bool all = false) const;
  std::string get_userid_v1(const std::string_view username);
  std::string get_beatmap(const std::string_view beatmap) const;

  Request();
};
