#include <requests.h>
#include <cache.h>
#include <debug_settings.h>

#include <chrono>
#include <string_view>

#include <cpr/cpr.h>
#include <fmt/base.h>
#include <spdlog/spdlog.h>

cpr::Session& Request::get_osu_session() {
    thread_local cpr::Session session;
    thread_local bool initialized = false;
    if (!initialized) {
        session.SetVerifySsl(true);
        initialized = true;
    }
    session.SetParameters(cpr::Parameters{});
    session.SetBody(cpr::Body{});
    return session;
}

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
  std::string url = fmt::format("https://osu.ppy.sh/api/get_user?u={}", username);
  log_request("GET", url);

  cpr::Response r      = cpr::Get(cpr::Url{"https://osu.ppy.sh/api/get_user"},
                                  cpr::Parameters{{"k", config.api_v1_key}, {"u", username.data()}});

  log_response(r);

  const size_t pos = r.text.find_first_of("0123456789");
  if (pos == std::string::npos) {
    spdlog::warn("[API] get_userid_v1: no numeric ID found in response for '{}'", username);
    return "";
  }
  const size_t endpos = r.text.find_first_not_of("0123456789", pos);
  return std::string(r.text.substr(pos, endpos - pos));
}

// user = username by default
std::string Request::get_user(const std::string_view user, const bool by_id, const std::string& mode) {
   if (!update_token()) {
    spdlog::error("[API] Can't send requests, token is dead");
    return "";
  }

  auto start = std::chrono::steady_clock::now();

  // URL-encode username if not using ID
  std::string encoded_user = by_id ? std::string(user) : cpr::util::urlEncode(std::string(user));
  // Mode: "osu", "taiko", "fruits", "mania"
  std::string api_mode = mode.empty() ? "osu" : mode;
  std::string url = fmt::format("https://osu.ppy.sh/api/v2/users/{}{}/{}", by_id ? "" : "@", encoded_user, api_mode);

  log_request("GET", url);

  cpr::Response r = execute_with_retry([&]() {
    auto& s = get_osu_session();
    s.SetUrl(cpr::Url{url});
    s.SetHeader(cpr::Header{{"Authorization", "Bearer " + config.access_token},
                {"Content-Type", "application/json"},
                {"Accept", "application/json"}});
    return s.Get();
  });

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    spdlog::info("[API] get_user success user={} mode={} duration={}ms", user, api_mode, duration);
    return r.text;
  }
  spdlog::warn("[API] get_user failed user={} mode={} status={} duration={}ms", user, api_mode, r.status_code, duration);
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
  std::string url = fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}/scores/users/{}{}",
                                beatmap, user, all ? "/all" : "");

  log_request("GET", url);

  cpr::Response r = execute_with_retry([&]() {
    auto& s = get_osu_session();
    s.SetUrl(cpr::Url{url});
    s.SetHeader(cpr::Header{{"Authorization", "Bearer " + config.access_token},
                {"Content-Type", "application/json"},
                {"Accept", "application/json"}});
    return s.Get();
  });

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  const auto status_code = r.status_code;
  switch (status_code) {
    case 200: {
      spdlog::info("[API] get_user_beatmap_score success user={} beatmap={} all={} duration={}ms",
        user, beatmap, all, duration);
      auto& dbg = debug::Settings::instance();
      if (dbg.verbose_osu_api.load()) {
        spdlog::info("[API] get_user_beatmap_score response: {}",
          debug::Settings::truncate(r.text, dbg.max_response_log_length.load()));
      }
      if (r.text != "{\"scores\":[]}")
        return r.text;
      else {
        spdlog::info("[API] No scores found for user={} on beatmap={}", user, beatmap);
        return {};
      }
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
  std::string url = fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}", beatmap);

  log_request("GET", url);

  cpr::Response r = execute_with_retry([&]() {
    auto& s = get_osu_session();
    s.SetUrl(cpr::Url{url});
    s.SetHeader(cpr::Header{{"Authorization", "Bearer " + config.access_token},
                {"Content-Type", "application/json"},
                {"Accept", "application/json"}});
    return s.Get();
  });

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    spdlog::info("[API] get_beatmap success beatmap={} duration={}ms", beatmap, duration);
    auto& dbg = debug::Settings::instance();
    if (dbg.verbose_osu_api.load()) {
      spdlog::info("[API] get_beatmap response: {}",
        debug::Settings::truncate(r.text, dbg.max_response_log_length.load()));
    }
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

  std::string url = fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}/attributes", beatmap);
  std::string body_str = body.dump();

  log_request("POST", url, body_str);

  cpr::Response r = execute_with_retry([&]() {
    auto& s = get_osu_session();
    s.SetUrl(cpr::Url{url});
    s.SetHeader(cpr::Header{{"Authorization", "Bearer " + config.access_token},
                {"Content-Type", "application/json"},
                {"Accept", "application/json"}});
    s.SetBody(cpr::Body{body_str});
    return s.Post();
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

  // Log request
  log_request("GET", fmt::format("{}?include_fails={}&mode={}&limit={}&offset={}",
    url, include_fails ? "1" : "0", mode, limit, offset));

  cpr::Response r = execute_with_retry([&]() {
    auto& s = get_osu_session();
    s.SetUrl(cpr::Url{url});
    s.SetParameters(params);
    s.SetHeader(cpr::Header{{"Authorization", "Bearer " + config.access_token},
                {"Content-Type", "application/json"},
                {"Accept", "application/json"}});
    return s.Get();
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

  // Log request
  log_request("GET", fmt::format("{}?mode={}&limit={}&offset={}", url, mode, limit, offset));

  cpr::Response r = execute_with_retry([&]() {
    auto& s = get_osu_session();
    s.SetUrl(cpr::Url{url});
    s.SetParameters(params);
    s.SetHeader(cpr::Header{{"Authorization", "Bearer " + config.access_token},
                {"Content-Type", "application/json"},
                {"Accept", "application/json"}});
    return s.Get();
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
  std::string url = fmt::format("https://osu.ppy.sh/api/v2/beatmapsets/{}", beatmapset_id);

  log_request("GET", url);

  auto& s = get_osu_session();
  s.SetUrl(cpr::Url{url});
  s.SetHeader(cpr::Header{{"Authorization", "Bearer " + config.access_token},
              {"Content-Type", "application/json"},
              {"Accept", "application/json"}});
  cpr::Response r = s.Get();

  log_response(r);

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

Request::OsuStatsCounts Request::get_osustats_counts(const std::string& username, int gamemode) {
  OsuStatsCounts counts;
  // Note: top1 should be fetched from user profile (scores_first_count) - more accurate than osustats
  // osustats only updates once per day
  const std::vector<std::pair<int, size_t*>> tiers = {
      {8, &counts.top8}, {15, &counts.top15},
      {25, &counts.top25}, {50, &counts.top50}, {100, &counts.top100}
  };

  auto start = std::chrono::steady_clock::now();
  spdlog::info("[API] get_osustats_counts starting for user={} mode={}", username, gamemode);

  for (auto& [rank_max, count_ptr] : tiers) {
    // Use /api/getScores endpoint - response format: [scores_array, total_count, bool, bool]
    cpr::Response r = cpr::Post(
        cpr::Url{"https://osustats.ppy.sh/api/getScores"},
        cpr::Payload{
            {"u1", username},
            {"rankMin", "1"},
            {"rankMax", std::to_string(rank_max)},
            {"gamemode", std::to_string(gamemode)},
            {"page", "1"}
        },
        cpr::Timeout{15000}
    );

    if (r.status_code != 200) {
      spdlog::warn("[API] osustats request failed: status={} rank_max={}", r.status_code, rank_max);
      continue;
    }

    try {
      auto arr = json::parse(r.text);
      // Response is [scores_array, total_count, has_more, unknown_bool]
      if (arr.is_array() && arr.size() >= 2 && arr[1].is_number()) {
        *count_ptr = arr[1].get<size_t>();
      }
    } catch (const json::exception& e) {
      spdlog::error("[API] osustats JSON parse error: {}", e.what());
    }
  }

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();

  spdlog::info("[API] get_osustats_counts done user={} mode={} duration={}ms "
               "top1={} top8={} top15={} top25={} top50={} top100={}",
               username, gamemode, duration,
               counts.top1, counts.top8, counts.top15,
               counts.top25, counts.top50, counts.top100);

  return counts;
}

std::string Request::search_beatmapsets(const std::string& query, int limit) {
  if (!update_token()) {
    spdlog::error("[API] Can't send requests, token is dead");
    return "";
  }

  auto start = std::chrono::steady_clock::now();
  std::string url = "https://osu.ppy.sh/api/v2/beatmapsets/search";

  log_request("GET", fmt::format("{}?q={}&limit={}", url, query, limit));

  cpr::Response r = execute_with_retry([&]() {
    auto& s = get_osu_session();
    s.SetUrl(cpr::Url{url});
    s.SetParameters(cpr::Parameters{{"q", query}, {"limit", std::to_string(limit)}});
    s.SetHeader(cpr::Header{{"Authorization", "Bearer " + config.access_token},
                {"Content-Type", "application/json"},
                {"Accept", "application/json"}});
    return s.Get();
  });

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  if (r.status_code == 200) {
    spdlog::info("[API] search_beatmapsets success query='{}' duration={}ms", query, duration);
    return r.text;
  }
  spdlog::warn("[API] search_beatmapsets failed query='{}' status={} duration={}ms",
    query, r.status_code, duration);
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
    cpr::Url{"https://api.openweathermap.org/data/2.5/weather"},
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
