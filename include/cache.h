#pragma once

#include <libmemcached/memcached.h>
#include <string>
#include <optional>
#include <memory>
#include <chrono>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>
#include "osu.h"
#include "state/session_state.h"

namespace cache {

// Memcached client wrapper
class MemcachedCache {
public:
    // Initialize cache with connection parameters
    static void init(const std::string& host, int port);

    // Get the singleton instance
    static MemcachedCache& instance();

    // Disable copy and move
    MemcachedCache(const MemcachedCache&) = delete;
    MemcachedCache& operator=(const MemcachedCache&) = delete;

    ~MemcachedCache();

    // Generic set/get operations
    bool set(const std::string& key, const std::string& value, std::chrono::seconds ttl);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key);

    // LeaderboardState operations (5 min TTL)
    bool cache_leaderboard(const std::string& state_id, const LeaderboardState& state);
    std::optional<LeaderboardState> get_leaderboard(const std::string& state_id);
    bool delete_leaderboard(const std::string& state_id);

    // RecentScoreState operations (5 min TTL)
    bool cache_recent_scores(const std::string& state_id, const RecentScoreState& state);
    std::optional<RecentScoreState> get_recent_scores(const std::string& state_id);
    bool delete_recent_scores(const std::string& state_id);

    // CompareState operations (5 min TTL)
    bool cache_compare(const std::string& state_id, const CompareState& state);
    std::optional<CompareState> get_compare(const std::string& state_id);
    bool delete_compare(const std::string& state_id);

    // OAuth token operations (custom TTL based on expires_at)
    bool cache_oauth_tokens(const std::string& access_token, const std::string& refresh_token,
                           std::chrono::seconds expires_in);
    std::optional<std::pair<std::string, std::string>> get_oauth_tokens();

    // Username cache operations (1 hour TTL)
    bool cache_username(int64_t user_id, const std::string& username);
    std::optional<std::string> get_username(int64_t user_id);

    // Health check
    bool is_connected();

    // Clear all cache
    void flush_all();

private:
    MemcachedCache(const std::string& host, int port);

    static std::unique_ptr<MemcachedCache> instance_;
    static std::mutex init_mutex_;

    memcached_st* memc_;
    std::string prefix_ = "patchouli:";
    mutable std::mutex cache_mutex_; // Protect memcached operations from concurrent access

    // Helper to build prefixed keys
    std::string build_key(const std::string& key) const;

    // Serialization helpers
    std::string serialize_leaderboard(const LeaderboardState& state);
    std::optional<LeaderboardState> deserialize_leaderboard(const std::string& data);
    std::string serialize_recent_scores(const RecentScoreState& state);
    std::optional<RecentScoreState> deserialize_recent_scores(const std::string& data);
    std::string serialize_compare(const CompareState& state);
    std::optional<CompareState> deserialize_compare(const std::string& data);
};

} // namespace cache
