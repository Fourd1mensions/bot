#include <requests.h>

#include <string_view>

#include <cpr/cpr.h>
#include <fmt/base.h>
#include <spdlog/spdlog.h>

bool Request::set_token() { 
  spdlog::info("Requesting new token");
  
  const auto payload = cpr::Payload{{"client_id", config.client_id},
      {"client_secret", config.client_secret},
      {"grant_type", "client_credentials"},
      {"scope", "public"}};

  cpr::Response r = cpr::Post(cpr::Url{"https://osu.ppy.sh/oauth/token"},
                              cpr::Header{{"Accept", "application/json"},
                                          {"Content-Type", "application/x-www-form-urlencoded"}},
                              payload);
  if (r.status_code == 200) {
    auto j               = json::parse(r.text);
    config.access_token = j.value("access_token", "");
    size_t now          = utils::get_time();    
    config.expires_at   = now + j.value("expires_in", 86995);
 
    utils::save_config(config);
    spdlog::info("Got ACCESS_TOKEN!");
    return true;
  }
  spdlog::error("Failed to retrieve ACCESS_TOKEN ({}): {}", r.status_code, r.text);
  return false;
}

bool Request::update_token() {
  if (utils::get_time() > config.expires_at) 
    return set_token();

  return true;
}

std::string Request::get_userid_v1(const std::string_view username) {
  cpr::Response r      = cpr::Get(cpr::Url{"http://osu.ppy.sh/api/get_user"},
                                  cpr::Parameters{{"k", config.api_v1_key}, {"u", username.data()}});
  const size_t        pos    = r.text.find_first_of("0123456789");
  const size_t        endpos = r.text.find_first_not_of("0123456789", pos);
  return std::string(r.text.substr(pos, endpos - pos));
}

// user = username by default
std::string Request::get_user(const std::string_view user, const bool by_id) {
   if (!update_token()) {
    spdlog::error("Can't send requests, token is dead");
    return "";
  }
  
  cpr::Response r = cpr::Get(
      cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/users/{}{}/osu", by_id ? "" : "@", user)},
      cpr::Header{{"Authorization", "Bearer " + config.access_token},
                  {"Content-Type", "application/json"},
                  {"Accept", "application/json"}});
  if (r.status_code == 200) {
    spdlog::info("get_user success");
    return r.text;
  }
  spdlog::info("get_user failed, status: {}", r.status_code);
  return "";
}
// if all=false returns single score that peppy wants, else - all user scores on map
std::string Request::get_user_beatmap_score(const std::string_view beatmap,
                                            const std::string_view user, const bool all) {
  if (!update_token()) {
    spdlog::error("Can't send requests, token is dead");
    return "";
  }

  cpr::Response r =
      cpr::Get(cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}/scores/users/{}{}",
                                    beatmap, user, all ? "/all" : "")},
               cpr::Header{{"Authorization", "Bearer " + config.access_token},
                           {"Content-Type", "application/json"},
                           {"Accept", "application/json"}});

  const auto status_code = r.status_code;
  switch (status_code) {
    case 200:
      spdlog::info("status {} for {} on {}", status_code, user, beatmap);
      if (r.text != "{\"scores\":[]}")
        return r.text;
      else {
        fmt::print("...but no scores found\n");
        return {};
      }
    case 404: spdlog::info("status {}, no scores found for {}", status_code, user); break;
    default: spdlog::warn("status: {}, get_user_beatmap_score failed", status_code); break;
  }
  return {};
}

std::string Request::get_beatmap(const std::string_view beatmap) {
  if (!update_token()) {
    spdlog::error("Can't send requests, token is dead");
    return "";
  }
  cpr::Response r =
      cpr::Get(cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}", beatmap)},
               cpr::Header{{"Authorization", "Bearer " + config.access_token},
                           {"Content-Type", "application/json"},
                           {"Accept", "application/json"}});
  if (r.status_code == 200) {
    return r.text;
  }
  spdlog::info("get_beatmap failed, status: {}", r.status_code);
  return "";
}

Request::Request() {
  utils::load_config(config);
  update_token();
}
