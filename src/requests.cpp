#include <requests.h>

#include <string_view>
#include <thread>
#include <chrono>

#include <cpr/cpr.h>
#include <fmt/base.h>
#include <spdlog/spdlog.h>

bool Request::set_tokens(const std::string_view grant_type) {
  cpr::Payload payload{};
  spdlog::info("Requesting new {}", grant_type);
  if (grant_type == "authorization_code") {
    payload = cpr::Payload{{"client_id", config.client_id},
                           {"client_secret", config.client_secret},
                           {"grant_type", grant_type.data()},
                           {"code", config.auth_code},
                           {"redirect_uri", config.redirect_uri}};
  } else {
    payload = cpr::Payload{{"client_id", config.client_id},
                           {"client_secret", config.client_secret},
                           {"grant_type", grant_type.data()},
                           {"refresh_token", config.refresh_token},
                           {"redirect_uri", config.redirect_uri}};
  }

  cpr::Response r = cpr::Post(cpr::Url{"https://osu.ppy.sh/oauth/token"},
                              cpr::Header{{"Accept", "application/json"},
                                          {"Content-Type", "application/x-www-form-urlencoded"}},
                              payload);
  if (r.status_code == 200) {
    auto j               = json::parse(r.text);
    config.access_token  = j.value("access_token", "");
    config.refresh_token = j.value("refresh_token", "");
    config.expires_in    = j.value("expires_in", 86399);
    utils::save_config(config);
    spdlog::info("Got ACCESS_TOKEN!");
    return true;
  }
  spdlog::error("Failed to retrieve ACCESS_TOKEN ({}): {}", r.status_code, r.text);
  return false;
}

bool Request::check_token() {
  std::string test = get_user("peppy");
  if (!test.empty()) {
    spdlog::info("Test request is success");
    return true;
  }
  spdlog::warn("Test request failed, trying to refresh token...");
  if (set_tokens("refresh_token")) {
    spdlog::info("Refresh token is success");
    return true;
  }
  spdlog::warn("Refresh token is failed, trying to use auth code...");
  if (set_tokens("authorization_code")) {
    spdlog::info("Refresh token by code is success");
    return true;
  }
  spdlog::error(
      "Refresh token by code is failed. Please update authentification code and restart bot");
  is_refresh_needed = true;
  return false;
}

std::string Request::get_userid_v1(const std::string_view username) {
  cpr::Response r      = cpr::Get(cpr::Url{"http://osu.ppy.sh/api/get_user"},
                                  cpr::Parameters{{"k", config.api_v1_key}, {"u", username.data()}});
  size_t        pos    = r.text.find_first_of("0123456789");
  size_t        endpos = r.text.find_first_not_of("0123456789", pos);
  return std::string(r.text.substr(pos, endpos - pos));
}

// user = username by default
std::string Request::get_user(const std::string_view user, const bool by_id) const {
  if (is_refresh_needed) {
    spdlog::error("get_user failed, token is dead");
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
                                            const std::string_view user, const bool all) const {
  if (is_refresh_needed) {
    spdlog::error("get_user_score failed, token is dead");
    return {};
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

std::string Request::get_beatmap(const std::string_view beatmap) const {
  if (is_refresh_needed) {
    spdlog::error("get_beatmap failed, token is dead");
    return "";
  }
  cpr::Response r =
      cpr::Get(cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}", beatmap)},
               cpr::Header{{"Authorization", "Bearer " + config.access_token},
                           {"Content-Type", "application/json"},
                           {"Accept", "application/json"}});
  if (r.status_code == 200) {
    spdlog::info("get_beatmap success");
    return r.text;
  }
  spdlog::info("get_beatmap failed, status: {}", r.status_code);
  return "";
}

Request::Request() {
  // TODO: split this code to functions, check config.json is opened
  config.api_v1_key     = utils::read_field("API_V1_KEY", "config.json");
  config.client_id      = utils::read_field("CLIENT_ID", "config.json");
  config.client_secret  = utils::read_field("CLIENT_SECRET", "config.json");
  config.auth_code      = utils::read_field("AUTH_CODE", "config.json");
  config.access_token   = utils::read_field("ACCESS_TOKEN", "config.json");
  config.refresh_token  = utils::read_field("REFRESH_TOKEN", "config.json");
  config.redirect_uri   = utils::read_field("REDIRECT_URI", "config.json");
  // TODO: new token update alg uses expires_in
  std::jthread([&]() {
    while(true) {
      if(!check_token()) break;
      std::this_thread::sleep_for(std::chrono::seconds(300));
    }
  }).detach();

}
