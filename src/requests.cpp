#include <requests.h>
#include <cache.h>

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
    int expires_in      = j.value("expires_in", 86995);
    config.expires_at   = now + expires_in;

    utils::save_config(config);

    // Cache tokens in Memcached with TTL (expires_in - 5 minutes for safety)
    try {
      auto& cache = cache::MemcachedCache::instance();
      std::string refresh_token = j.value("refresh_token", "");
      int cache_ttl = std::max(300, expires_in - 300); // At least 5 minutes, or (expires_in - 5 minutes)
      cache.cache_oauth_tokens(config.access_token, refresh_token, std::chrono::seconds(cache_ttl));
      spdlog::debug("OAuth tokens cached in Memcached (TTL: {}s)", cache_ttl);
    } catch (const std::exception& e) {
      spdlog::warn("Failed to cache OAuth tokens in Memcached: {}", e.what());
    }

    spdlog::info("[API] OAuth token acquired successfully ({}ms)", duration);
    return true;
  }
  spdlog::error("[API] Failed to retrieve OAuth token status={} duration={}ms error={}",
    r.status_code, duration, r.text);
  return false;
}

bool Request::update_token() {
  size_t now = utils::get_time();

  // Check if our local token is still valid
  if (now <= config.expires_at && !config.access_token.empty()) {
    return true;
  }

  // Token expired or empty, check Memcached cache for shared token
  try {
    auto& cache = cache::MemcachedCache::instance();
    if (auto tokens = cache.get_oauth_tokens()) {
      const auto& [access_token, refresh_token] = *tokens;
      if (!access_token.empty()) {
        config.access_token = access_token;
        // Set expires_at conservatively since we don't know exact expiry from cache
        config.expires_at = now + 3600; // 1 hour
        utils::save_config(config);
        spdlog::info("[API] OAuth token retrieved from Memcached cache");
        return true;
      }
    }
  } catch (const std::exception& e) {
    spdlog::debug("Failed to get OAuth tokens from Memcached: {}", e.what());
  }

  // Cache miss or expired, request new token
  return set_token();
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

  // URL-encode username if not using ID
  std::string encoded_user = by_id ? std::string(user) : cpr::util::urlEncode(std::string(user));

  cpr::Response r = execute_with_retry([&]() {
    return cpr::Get(
        cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/users/{}{}/osu", by_id ? "" : "@", encoded_user)},
        cpr::Header{{"Authorization", "Bearer " + config.access_token},
                    {"Content-Type", "application/json"},
                    {"Accept", "application/json"}});
  });

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
  cpr::Response r = execute_with_retry([&]() {
    return cpr::Get(cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}/scores/users/{}{}",
                                         beatmap, user, all ? "/all" : "")},
                    cpr::Header{{"Authorization", "Bearer " + config.access_token},
                                {"Content-Type", "application/json"},
                                {"Accept", "application/json"}});
  });

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
  cpr::Response r = execute_with_retry([&]() {
    return cpr::Get(cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}", beatmap)},
                    cpr::Header{{"Authorization", "Bearer " + config.access_token},
                                {"Content-Type", "application/json"},
                                {"Accept", "application/json"}});
  });

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

  cpr::Response r = execute_with_retry([&]() {
    return cpr::Post(
        cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}/attributes", beatmap)},
        cpr::Header{{"Authorization", "Bearer " + config.access_token},
                    {"Content-Type", "application/json"},
                    {"Accept", "application/json"}},
        cpr::Body{body.dump()});
  });

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

std::string Request::get_user_recent_scores(const std::string_view user_id,
                                           bool include_fails,
                                           const std::string& mode,
                                           int limit,
                                           int offset) {
  if (!update_token()) {
    spdlog::error("[API] Can't send requests, token is dead");
    return "";
  }

  auto start = std::chrono::steady_clock::now();

  // Build URL with parameters
  std::string url = fmt::format("https://osu.ppy.sh/api/v2/users/{}/scores/recent", user_id);

  cpr::Parameters params;
  params.Add({"include_fails", include_fails ? "1" : "0"});
  if (!mode.empty() && mode != "osu") {
    params.Add({"mode", mode});
  }
  if (limit > 0) {
    params.Add({"limit", std::to_string(limit)});
  }
  if (offset > 0) {
    params.Add({"offset", std::to_string(offset)});
  }

  cpr::Response r = execute_with_retry([&]() {
    return cpr::Get(
        cpr::Url{url},
        params,
        cpr::Header{{"Authorization", "Bearer " + config.access_token},
                    {"Content-Type", "application/json"},
                    {"Accept", "application/json"}});
  });

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    spdlog::info("[API] get_user_recent_scores success user={} include_fails={} limit={} duration={}ms",
      user_id, include_fails, limit, duration);
    return r.text;
  }
  spdlog::warn("[API] get_user_recent_scores failed user={} status={} duration={}ms",
    user_id, r.status_code, duration);
  return "";
}

std::string Request::get_user_best_scores(const std::string_view user_id,
                                         const std::string& mode,
                                         int limit,
                                         int offset) {
  if (!update_token()) {
    spdlog::error("[API] Can't send requests, token is dead");
    return "";
  }

  auto start = std::chrono::steady_clock::now();

  // Build URL with parameters
  std::string url = fmt::format("https://osu.ppy.sh/api/v2/users/{}/scores/best", user_id);

  cpr::Parameters params;
  if (!mode.empty() && mode != "osu") {
    params.Add({"mode", mode});
  }
  if (limit > 0) {
    params.Add({"limit", std::to_string(limit)});
  }
  if (offset > 0) {
    params.Add({"offset", std::to_string(offset)});
  }

  cpr::Response r = execute_with_retry([&]() {
    return cpr::Get(
        cpr::Url{url},
        params,
        cpr::Header{{"Authorization", "Bearer " + config.access_token},
                    {"Content-Type", "application/json"},
                    {"Accept", "application/json"}});
  });

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    spdlog::info("[API] get_user_best_scores success user={} limit={} duration={}ms",
      user_id, limit, duration);
    return r.text;
  }
  spdlog::warn("[API] get_user_best_scores failed user={} status={} duration={}ms",
    user_id, r.status_code, duration);
  return "";
}

std::string Request::get_beatmap_id_from_set(const std::string_view beatmapset_id) {
  if (!update_token()) {
    spdlog::error("[API] Can't send requests, token is dead");
    return "";
  }

  auto start = std::chrono::steady_clock::now();
  cpr::Response r =
      cpr::Get(cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmapsets/{}", beatmapset_id)},
               cpr::Header{{"Authorization", "Bearer " + config.access_token},
                           {"Content-Type", "application/json"},
                           {"Accept", "application/json"}});

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    try {
      json j = json::parse(r.text);
      if (j.contains("beatmaps") && !j["beatmaps"].empty()) {
        auto beatmap_id = j["beatmaps"].at(0).value("id", 0);
        if (beatmap_id != 0) {
          spdlog::info("[API] get_beatmap_id_from_set success beatmapset={} beatmap={} duration={}ms",
            beatmapset_id, beatmap_id, duration);
          return fmt::to_string(beatmap_id);
        }
      }
      spdlog::warn("[API] get_beatmap_id_from_set no beatmaps in set beatmapset={} duration={}ms",
        beatmapset_id, duration);
    } catch (const json::exception& e) {
      spdlog::error("[API] Failed to parse beatmapset {}: {}", beatmapset_id, e.what());
    }
    return "";
  }

  spdlog::warn("[API] get_beatmap_id_from_set failed beatmapset={} status={} duration={}ms",
    beatmapset_id, r.status_code, duration);
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
