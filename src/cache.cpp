#include "cache.h"
#include <spdlog/spdlog.h>
#include <format>
#include <cstring>

using json = nlohmann::json;

namespace cache {

// Static members initialization
std::unique_ptr<MemcachedCache> MemcachedCache::instance_ = nullptr;
std::mutex MemcachedCache::init_mutex_;

void MemcachedCache::init(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(init_mutex_);

    if (instance_) {
        spdlog::warn("Memcached cache already initialized, skipping");
        return;
    }

    instance_ = std::unique_ptr<MemcachedCache>(new MemcachedCache(host, port));
    spdlog::info("Memcached cache initialized: {}:{}", host, port);
}

MemcachedCache& MemcachedCache::instance() {
    if (!instance_) {
        throw std::runtime_error("Cache not initialized. Call MemcachedCache::init() first");
    }
    return *instance_;
}

MemcachedCache::MemcachedCache(const std::string& host, int port) {
    memc_ = memcached_create(nullptr);

    if (!memc_) {
        throw std::runtime_error("Failed to create memcached instance");
    }

    std::string server_config = std::format("{}:{}", host, port);
    memcached_server_st* servers = memcached_servers_parse(server_config.c_str());

    memcached_return_t rc = memcached_server_push(memc_, servers);
    memcached_server_list_free(servers);

    if (rc != MEMCACHED_SUCCESS) {
        memcached_free(memc_);
        throw std::runtime_error(std::format("Failed to add memcached server: {}",
                                            memcached_strerror(memc_, rc)));
    }

    // Set behavior options
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, 1);
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_NO_BLOCK, 1);
    memcached_behavior_set(memc_, MEMCACHED_BEHAVIOR_TCP_NODELAY, 1);
}

MemcachedCache::~MemcachedCache() {
    if (memc_) {
        memcached_free(memc_);
    }
}

std::string MemcachedCache::build_key(const std::string& key) const {
    return prefix_ + key;
}

bool MemcachedCache::set(const std::string& key, const std::string& value, std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    std::string full_key = build_key(key);
    memcached_return_t rc = memcached_set(
        memc_,
        full_key.c_str(), full_key.length(),
        value.c_str(), value.length(),
        static_cast<time_t>(ttl.count()),
        0
    );

    if (rc != MEMCACHED_SUCCESS) {
        spdlog::warn("Memcached set failed for key '{}': {}",
                    key, memcached_strerror(memc_, rc));
        return false;
    }

    spdlog::debug("Cached: {} (TTL: {}s)", key, ttl.count());
    return true;
}

std::optional<std::string> MemcachedCache::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    std::string full_key = build_key(key);
    size_t value_length;
    uint32_t flags;
    memcached_return_t rc;

    char* value = memcached_get(
        memc_,
        full_key.c_str(), full_key.length(),
        &value_length,
        &flags,
        &rc
    );

    if (rc == MEMCACHED_NOTFOUND) {
        if (value) free(value);
        return std::nullopt;
    }

    if (rc != MEMCACHED_SUCCESS) {
        spdlog::warn("Memcached get failed for key '{}': {}",
                    key, memcached_strerror(memc_, rc));
        if (value) free(value);
        return std::nullopt;
    }

    std::string result(value, value_length);
    free(value);

    spdlog::debug("Cache hit: {}", key);
    return result;
}

bool MemcachedCache::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    std::string full_key = build_key(key);
    memcached_return_t rc = memcached_delete(memc_, full_key.c_str(), full_key.length(), 0);

    if (rc != MEMCACHED_SUCCESS && rc != MEMCACHED_NOTFOUND) {
        spdlog::warn("Memcached delete failed for key '{}': {}",
                    key, memcached_strerror(memc_, rc));
        return false;
    }

    return true;
}

bool MemcachedCache::exists(const std::string& key) {
    return get(key).has_value();
}

// LeaderboardState serialization
std::string MemcachedCache::serialize_leaderboard(const LeaderboardState& state) {
    json j;
    j["mods_filter"] = state.mods_filter;
    j["sort_method"] = static_cast<int>(state.sort_method);
    j["current_page"] = state.current_page;
    j["total_pages"] = state.total_pages;
    j["caller_discord_id"] = static_cast<uint64_t>(state.caller_discord_id);
    j["created_at"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        state.created_at.time_since_epoch()
    ).count();

    // Serialize beatmap
    j["beatmap"]["beatmap_id"] = state.beatmap.beatmap_id;
    j["beatmap"]["beatmapset_id"] = state.beatmap.beatmapset_id;
    j["beatmap"]["max_combo"] = state.beatmap.max_combo;
    j["beatmap"]["difficulty_rating"] = state.beatmap.difficulty_rating;
    j["beatmap"]["modded_difficulty_rating"] = state.beatmap.modded_difficulty_rating;
    j["beatmap"]["has_modded_rating"] = state.beatmap.has_modded_rating;
    j["beatmap"]["artist"] = state.beatmap.artist;
    j["beatmap"]["title"] = state.beatmap.title;
    j["beatmap"]["version"] = state.beatmap.version;
    j["beatmap"]["beatmap_url"] = state.beatmap.beatmap_url;
    j["beatmap"]["image_url"] = state.beatmap.image_url;
    j["beatmap"]["mode"] = state.beatmap.mode;
    j["beatmap"]["bpm"] = state.beatmap.bpm;
    j["beatmap"]["total_length"] = state.beatmap.total_length;
    j["beatmap"]["status"] = static_cast<int>(state.beatmap.status);

    // Serialize scores
    j["scores"] = json::array();
    for (const auto& score : state.scores) {
        json score_json;
        score_json["accuracy"] = score.accuracy;
        score_json["max_combo"] = score.max_combo;
        score_json["total_score"] = score.total_score;
        score_json["mods"] = score.mods;
        score_json["rank"] = score.rank;
        score_json["created_at"] = score.created_at;
        score_json["username"] = score.username;
        score_json["count_miss"] = score.count_miss;
        score_json["count_50"] = score.count_50;
        score_json["count_100"] = score.count_100;
        score_json["count_300"] = score.count_300;
        score_json["pp"] = score.pp;
        score_json["user_id"] = score.user_id;
        score_json["beatmap_id"] = score.beatmap_id;
        score_json["passed"] = score.passed;
        score_json["mode"] = score.mode;
        j["scores"].push_back(score_json);
    }

    return j.dump();
}

std::optional<LeaderboardState> MemcachedCache::deserialize_leaderboard(const std::string& data) {
    try {
        json j = json::parse(data);

        LeaderboardState state;
        state.mods_filter = j["mods_filter"];
        state.sort_method = static_cast<LbSortMethod>(j.value("sort_method", 0)); // Default to PP for backward compatibility
        state.current_page = j["current_page"];
        state.total_pages = j["total_pages"];
        state.caller_discord_id = dpp::snowflake(j.value("caller_discord_id", 0ULL)); // Default to 0 for backward compatibility

        // Deserialize timestamp (defaults to now if not present for backwards compatibility)
        if (j.contains("created_at")) {
            auto millis = j["created_at"].get<int64_t>();
            state.created_at = std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(millis)
            );
        }

        // Deserialize beatmap
        state.beatmap.beatmap_id = j["beatmap"]["beatmap_id"];
        state.beatmap.beatmapset_id = j["beatmap"]["beatmapset_id"];
        state.beatmap.max_combo = j["beatmap"]["max_combo"];
        state.beatmap.difficulty_rating = j["beatmap"]["difficulty_rating"];
        state.beatmap.modded_difficulty_rating = j["beatmap"]["modded_difficulty_rating"];
        state.beatmap.has_modded_rating = j["beatmap"]["has_modded_rating"];
        state.beatmap.artist = j["beatmap"]["artist"];
        state.beatmap.title = j["beatmap"]["title"];
        state.beatmap.version = j["beatmap"]["version"];
        state.beatmap.beatmap_url = j["beatmap"]["beatmap_url"];
        state.beatmap.image_url = j["beatmap"]["image_url"];
        state.beatmap.mode = j["beatmap"].value("mode", "osu");
        state.beatmap.bpm = j["beatmap"].value("bpm", 0.0f);
        state.beatmap.total_length = j["beatmap"].value("total_length", 0);
        state.beatmap.status = static_cast<BeatmapStatus>(j["beatmap"].value("status", 0)); // Default to Pending for backwards compatibility

        // Deserialize scores
        for (const auto& score_json : j["scores"]) {
            Score score;
            score.accuracy = score_json["accuracy"];
            score.max_combo = score_json["max_combo"];
            score.total_score = score_json["total_score"];
            score.mods = score_json["mods"];
            score.rank = score_json["rank"];
            score.created_at = score_json["created_at"];
            score.username = score_json["username"];
            score.count_miss = score_json["count_miss"];
            score.count_50 = score_json["count_50"];
            score.count_100 = score_json["count_100"];
            score.count_300 = score_json["count_300"];
            score.pp = score_json["pp"];
            score.user_id = score_json["user_id"];
            score.beatmap_id = score_json.value("beatmap_id", 0);
            score.passed = score_json.value("passed", true);
            score.mode = score_json.value("mode", "osu");  // Default to osu for backwards compatibility
            score.is_empty = false;
            state.scores.push_back(score);
        }

        return state;
    } catch (const json::exception& e) {
        spdlog::error("Failed to deserialize leaderboard: {}", e.what());
        return std::nullopt;
    }
}

std::string MemcachedCache::serialize_recent_scores(const RecentScoreState& state) {
    json j;
    j["current_index"] = state.current_index;
    j["mode"] = state.mode;
    j["include_fails"] = state.include_fails;
    j["use_best_scores"] = state.use_best_scores;
    j["osu_user_id"] = state.osu_user_id;
    j["refresh_count"] = state.refresh_count;
    j["caller_discord_id"] = static_cast<uint64_t>(state.caller_discord_id);
    j["preset"] = services::embed_preset_to_string(state.preset);
    j["created_at"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        state.created_at.time_since_epoch()
    ).count();

    // Serialize scores
    j["scores"] = json::array();
    for (const auto& score : state.scores) {
        json score_json;
        score_json["accuracy"] = score.accuracy;
        score_json["max_combo"] = score.max_combo;
        score_json["total_score"] = score.total_score;
        score_json["mods"] = score.mods;
        score_json["rank"] = score.rank;
        score_json["created_at"] = score.created_at;
        score_json["username"] = score.username;
        score_json["count_miss"] = score.count_miss;
        score_json["count_50"] = score.count_50;
        score_json["count_100"] = score.count_100;
        score_json["count_300"] = score.count_300;
        score_json["pp"] = score.pp;
        score_json["user_id"] = score.user_id;
        score_json["beatmap_id"] = score.beatmap_id;
        score_json["passed"] = score.passed;
        score_json["mode"] = score.mode;
        j["scores"].push_back(score_json);
    }

    // Serialize beatmap difficulty cache
    j["difficulty_cache"] = json::object();
    for (const auto& [beatmap_id, data] : state.beatmap_difficulty_cache) {
        j["difficulty_cache"][std::to_string(beatmap_id)] = {std::get<0>(data), std::get<1>(data), std::get<2>(data), std::get<3>(data), std::get<4>(data)};
    }

    return j.dump();
}

std::optional<RecentScoreState> MemcachedCache::deserialize_recent_scores(const std::string& data) {
    try {
        json j = json::parse(data);

        RecentScoreState state;
        state.current_index = j["current_index"];
        state.mode = j.value("mode", "osu");
        state.include_fails = j.value("include_fails", false);
        state.use_best_scores = j.value("use_best_scores", false);
        state.osu_user_id = j.value("osu_user_id", 0);
        state.refresh_count = j.value("refresh_count", 0);
        state.caller_discord_id = dpp::snowflake(j.value("caller_discord_id", 0ULL));
        state.preset = services::embed_preset_from_string(j.value("preset", "classic"));

        // Deserialize timestamp
        if (j.contains("created_at")) {
            auto millis = j["created_at"].get<int64_t>();
            state.created_at = std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(millis)
            );
        }

        // Deserialize scores
        for (const auto& score_json : j["scores"]) {
            Score score;
            score.accuracy = score_json["accuracy"];
            score.max_combo = score_json["max_combo"];
            score.total_score = score_json["total_score"];
            score.mods = score_json["mods"];
            score.rank = score_json["rank"];
            score.created_at = score_json["created_at"];
            score.username = score_json["username"];
            score.count_miss = score_json["count_miss"];
            score.count_50 = score_json["count_50"];
            score.count_100 = score_json["count_100"];
            score.count_300 = score_json["count_300"];
            score.pp = score_json["pp"];
            score.user_id = score_json["user_id"];
            score.beatmap_id = score_json.value("beatmap_id", 0);
            score.passed = score_json.value("passed", true);
            score.mode = score_json.value("mode", "osu");  // Default to osu for backwards compatibility
            score.is_empty = false;
            state.scores.push_back(score);
        }

        // Deserialize beatmap difficulty cache
        if (j.contains("difficulty_cache")) {
            for (const auto& [key, value] : j["difficulty_cache"].items()) {
                uint32_t beatmap_id;
                try {
                    beatmap_id = std::stoul(key);
                } catch (const std::exception& e) {
                    spdlog::warn("Invalid beatmap_id key in difficulty_cache: '{}'", key);
                    continue;
                }
                float ar = value[0].get<float>();
                float od = value[1].get<float>();

                // Backwards compatible with old format (AR, OD, total_objects) and new format (AR, OD, CS, HP, total_objects)
                if (value.size() >= 5) {
                    // New format: AR, OD, CS, HP, total_objects
                    float cs = value[2].get<float>();
                    float hp = value[3].get<float>();
                    int total_objects = value[4].get<int>();
                    state.beatmap_difficulty_cache[beatmap_id] = std::make_tuple(ar, od, cs, hp, total_objects);
                } else {
                    // Old format: AR, OD, total_objects (use defaults for CS and HP)
                    int total_objects = value.size() > 2 ? value[2].get<int>() : 0;
                    state.beatmap_difficulty_cache[beatmap_id] = std::make_tuple(ar, od, 5.0f, 5.0f, total_objects);
                }
            }
        }

        return state;
    } catch (const json::exception& e) {
        spdlog::error("Failed to deserialize recent scores: {}", e.what());
        return std::nullopt;
    }
}

// LeaderboardState operations
bool MemcachedCache::cache_leaderboard(const std::string& state_id, const LeaderboardState& state) {
    std::string data = serialize_leaderboard(state);
    return set("leaderboard:" + state_id, data, std::chrono::seconds(300)); // 5 minutes
}

std::optional<LeaderboardState> MemcachedCache::get_leaderboard(const std::string& state_id) {
    auto data = get("leaderboard:" + state_id);
    if (!data) {
        return std::nullopt;
    }
    return deserialize_leaderboard(*data);
}

bool MemcachedCache::delete_leaderboard(const std::string& state_id) {
    return del("leaderboard:" + state_id);
}

// RecentScoreState operations
bool MemcachedCache::cache_recent_scores(const std::string& state_id, const RecentScoreState& state) {
    std::string data = serialize_recent_scores(state);
    return set("recent_scores:" + state_id, data, std::chrono::seconds(300)); // 5 minutes
}

std::optional<RecentScoreState> MemcachedCache::get_recent_scores(const std::string& state_id) {
    auto data = get("recent_scores:" + state_id);
    if (!data) {
        return std::nullopt;
    }
    return deserialize_recent_scores(*data);
}

bool MemcachedCache::delete_recent_scores(const std::string& state_id) {
    return del("recent_scores:" + state_id);
}

// CompareState operations
bool MemcachedCache::cache_compare(const std::string& state_id, const CompareState& state) {
    std::string data = serialize_compare(state);
    return set("compare:" + state_id, data, std::chrono::seconds(300)); // 5 minutes
}

std::optional<CompareState> MemcachedCache::get_compare(const std::string& state_id) {
    auto data = get("compare:" + state_id);
    if (!data) {
        return std::nullopt;
    }
    return deserialize_compare(*data);
}

bool MemcachedCache::delete_compare(const std::string& state_id) {
    return del("compare:" + state_id);
}

std::string MemcachedCache::serialize_compare(const CompareState& state) {
    json j;
    j["username"] = state.username;
    j["mods_filter"] = state.mods_filter;
    j["current_page"] = state.current_page;
    j["total_pages"] = state.total_pages;
    j["caller_discord_id"] = static_cast<uint64_t>(state.caller_discord_id);
    j["created_at"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        state.created_at.time_since_epoch()
    ).count();

    // Serialize beatmap
    j["beatmap"]["beatmap_id"] = state.beatmap.beatmap_id;
    j["beatmap"]["beatmapset_id"] = state.beatmap.beatmapset_id;
    j["beatmap"]["max_combo"] = state.beatmap.max_combo;
    j["beatmap"]["difficulty_rating"] = state.beatmap.difficulty_rating;
    j["beatmap"]["artist"] = state.beatmap.artist;
    j["beatmap"]["title"] = state.beatmap.title;
    j["beatmap"]["version"] = state.beatmap.version;
    j["beatmap"]["beatmap_url"] = state.beatmap.beatmap_url;
    j["beatmap"]["image_url"] = state.beatmap.image_url;

    // Serialize scores
    j["scores"] = json::array();
    for (const auto& score : state.scores) {
        json score_json;
        score_json["accuracy"] = score.accuracy;
        score_json["max_combo"] = score.max_combo;
        score_json["total_score"] = score.total_score;
        score_json["mods"] = score.mods;
        score_json["rank"] = score.rank;
        score_json["count_miss"] = score.count_miss;
        score_json["count_50"] = score.count_50;
        score_json["count_100"] = score.count_100;
        score_json["count_300"] = score.count_300;
        score_json["pp"] = score.pp;
        score_json["passed"] = score.passed;
        j["scores"].push_back(score_json);
    }

    return j.dump();
}

std::optional<CompareState> MemcachedCache::deserialize_compare(const std::string& data) {
    try {
        json j = json::parse(data);

        CompareState state;
        state.username = j["username"];
        state.mods_filter = j["mods_filter"];
        state.current_page = j["current_page"];
        state.total_pages = j["total_pages"];
        state.caller_discord_id = dpp::snowflake(j.value("caller_discord_id", 0ULL));

        if (j.contains("created_at")) {
            auto millis = j["created_at"].get<int64_t>();
            state.created_at = std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(millis)
            );
        }

        // Deserialize beatmap
        state.beatmap.beatmap_id = j["beatmap"]["beatmap_id"];
        state.beatmap.beatmapset_id = j["beatmap"]["beatmapset_id"];
        state.beatmap.max_combo = j["beatmap"]["max_combo"];
        state.beatmap.difficulty_rating = j["beatmap"]["difficulty_rating"];
        state.beatmap.artist = j["beatmap"]["artist"];
        state.beatmap.title = j["beatmap"]["title"];
        state.beatmap.version = j["beatmap"]["version"];
        state.beatmap.beatmap_url = j["beatmap"]["beatmap_url"];
        state.beatmap.image_url = j["beatmap"]["image_url"];

        // Deserialize scores
        for (const auto& score_json : j["scores"]) {
            Score score;
            score.accuracy = score_json["accuracy"];
            score.max_combo = score_json["max_combo"];
            score.total_score = score_json["total_score"];
            score.mods = score_json["mods"];
            score.rank = score_json["rank"];
            score.count_miss = score_json["count_miss"];
            score.count_50 = score_json["count_50"];
            score.count_100 = score_json["count_100"];
            score.count_300 = score_json["count_300"];
            score.pp = score_json["pp"];
            score.passed = score_json.value("passed", true);
            score.is_empty = false;
            state.scores.push_back(score);
        }

        return state;
    } catch (const json::exception& e) {
        spdlog::error("Failed to deserialize compare: {}", e.what());
        return std::nullopt;
    }
}

// UsersState operations
bool MemcachedCache::cache_users(const std::string& state_id, const UsersState& state) {
    std::string data = serialize_users(state);
    return set("users:" + state_id, data, std::chrono::seconds(300)); // 5 minutes
}

std::optional<UsersState> MemcachedCache::get_users(const std::string& state_id) {
    auto data = get("users:" + state_id);
    if (!data) {
        return std::nullopt;
    }
    return deserialize_users(*data);
}

bool MemcachedCache::delete_users(const std::string& state_id) {
    return del("users:" + state_id);
}

std::string MemcachedCache::serialize_users(const UsersState& state) {
    json j;
    j["current_page"] = state.current_page;
    j["total_pages"] = state.total_pages;
    j["caller_discord_id"] = static_cast<uint64_t>(state.caller_discord_id);
    j["created_at"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        state.created_at.time_since_epoch()
    ).count();

    j["users"] = json::array();
    for (const auto& user : state.users) {
        json user_json;
        user_json["discord_id"] = static_cast<uint64_t>(user.discord_id);
        user_json["osu_user_id"] = user.osu_user_id;
        user_json["osu_username"] = user.osu_username;
        j["users"].push_back(user_json);
    }

    return j.dump();
}

std::optional<UsersState> MemcachedCache::deserialize_users(const std::string& data) {
    try {
        json j = json::parse(data);

        UsersState state;
        state.current_page = j["current_page"];
        state.total_pages = j["total_pages"];
        state.caller_discord_id = dpp::snowflake(j.value("caller_discord_id", 0ULL));

        if (j.contains("created_at")) {
            auto millis = j["created_at"].get<int64_t>();
            state.created_at = std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(millis)
            );
        }

        for (const auto& user_json : j["users"]) {
            UserMapping user;
            user.discord_id = dpp::snowflake(user_json["discord_id"].get<uint64_t>());
            user.osu_user_id = user_json["osu_user_id"];
            user.osu_username = user_json["osu_username"];
            state.users.push_back(user);
        }

        // Ensure total_pages is valid (avoid underflow in pagination)
        if (state.total_pages == 0) state.total_pages = 1;

        return state;
    } catch (const json::exception& e) {
        spdlog::error("Failed to deserialize users: {}", e.what());
        return std::nullopt;
    }
}

// TopState operations
bool MemcachedCache::cache_top(const std::string& state_id, const TopState& state) {
    std::string data = serialize_top(state);
    return set("top:" + state_id, data, std::chrono::seconds(300)); // 5 minutes
}

std::optional<TopState> MemcachedCache::get_top(const std::string& state_id) {
    auto data = get("top:" + state_id);
    if (!data) {
        return std::nullopt;
    }
    return deserialize_top(*data);
}

bool MemcachedCache::delete_top(const std::string& state_id) {
    return del("top:" + state_id);
}

std::string MemcachedCache::serialize_top(const TopState& state) {
    json j;
    j["username"] = state.username;
    j["osu_user_id"] = state.osu_user_id;
    j["mode"] = state.mode;
    j["mods_filter"] = state.mods_filter;
    j["grade_filter"] = state.grade_filter;
    j["sort_method"] = static_cast<int>(state.sort_method);
    j["reverse"] = state.reverse;
    j["current_page"] = state.current_page;
    j["total_pages"] = state.total_pages;
    j["caller_discord_id"] = static_cast<uint64_t>(state.caller_discord_id);
    j["preset"] = static_cast<int>(state.preset);
    j["created_at"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        state.created_at.time_since_epoch()
    ).count();

    // Serialize scores
    j["scores"] = json::array();
    for (const auto& score : state.scores) {
        json score_json;
        score_json["accuracy"] = score.accuracy;
        score_json["max_combo"] = score.max_combo;
        score_json["total_score"] = score.total_score;
        score_json["mods"] = score.mods;
        score_json["rank"] = score.rank;
        score_json["created_at"] = score.created_at;
        score_json["username"] = score.username;
        score_json["count_miss"] = score.count_miss;
        score_json["count_50"] = score.count_50;
        score_json["count_100"] = score.count_100;
        score_json["count_300"] = score.count_300;
        score_json["pp"] = score.pp;
        score_json["user_id"] = score.user_id;
        score_json["beatmap_id"] = score.beatmap_id;
        score_json["passed"] = score.passed;
        score_json["mode"] = score.mode;
        score_json["weight_percentage"] = score.weight_percentage;
        score_json["weight_pp"] = score.weight_pp;
        // Beatmap metadata
        score_json["beatmapset_id"] = score.beatmapset_id;
        score_json["beatmap_title"] = score.beatmap_title;
        score_json["beatmap_artist"] = score.beatmap_artist;
        score_json["beatmap_version"] = score.beatmap_version;
        score_json["beatmap_max_combo"] = score.beatmap_max_combo;
        j["scores"].push_back(score_json);
    }

    return j.dump();
}

std::optional<TopState> MemcachedCache::deserialize_top(const std::string& data) {
    try {
        json j = json::parse(data);

        TopState state;
        state.username = j["username"];
        state.osu_user_id = j["osu_user_id"];
        state.mode = j["mode"];
        state.mods_filter = j.value("mods_filter", "");
        state.grade_filter = j.value("grade_filter", "");
        state.sort_method = static_cast<TopSortMethod>(j.value("sort_method", 0));
        state.reverse = j.value("reverse", false);
        state.current_page = j["current_page"];
        state.total_pages = j["total_pages"];
        state.caller_discord_id = dpp::snowflake(j.value("caller_discord_id", 0ULL));
        state.preset = static_cast<services::EmbedPreset>(j.value("preset", 1));

        if (j.contains("created_at")) {
            auto millis = j["created_at"].get<int64_t>();
            state.created_at = std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(millis)
            );
        }

        // Deserialize scores
        for (const auto& score_json : j["scores"]) {
            Score score;
            score.accuracy = score_json["accuracy"];
            score.max_combo = score_json["max_combo"];
            score.total_score = score_json["total_score"];
            score.mods = score_json["mods"];
            score.rank = score_json["rank"];
            score.created_at = score_json["created_at"];
            score.username = score_json["username"];
            score.count_miss = score_json["count_miss"];
            score.count_50 = score_json["count_50"];
            score.count_100 = score_json["count_100"];
            score.count_300 = score_json["count_300"];
            score.pp = score_json["pp"];
            score.user_id = score_json["user_id"];
            score.beatmap_id = score_json["beatmap_id"];
            score.passed = score_json["passed"];
            score.mode = score_json["mode"];
            score.weight_percentage = score_json.value("weight_percentage", 0.0f);
            score.weight_pp = score_json.value("weight_pp", 0.0f);
            // Beatmap metadata
            score.beatmapset_id = score_json.value("beatmapset_id", 0u);
            score.beatmap_title = score_json.value("beatmap_title", "");
            score.beatmap_artist = score_json.value("beatmap_artist", "");
            score.beatmap_version = score_json.value("beatmap_version", "");
            score.beatmap_max_combo = score_json.value("beatmap_max_combo", 0u);
            score.is_empty = false;
            state.scores.push_back(score);
        }

        // Ensure total_pages is valid
        if (state.total_pages == 0) state.total_pages = 1;

        return state;
    } catch (const json::exception& e) {
        spdlog::error("Failed to deserialize top: {}", e.what());
        return std::nullopt;
    }
}

// OAuth token operations
bool MemcachedCache::cache_oauth_tokens(const std::string& access_token,
                                       const std::string& refresh_token,
                                       std::chrono::seconds expires_in) {
    json j;
    j["access_token"] = access_token;
    j["refresh_token"] = refresh_token;

    return set("oauth:tokens", j.dump(), expires_in);
}

std::optional<std::pair<std::string, std::string>> MemcachedCache::get_oauth_tokens() {
    auto data = get("oauth:tokens");
    if (!data) {
        return std::nullopt;
    }

    try {
        json j = json::parse(*data);
        return std::make_pair(
            j["access_token"].get<std::string>(),
            j["refresh_token"].get<std::string>()
        );
    } catch (const json::exception& e) {
        spdlog::error("Failed to deserialize OAuth tokens: {}", e.what());
        return std::nullopt;
    }
}

// Username cache operations
bool MemcachedCache::cache_username(int64_t user_id, const std::string& username) {
    std::string key = std::format("username:{}", user_id);
    return set(key, username, std::chrono::seconds(3600)); // 1 hour
}

std::optional<std::string> MemcachedCache::get_username(int64_t user_id) {
    std::string key = std::format("username:{}", user_id);
    return get(key);
}

// Beatmap API response cache
bool MemcachedCache::cache_beatmap(uint32_t beatmap_id, const std::string& api_response_json) {
    std::string key = std::format("beatmap:{}", beatmap_id);
    return set(key, api_response_json, std::chrono::seconds(86400)); // 24 hours
}

std::optional<std::string> MemcachedCache::get_cached_beatmap(uint32_t beatmap_id) {
    std::string key = std::format("beatmap:{}", beatmap_id);
    return get(key);
}

// Health check
bool MemcachedCache::is_connected() {
    try {
        set("health:check", "ok", std::chrono::seconds(10));
        auto result = get("health:check");
        return result.has_value() && *result == "ok";
    } catch (const std::exception& e) {
        spdlog::error("Memcached health check failed: {}", e.what());
        return false;
    }
}

void MemcachedCache::flush_all() {
    memcached_return_t rc = memcached_flush(memc_, 0);
    if (rc != MEMCACHED_SUCCESS) {
        spdlog::warn("Memcached flush failed: {}", memcached_strerror(memc_, rc));
    } else {
        spdlog::info("Memcached cache flushed");
    }
}

} // namespace cache
