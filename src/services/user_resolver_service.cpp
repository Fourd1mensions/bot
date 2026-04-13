#include "services/user_resolver_service.h"
#include "services/command_params_service.h"
#include "requests.h"
#include "database.h"
#include "cache.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace services {

using json = nlohmann::json;

UserResolverService::UserResolverService(Request& request)
    : request_(request) {}

UserResolveResult UserResolverService::resolve(
    const std::string& username,
    dpp::snowflake caller_discord_id
) const {
    UserResolveResult result;

    if (username.empty()) {
        // Use caller's linked account
        auto& db = db::Database::instance();
        auto osu_id_opt = db.get_osu_user_id(caller_discord_id);

        if (!osu_id_opt) {
            result.error_message = "you need to link your osu! account first with /set";
            return result;
        }

        result.osu_user_id = *osu_id_opt;
        return result;
    }

    // Check if username is a Discord mention
    auto mention_id = CommandParamsService::parse_discord_mention(username);
    if (mention_id) {
        try {
            dpp::snowflake mentioned_discord_id = std::stoull(*mention_id);

            auto& db = db::Database::instance();
            auto osu_id_opt = db.get_osu_user_id(mentioned_discord_id);

            if (!osu_id_opt) {
                result.error_message = fmt::format(
                    "user <@{}> hasn't linked their osu! account yet", *mention_id);
                return result;
            }

            result.osu_user_id = *osu_id_opt;
            spdlog::debug("[UserResolver] Resolved mention: Discord {} -> osu! {}",
                *mention_id, result.osu_user_id);
            return result;

        } catch (const std::exception& e) {
            result.error_message = "invalid user mention";
            spdlog::error("[UserResolver] Failed to parse Discord mention: {}", e.what());
            return result;
        }
    }

    // Look up user by username via osu! API
    std::string user_response = request_.get_user(username, false);
    if (user_response.empty()) {
        result.error_message = fmt::format("user '{}' not found", username);
        return result;
    }

    try {
        json user_json = json::parse(user_response);
        result.osu_user_id = user_json.value("id", static_cast<int64_t>(0));

        if (result.osu_user_id == 0) {
            result.error_message = fmt::format("user '{}' not found", username);
        }
    } catch (const json::exception& e) {
        result.error_message = "failed to parse user data";
        spdlog::error("[UserResolver] Failed to parse user response: {}", e.what());
    }

    return result;
}

std::string UserResolverService::get_username_cached(int64_t user_id) {
    // Try Memcached first (hot cache)
    try {
        auto& cache = cache::MemcachedCache::instance();
        if (auto cached = cache.get_username(user_id)) {
            spdlog::debug("[CACHE] Username HIT (Memcached) for user {} -> {}", user_id, *cached);
            return *cached;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[CACHE] Memcached get_username failed for user {}: {}", user_id, e.what());
    }

    // Try PostgreSQL cache (warm cache)
    try {
        auto& db = db::Database::instance();
        if (auto cached = db.get_cached_username(user_id)) {
            spdlog::debug("[CACHE] Username HIT (PostgreSQL) for user {} -> {}", user_id, *cached);

            // Update Memcached with this username
            try {
                auto& cache = cache::MemcachedCache::instance();
                cache.cache_username(user_id, *cached);
                spdlog::debug("[CACHE] Promoted username to Memcached");
            } catch (const std::exception& e) {
                spdlog::debug("[CACHE] Failed to promote to Memcached: {}", e.what());
            }

            return *cached;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[CACHE] PostgreSQL get_cached_username failed for user {}: {}", user_id, e.what());
    }

    // Cache miss - fetch from API
    spdlog::debug("[CACHE] Username MISS for user {}, fetching from API", user_id);
    std::string usr_j = request_.get_user(fmt::format("{}", user_id), true);
    if (usr_j.empty()) {
        spdlog::warn("[CACHE] Empty API response for user {}", user_id);
        return fmt::format("User {}", user_id);
    }

    std::string username;
    try {
        json usr = json::parse(usr_j);
        username = usr.value("username", "");
    } catch (const std::exception& e) {
        spdlog::warn("[CACHE] Failed to parse user API response for {}: {}", user_id, e.what());
        return fmt::format("User {}", user_id);
    }

    if (username.empty()) {
        spdlog::warn("[CACHE] No username field in API response for user {}", user_id);
        return fmt::format("User {}", user_id);
    }
    spdlog::debug("[CACHE] Fetched username from API: {} -> {}", user_id, username);

    // Cache in both layers
    try {
        auto& db = db::Database::instance();
        db.cache_username(user_id, username);
        spdlog::debug("[CACHE] Cached username in PostgreSQL");
    } catch (const std::exception& e) {
        spdlog::warn("[CACHE] Failed to cache username in PostgreSQL: {}", e.what());
    }

    try {
        auto& cache = cache::MemcachedCache::instance();
        cache.cache_username(user_id, username);
        spdlog::debug("[CACHE] Cached username in Memcached");
    } catch (const std::exception& e) {
        spdlog::debug("[CACHE] Failed to cache username in Memcached: {}", e.what());
    }

    return username;
}

} // namespace services
