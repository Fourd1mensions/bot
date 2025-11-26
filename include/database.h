#pragma once

#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <optional>
#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <dpp/dpp.h>

namespace db {

// Structure to represent beatmap .osz file metadata
struct BeatmapFile {
    int64_t beatmapset_id;
    std::string osz_path;
    std::optional<std::string> mirror_hostname;
    std::optional<std::chrono::system_clock::time_point> last_accessed;
    std::optional<std::chrono::system_clock::time_point> created_at;
};

// Structure to represent temporary beatmap extract
struct BeatmapExtract {
    std::string extract_id;
    int64_t beatmapset_id;
    std::string extract_path;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point expires_at;
};

// Structure to represent cached user data
struct CachedUser {
    int64_t user_id;
    std::string username;
    std::chrono::system_clock::time_point expires_at;
};

// Connection pool for PostgreSQL
class ConnectionPool {
public:
    ConnectionPool(const std::string& connection_string, size_t pool_size = 5);
    ~ConnectionPool();

    // Get a connection from the pool (blocks if none available)
    std::unique_ptr<pqxx::connection> acquire();

    // Return a connection to the pool
    void release(std::unique_ptr<pqxx::connection> conn);

private:
    std::string conn_string_;
    std::queue<std::unique_ptr<pqxx::connection>> connections_;
    std::mutex mutex_;
    std::condition_variable cv_;
    size_t pool_size_;
};

// Main database interface
class Database {
public:
    // Initialize database with connection parameters
    static void init(const std::string& host, int port,
                    const std::string& dbname, const std::string& user,
                    const std::string& password, size_t pool_size = 5);

    // Get the singleton instance
    static Database& instance();

    // Disable copy and move
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Users table operations
    void set_user_mapping(dpp::snowflake discord_id, int64_t osu_user_id);
    std::optional<int64_t> get_osu_user_id(dpp::snowflake discord_id);
    bool remove_user_mapping(dpp::snowflake discord_id);
    std::vector<std::pair<dpp::snowflake, int64_t>> get_all_user_mappings();

    // Chat map table operations
    void set_chat_context(dpp::snowflake channel_id, dpp::snowflake message_id,
                         const std::string& beatmap_id);
    std::optional<std::pair<dpp::snowflake, std::string>> get_chat_context(dpp::snowflake channel_id);
    bool remove_chat_context(dpp::snowflake channel_id);

    // Beatmap files table operations (.osz storage)
    void register_beatmap_file(int64_t beatmapset_id, const std::string& osz_path,
                              const std::optional<std::string>& mirror_hostname = std::nullopt,
                              int64_t file_size = 0);
    void update_file_access(int64_t beatmapset_id);
    std::optional<BeatmapFile> get_beatmap_file(int64_t beatmapset_id);
    std::vector<BeatmapFile> get_all_beatmap_files();
    bool beatmap_file_exists(int64_t beatmapset_id);
    bool remove_beatmap_file(int64_t beatmapset_id);

    // Beatmap extracts table operations (temporary extracted files)
    std::string create_beatmap_extract(int64_t beatmapset_id, const std::string& extract_path,
                                       std::chrono::hours ttl = std::chrono::hours(24));
    std::optional<BeatmapExtract> get_beatmap_extract(const std::string& extract_id);
    bool remove_beatmap_extract(const std::string& extract_id);
    std::vector<BeatmapExtract> cleanup_expired_extracts();

    // Beatmap ID cache operations (beatmap_id -> beatmapset_id mapping)
    void cache_beatmap_id(int64_t beatmap_id, int64_t beatmapset_id, const std::string& mode = "osu");
    std::optional<int64_t> get_beatmapset_id(int64_t beatmap_id);
    void update_beatmap_id_access(int64_t beatmap_id);

    // Individual .osu files tracking
    void register_osu_file(int64_t beatmap_id, int64_t beatmapset_id,
                          const std::string& file_path, int64_t file_size = 0);
    void update_osu_file_access(int64_t beatmap_id);
    bool osu_file_exists(int64_t beatmap_id);
    std::optional<std::string> get_osu_file_path(int64_t beatmap_id);

    // User cache table operations
    void cache_username(int64_t user_id, const std::string& username,
                       std::chrono::minutes ttl = std::chrono::minutes(60));
    std::optional<std::string> get_cached_username(int64_t user_id);
    void cleanup_expired_cache();

    // Pending button removals table operations
    void register_pending_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id,
                                         std::chrono::system_clock::time_point expires_at);
    std::vector<std::pair<dpp::snowflake, dpp::snowflake>> get_expired_button_removals();
    std::vector<std::tuple<dpp::snowflake, dpp::snowflake, std::chrono::system_clock::time_point>> get_all_pending_removals();
    bool remove_pending_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id);

    // Health check
    bool is_connected();

    // Raw SQL execution (for migrations)
    template<typename Func>
    auto execute(Func&& func) -> decltype(func(std::declval<pqxx::connection&>())) {
        auto conn = pool_->acquire();
        try {
            if constexpr (std::is_void_v<decltype(func(*conn))>) {
                func(*conn);
                pool_->release(std::move(conn));
            } else {
                auto result = func(*conn);
                pool_->release(std::move(conn));
                return result;
            }
        } catch (...) {
            pool_->release(std::move(conn));
            throw;
        }
    }

private:
    Database(const std::string& connection_string, size_t pool_size);

    static std::unique_ptr<Database> instance_;
    static std::mutex init_mutex_;

    std::unique_ptr<ConnectionPool> pool_;
};

} // namespace db
