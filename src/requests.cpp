#include <requests.h>

#include <chrono>
#include <string_view>

#include <cpr/cpr.h>
#include <fmt/base.h>
#include <spdlog/spdlog.h>

bool Request::set_token() {
  spdlog::info("[API] Requesting new OAuth token");
  auto start = std::chrono::steady_clock::now();

  const auto payload = cpr::Payload{{"client_id", config.client_id},
      {"client_secret", config.client_secret},
      {"grant_type", "client_credentials"},
      {"scope", "public"}};

  cpr::Response r = cpr::Post(cpr::Url{"https://osu.ppy.sh/oauth/token"},
                              cpr::Header{{"Accept", "application/json"},
                                          {"Content-Type", "application/x-www-form-urlencoded"}},
                              payload);

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    auto j               = json::parse(r.text);
    config.access_token = j.value("access_token", "");
    size_t now          = utils::get_time();
    config.expires_at   = now + j.value("expires_in", 86995);

    utils::save_config(config);
    spdlog::info("[API] OAuth token acquired successfully ({}ms)", duration);
    return true;
  }
  spdlog::error("[API] Failed to retrieve OAuth token status={} duration={}ms error={}",
    r.status_code, duration, r.text);
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
    spdlog::error("[API] Can't send requests, token is dead");
    return "";
  }

  auto start = std::chrono::steady_clock::now();
  cpr::Response r = cpr::Get(
      cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/users/{}{}/osu", by_id ? "" : "@", user)},
      cpr::Header{{"Authorization", "Bearer " + config.access_token},
                  {"Content-Type", "application/json"},
                  {"Accept", "application/json"}});

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    spdlog::info("[API] get_user success user={} duration={}ms", user, duration);
    return r.text;
  }
  spdlog::warn("[API] get_user failed user={} status={} duration={}ms", user, r.status_code, duration);
  return "";
}
// if all=false returns single score that peppy wants, else - all user scores on map
std::string Request::get_user_beatmap_score(const std::string_view beatmap,
                                            const std::string_view user, const bool all) {
  if (!update_token()) {
    spdlog::error("[API] Can't send requests, token is dead");
    return "";
  }

  auto start = std::chrono::steady_clock::now();
  cpr::Response r =
      cpr::Get(cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}/scores/users/{}{}",
                                    beatmap, user, all ? "/all" : "")},
               cpr::Header{{"Authorization", "Bearer " + config.access_token},
                           {"Content-Type", "application/json"},
                           {"Accept", "application/json"}});

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  const auto status_code = r.status_code;
  switch (status_code) {
    case 200:
      spdlog::info("[API] get_user_beatmap_score success user={} beatmap={} all={} duration={}ms",
        user, beatmap, all, duration);
      if (r.text != "{\"scores\":[]}")
        return r.text;
      else {
        spdlog::info("[API] No scores found for user={} on beatmap={}", user, beatmap);
        return {};
      }
    case 404:
      spdlog::info("[API] get_user_beatmap_score not found user={} beatmap={} duration={}ms",
        user, beatmap, duration);
      break;
    default:
      spdlog::warn("[API] get_user_beatmap_score failed user={} beatmap={} status={} duration={}ms",
        user, beatmap, status_code, duration);
      break;
  }
  return {};
}

std::string Request::get_beatmap(const std::string_view beatmap) {
  if (!update_token()) {
    spdlog::error("[API] Can't send requests, token is dead");
    return "";
  }

  auto start = std::chrono::steady_clock::now();
  cpr::Response r =
      cpr::Get(cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}", beatmap)},
               cpr::Header{{"Authorization", "Bearer " + config.access_token},
                           {"Content-Type", "application/json"},
                           {"Accept", "application/json"}});

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    spdlog::info("[API] get_beatmap success beatmap={} duration={}ms", beatmap, duration);
    return r.text;
  }
  spdlog::warn("[API] get_beatmap failed beatmap={} status={} duration={}ms",
    beatmap, r.status_code, duration);
  return "";
}

std::string Request::get_beatmap_attributes(const std::string_view beatmap, uint32_t mods_bitset) {
  if (!update_token()) {
    spdlog::error("[API] Can't send requests, token is dead");
    return "";
  }

  auto start = std::chrono::steady_clock::now();

  // Build JSON body with mods if provided
  json body;
  if (mods_bitset > 0) {
    body["mods"] = mods_bitset;
  }

  cpr::Response r = cpr::Post(
      cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}/attributes", beatmap)},
      cpr::Header{{"Authorization", "Bearer " + config.access_token},
                  {"Content-Type", "application/json"},
                  {"Accept", "application/json"}},
      cpr::Body{body.dump()});

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    spdlog::info("[API] get_beatmap_attributes success beatmap={} mods={} duration={}ms",
      beatmap, mods_bitset, duration);
    return r.text;
  }
  spdlog::warn("[API] get_beatmap_attributes failed beatmap={} mods={} status={} duration={}ms",
    beatmap, mods_bitset, r.status_code, duration);
  return "";
}

std::string Request::get_weather(const std::string_view city) {
  const std::string& key = config.weather_api_key;
  if (key.empty()) {
    spdlog::error("[API] WEATHER_API_KEY not configured in config.json");
    return "{}";
  }

  std::string city_ = city.data();
  auto start = std::chrono::steady_clock::now();
  auto response = cpr::Get(
    cpr::Url{"http://api.openweathermap.org/data/2.5/weather"},
    cpr::Parameters{cpr::Parameter{"q", city_},
                    cpr::Parameter{"appid", key},
                    cpr::Parameter{"units", "metric"},
                    cpr::Parameter{"lang", "ru"}
                  }
  );

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (response.status_code == 200) {
    auto j = json::parse(response.text);
    spdlog::info("[API] get_weather success city={} duration={}ms", city, duration);
    return j.dump(4);
  }

  spdlog::warn("[API] get_weather failed city={} status={} duration={}ms",
    city, response.status_code, duration);
  return std::string();
}

Request::Request() {
  utils::load_config(config);
  update_token();
}
