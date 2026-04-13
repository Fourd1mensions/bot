#pragma once

#include <string>
#include <optional>
#include <variant>
#include <cstdint>
#include <dpp/dpp.h>

// Forward declarations
class Request;

namespace services {

/**
 * Result of user resolution - either success with osu_user_id or an error message.
 */
struct UserResolveResult {
    int64_t osu_user_id = 0;
    std::string error_message;

    bool success() const { return osu_user_id != 0; }
    operator bool() const { return success(); }
};

/**
 * Service for resolving usernames/mentions to osu! user IDs.
 * Handles:
 * - Empty username -> caller's linked account from database
 * - Discord mention (<@123>) -> lookup linked account
 * - Username string -> osu! API lookup
 */
class UserResolverService {
public:
    explicit UserResolverService(Request& request);
    ~UserResolverService() = default;

    // Disable copy
    UserResolverService(const UserResolverService&) = delete;
    UserResolverService& operator=(const UserResolverService&) = delete;

    /**
     * Resolve a username/mention to osu! user ID.
     * @param username Username, Discord mention, or empty for caller
     * @param caller_discord_id Discord ID of the command caller
     * @return Result with osu_user_id or error_message
     */
    UserResolveResult resolve(
        const std::string& username,
        dpp::snowflake caller_discord_id
    ) const;

    /**
     * Get username for osu! user ID with multi-layer caching.
     * Checks Memcached -> PostgreSQL -> API.
     */
    std::string get_username_cached(int64_t user_id);

private:
    Request& request_;
};

} // namespace services
