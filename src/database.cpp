#include "database.h"
#include <spdlog/spdlog.h>
#include <format>

namespace db {

// Static members initialization
std::unique_ptr<Database> Database::instance_ = nullptr;
std::mutex Database::init_mutex_;

// ConnectionPool implementation
ConnectionPool::ConnectionPool(const std::string& connection_string, size_t pool_size)
    : conn_string_(connection_string), pool_size_(pool_size) {

    for (size_t i = 0; i < pool_size; ++i) {
        try {
            connections_.push(std::make_unique<pqxx::connection>(conn_string_));
        } catch (const std::exception& e) {
            spdlog::error("Failed to create database connection: {}", e.what());
            throw;
        }
    }
    spdlog::info("Database connection pool initialized with {} connections", pool_size);
}

ConnectionPool::~ConnectionPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!connections_.empty()) {
        connections_.pop();
    }
}

std::unique_ptr<pqxx::connection> ConnectionPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !connections_.empty(); });

    auto conn = std::move(connections_.front());
    connections_.pop();
    return conn;
}

void ConnectionPool::release(std::unique_ptr<pqxx::connection> conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.push(std::move(conn));
    cv_.notify_one();
}

// Database implementation
void Database::init(const std::string& host, int port,
                   const std::string& dbname, const std::string& user,
                   const std::string& password, size_t pool_size) {
    std::lock_guard<std::mutex> lock(init_mutex_);

    if (instance_) {
        spdlog::warn("Database already initialized, skipping");
        return;
    }

    std::string conn_str = std::format(
        "host={} port={} dbname={} user={} password={}",
        host, port, dbname, user, password
    );

    instance_ = std::unique_ptr<Database>(new Database(conn_str, pool_size));
    spdlog::info("Database initialized: {}:{}/{}", host, port, dbname);
}

Database& Database::instance() {
    if (!instance_) {
        throw std::runtime_error("Database not initialized. Call Database::init() first");
    }
    return *instance_;
}

Database::Database(const std::string& connection_string, size_t pool_size)
    : pool_(std::make_unique<ConnectionPool>(connection_string, pool_size)) {}

// Users table operations
void Database::set_user_mapping(dpp::snowflake discord_id, int64_t osu_user_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO users (discord_id, osu_user_id) VALUES ($1, $2) "
            "ON CONFLICT (discord_id) DO UPDATE SET osu_user_id = $2, updated_at = CURRENT_TIMESTAMP",
            static_cast<int64_t>(discord_id), osu_user_id
        );
        txn.commit();
        spdlog::debug("Set user mapping: {} -> {}", discord_id, osu_user_id);
    });
}

std::optional<int64_t> Database::get_osu_user_id(dpp::snowflake discord_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<int64_t> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT osu_user_id FROM users WHERE discord_id = $1",
            static_cast<int64_t>(discord_id)
        );

        if (result.empty()) {
            return std::nullopt;
        }

        return result[0][0].as<int64_t>();
    });
}

bool Database::remove_user_mapping(dpp::snowflake discord_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "DELETE FROM users WHERE discord_id = $1",
            static_cast<int64_t>(discord_id)
        );
        txn.commit();
        return result.affected_rows() > 0;
    });
}

std::vector<std::pair<dpp::snowflake, int64_t>> Database::get_all_user_mappings() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT discord_id, osu_user_id FROM users");

        std::vector<std::pair<dpp::snowflake, int64_t>> mappings;
        mappings.reserve(result.size());

        for (const auto& row : result) {
            mappings.emplace_back(
                dpp::snowflake(row[0].as<uint64_t>()),
                row[1].as<int64_t>()
            );
        }

        return mappings;
    });
}

// Chat map table operations
void Database::set_chat_context(dpp::snowflake channel_id, dpp::snowflake message_id,
                                const std::string& beatmap_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO chat_map (channel_id, message_id, beatmap_id) VALUES ($1, $2, $3) "
            "ON CONFLICT (channel_id) DO UPDATE SET message_id = $2, beatmap_id = $3, updated_at = CURRENT_TIMESTAMP",
            static_cast<int64_t>(channel_id), static_cast<int64_t>(message_id), beatmap_id
        );
        txn.commit();
        spdlog::debug("Set chat context: channel={} beatmap={}", channel_id, beatmap_id);
    });
}

std::optional<std::pair<dpp::snowflake, std::string>> Database::get_chat_context(dpp::snowflake channel_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<std::pair<dpp::snowflake, std::string>> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT message_id, beatmap_id FROM chat_map WHERE channel_id = $1",
            static_cast<int64_t>(channel_id)
        );

        if (result.empty()) {
            return std::nullopt;
        }

        return std::make_pair(
            dpp::snowflake(result[0][0].as<uint64_t>()),
            result[0][1].as<std::string>()
        );
    });
}

bool Database::remove_chat_context(dpp::snowflake channel_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "DELETE FROM chat_map WHERE channel_id = $1",
            static_cast<int64_t>(channel_id)
        );
        txn.commit();
        return result.affected_rows() > 0;
    });
}

// Beatmap files table operations (.osz storage)
void Database::register_beatmap_file(int64_t beatmapset_id, const std::string& osz_path,
                                    const std::optional<std::string>& mirror_hostname,
                                    int64_t file_size) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO beatmap_files (beatmapset_id, osz_path, mirror_hostname, file_size, last_accessed, access_count) "
            "VALUES ($1, $2, $3, $4, CURRENT_TIMESTAMP, 0) "
            "ON CONFLICT (beatmapset_id) DO UPDATE SET "
            "osz_path = $2, mirror_hostname = $3, file_size = $4, last_accessed = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP",
            beatmapset_id,
            osz_path,
            mirror_hostname ? pqxx::zview(*mirror_hostname) : pqxx::zview(),
            file_size
        );
        txn.commit();
        spdlog::debug("Registered beatmap .osz file for set {} ({} bytes)", beatmapset_id, file_size);
    });
}

void Database::update_file_access(int64_t beatmapset_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "UPDATE beatmap_files SET last_accessed = CURRENT_TIMESTAMP, "
            "access_count = access_count + 1, "
            "updated_at = CURRENT_TIMESTAMP WHERE beatmapset_id = $1",
            beatmapset_id
        );
        txn.commit();
    });
}

std::optional<BeatmapFile> Database::get_beatmap_file(int64_t beatmapset_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<BeatmapFile> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT beatmapset_id, osz_path, mirror_hostname, last_accessed, created_at "
            "FROM beatmap_files WHERE beatmapset_id = $1",
            beatmapset_id
        );

        if (result.empty()) {
            return std::nullopt;
        }

        const auto& row = result[0];
        BeatmapFile file;
        file.beatmapset_id = row[0].as<int64_t>();
        file.osz_path = row[1].as<std::string>();
        file.mirror_hostname = row[2].is_null() ? std::nullopt : std::optional(row[2].as<std::string>());

        if (!row[3].is_null()) {
            auto ts = row[3].as<std::string>();
            std::tm tm = {};
            std::istringstream ss(ts);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            file.last_accessed = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        }

        if (!row[4].is_null()) {
            auto ts = row[4].as<std::string>();
            std::tm tm = {};
            std::istringstream ss(ts);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            file.created_at = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        }

        return file;
    });
}

std::vector<BeatmapFile> Database::get_all_beatmap_files() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec(
            "SELECT beatmapset_id, osz_path, mirror_hostname "
            "FROM beatmap_files"
        );

        std::vector<BeatmapFile> files;
        files.reserve(result.size());

        for (const auto& row : result) {
            BeatmapFile file;
            file.beatmapset_id = row[0].as<int64_t>();
            file.osz_path = row[1].as<std::string>();
            file.mirror_hostname = row[2].is_null() ? std::nullopt : std::optional(row[2].as<std::string>());
            files.push_back(file);
        }

        return files;
    });
}

bool Database::beatmap_file_exists(int64_t beatmapset_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT 1 FROM beatmap_files WHERE beatmapset_id = $1",
            beatmapset_id
        );
        return !result.empty();
    });
}

bool Database::remove_beatmap_file(int64_t beatmapset_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "DELETE FROM beatmap_files WHERE beatmapset_id = $1",
            beatmapset_id
        );
        txn.commit();
        return result.affected_rows() > 0;
    });
}

// Beatmap extracts operations
std::string Database::create_beatmap_extract(int64_t beatmapset_id, const std::string& extract_path,
                                            std::chrono::hours ttl) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        // Generate unique extract_id
        auto result = txn.exec("SELECT REPLACE(gen_random_uuid()::text, '-', '')");
        std::string extract_id = result[0][0].as<std::string>();

        // Calculate expiry time
        auto expires = std::chrono::system_clock::now() + ttl;
        auto time_t_expires = std::chrono::system_clock::to_time_t(expires);
        std::tm tm = *std::gmtime(&time_t_expires);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);

        txn.exec_params(
            "INSERT INTO beatmap_extracts (extract_id, beatmapset_id, extract_path, expires_at) "
            "VALUES ($1, $2, $3, $4::timestamp)",
            extract_id, beatmapset_id, extract_path, buffer
        );
        txn.commit();

        spdlog::debug("Created beatmap extract {} for set {} (TTL: {}h)", extract_id, beatmapset_id, ttl.count());
        return extract_id;
    });
}

std::optional<BeatmapExtract> Database::get_beatmap_extract(const std::string& extract_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<BeatmapExtract> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT extract_id, beatmapset_id, extract_path, created_at, expires_at "
            "FROM beatmap_extracts WHERE extract_id = $1",
            extract_id
        );

        if (result.empty()) {
            return std::nullopt;
        }

        const auto& row = result[0];
        BeatmapExtract extract;
        extract.extract_id = row[0].as<std::string>();
        extract.beatmapset_id = row[1].as<int64_t>();
        extract.extract_path = row[2].as<std::string>();

        auto ts_created = row[3].as<std::string>();
        std::tm tm_created = {};
        std::istringstream ss_created(ts_created);
        ss_created >> std::get_time(&tm_created, "%Y-%m-%d %H:%M:%S");
        extract.created_at = std::chrono::system_clock::from_time_t(std::mktime(&tm_created));

        auto ts_expires = row[4].as<std::string>();
        std::tm tm_expires = {};
        std::istringstream ss_expires(ts_expires);
        ss_expires >> std::get_time(&tm_expires, "%Y-%m-%d %H:%M:%S");
        extract.expires_at = std::chrono::system_clock::from_time_t(std::mktime(&tm_expires));

        return extract;
    });
}

bool Database::remove_beatmap_extract(const std::string& extract_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "DELETE FROM beatmap_extracts WHERE extract_id = $1",
            extract_id
        );
        txn.commit();
        return result.affected_rows() > 0;
    });
}

std::vector<BeatmapExtract> Database::cleanup_expired_extracts() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec(
            "DELETE FROM beatmap_extracts WHERE expires_at < CURRENT_TIMESTAMP "
            "RETURNING extract_id, beatmapset_id, extract_path, created_at, expires_at"
        );

        std::vector<BeatmapExtract> extracts;
        extracts.reserve(result.size());

        for (const auto& row : result) {
            BeatmapExtract extract;
            extract.extract_id = row[0].as<std::string>();
            extract.beatmapset_id = row[1].as<int64_t>();
            extract.extract_path = row[2].as<std::string>();

            auto ts_created = row[3].as<std::string>();
            std::tm tm_created = {};
            std::istringstream ss_created(ts_created);
            ss_created >> std::get_time(&tm_created, "%Y-%m-%d %H:%M:%S");
            extract.created_at = std::chrono::system_clock::from_time_t(std::mktime(&tm_created));

            auto ts_expires = row[4].as<std::string>();
            std::tm tm_expires = {};
            std::istringstream ss_expires(ts_expires);
            ss_expires >> std::get_time(&tm_expires, "%Y-%m-%d %H:%M:%S");
            extract.expires_at = std::chrono::system_clock::from_time_t(std::mktime(&tm_expires));

            extracts.push_back(extract);
        }

        txn.commit();

        if (!extracts.empty()) {
            spdlog::info("Cleaned up {} expired beatmap extracts", extracts.size());
        }

        return extracts;
    });
}

// User cache operations
void Database::cache_username(int64_t user_id, const std::string& username,
                             std::chrono::minutes ttl) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        auto expires = std::chrono::system_clock::now() + ttl;
        auto time_t_expires = std::chrono::system_clock::to_time_t(expires);
        std::tm tm = *std::gmtime(&time_t_expires);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);

        txn.exec_params(
            "INSERT INTO user_cache (user_id, username, expires_at) VALUES ($1, $2, $3::timestamp) "
            "ON CONFLICT (user_id) DO UPDATE SET username = $2, expires_at = $3::timestamp",
            user_id, username, buffer
        );
        txn.commit();
        spdlog::debug("Cached username: {} -> {} (TTL: {}min)", user_id, username, ttl.count());
    });
}

std::optional<std::string> Database::get_cached_username(int64_t user_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<std::string> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT username FROM user_cache WHERE user_id = $1 AND expires_at > CURRENT_TIMESTAMP",
            user_id
        );

        if (result.empty()) {
            return std::nullopt;
        }

        return result[0][0].as<std::string>();
    });
}

void Database::cleanup_expired_cache() {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("DELETE FROM user_cache WHERE expires_at < CURRENT_TIMESTAMP");
        txn.commit();

        if (result.affected_rows() > 0) {
            spdlog::debug("Cleaned up {} expired cache entries", result.affected_rows());
        }
    });
}

// Pending button removals operations
void Database::register_pending_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id,
                                               std::chrono::system_clock::time_point expires_at) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        auto expires_str = std::format("{:%Y-%m-%d %H:%M:%S}",
            std::chrono::floor<std::chrono::seconds>(expires_at));

        txn.exec_params(
            "INSERT INTO pending_button_removals (channel_id, message_id, expires_at) "
            "VALUES ($1, $2, $3::timestamp) "
            "ON CONFLICT (channel_id, message_id) DO UPDATE SET expires_at = EXCLUDED.expires_at",
            static_cast<int64_t>(channel_id),
            static_cast<int64_t>(message_id),
            expires_str
        );

        txn.commit();
        spdlog::debug("Registered pending button removal for message {} in channel {}", message_id.str(), channel_id.str());
    });
}

std::vector<std::tuple<dpp::snowflake, dpp::snowflake, std::string>> Database::get_expired_button_removals() {
    return execute([](pqxx::connection& conn) {
        pqxx::work txn(conn);

        auto result = txn.exec(
            "SELECT channel_id, message_id, removal_type FROM pending_button_removals "
            "WHERE expires_at <= CURRENT_TIMESTAMP"
        );

        std::vector<std::tuple<dpp::snowflake, dpp::snowflake, std::string>> removals;
        removals.reserve(result.size());

        for (const auto& row : result) {
            dpp::snowflake channel_id(row["channel_id"].as<int64_t>());
            dpp::snowflake message_id(row["message_id"].as<int64_t>());
            std::string removal_type = row["removal_type"].as<std::string>();
            removals.emplace_back(channel_id, message_id, removal_type);
        }

        return removals;
    });
}

std::vector<std::tuple<dpp::snowflake, dpp::snowflake, std::chrono::system_clock::time_point, std::string>>
Database::get_all_pending_removals() {
    return execute([](pqxx::connection& conn) {
        pqxx::work txn(conn);

        auto result = txn.exec(
            "SELECT channel_id, message_id, expires_at, removal_type FROM pending_button_removals "
            "ORDER BY expires_at ASC"
        );

        std::vector<std::tuple<dpp::snowflake, dpp::snowflake, std::chrono::system_clock::time_point, std::string>> removals;
        removals.reserve(result.size());

        for (const auto& row : result) {
            dpp::snowflake channel_id(row["channel_id"].as<int64_t>());
            dpp::snowflake message_id(row["message_id"].as<int64_t>());

            // Parse timestamp
            std::string expires_str = row["expires_at"].as<std::string>();
            std::istringstream ss(expires_str);
            std::tm tm = {};
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            auto expires_at = std::chrono::system_clock::from_time_t(std::mktime(&tm));

            std::string removal_type = row["removal_type"].as<std::string>();
            removals.emplace_back(channel_id, message_id, expires_at, removal_type);
        }

        return removals;
    });
}

bool Database::remove_pending_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        auto result = txn.exec_params(
            "DELETE FROM pending_button_removals WHERE channel_id = $1 AND message_id = $2",
            static_cast<int64_t>(channel_id),
            static_cast<int64_t>(message_id)
        );

        txn.commit();

        if (result.affected_rows() > 0) {
            spdlog::debug("Removed pending button removal for message {} in channel {}",
                         message_id.str(), channel_id.str());
            return true;
        }
        return false;
    });
}

bool Database::is_connected() {
    try {
        execute([](pqxx::connection& conn) {
            pqxx::work txn(conn);
            txn.exec("SELECT 1");
        });
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Database connection check failed: {}", e.what());
        return false;
    }
}

// ============================================================================
// Beatmap ID cache operations (beatmap_id -> beatmapset_id mapping)
// ============================================================================

void Database::cache_beatmap_id(int64_t beatmap_id, int64_t beatmapset_id, const std::string& mode) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO beatmap_id_cache (beatmap_id, beatmapset_id, mode, cached_at, last_accessed) "
            "VALUES ($1, $2, $3, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
            "ON CONFLICT (beatmap_id) DO UPDATE SET "
            "beatmapset_id = EXCLUDED.beatmapset_id, "
            "mode = EXCLUDED.mode, "
            "last_accessed = CURRENT_TIMESTAMP",
            beatmap_id, beatmapset_id, mode
        );
        txn.commit();
        spdlog::debug("[DB] Cached beatmap_id {} -> beatmapset_id {} (mode: {})",
                     beatmap_id, beatmapset_id, mode);
    });
}

std::optional<int64_t> Database::get_beatmapset_id(int64_t beatmap_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<int64_t> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT beatmapset_id FROM beatmap_id_cache WHERE beatmap_id = $1",
            beatmap_id
        );

        if (result.empty()) {
            return std::nullopt;
        }

        // Update last_accessed asynchronously (best effort, don't care if it fails)
        try {
            txn.exec_params(
                "UPDATE beatmap_id_cache SET last_accessed = CURRENT_TIMESTAMP WHERE beatmap_id = $1",
                beatmap_id
            );
            txn.commit();
        } catch (...) {
            // Ignore update errors
        }

        return result[0][0].as<int64_t>();
    });
}

void Database::update_beatmap_id_access(int64_t beatmap_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "UPDATE beatmap_id_cache SET last_accessed = CURRENT_TIMESTAMP WHERE beatmap_id = $1",
            beatmap_id
        );
        txn.commit();
    });
}

// ============================================================================
// Individual .osu files tracking
// ============================================================================

void Database::register_osu_file(int64_t beatmap_id, int64_t beatmapset_id,
                                const std::string& file_path, int64_t file_size) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO osu_files (beatmap_id, beatmapset_id, file_path, file_size, created_at, last_accessed, access_count) "
            "VALUES ($1, $2, $3, $4, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 0) "
            "ON CONFLICT (beatmap_id) DO UPDATE SET "
            "file_path = EXCLUDED.file_path, "
            "file_size = EXCLUDED.file_size, "
            "last_accessed = CURRENT_TIMESTAMP",
            beatmap_id, beatmapset_id, file_path, file_size
        );
        txn.commit();
        spdlog::debug("[DB] Registered .osu file for beatmap {} (set: {}, size: {} bytes)",
                     beatmap_id, beatmapset_id, file_size);
    });
}

void Database::update_osu_file_access(int64_t beatmap_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "UPDATE osu_files SET last_accessed = CURRENT_TIMESTAMP, access_count = access_count + 1 "
            "WHERE beatmap_id = $1",
            beatmap_id
        );
        txn.commit();
    });
}

bool Database::osu_file_exists(int64_t beatmap_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT 1 FROM osu_files WHERE beatmap_id = $1",
            beatmap_id
        );
        return !result.empty();
    });
}

std::optional<std::string> Database::get_osu_file_path(int64_t beatmap_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<std::string> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT file_path FROM osu_files WHERE beatmap_id = $1",
            beatmap_id
        );

        if (result.empty()) {
            return std::nullopt;
        }

        // Update access stats asynchronously
        try {
            txn.exec_params(
                "UPDATE osu_files SET last_accessed = CURRENT_TIMESTAMP, access_count = access_count + 1 "
                "WHERE beatmap_id = $1",
                beatmap_id
            );
            txn.commit();
        } catch (...) {
            // Ignore update errors
        }

        return result[0][0].as<std::string>();
    });
}

} // namespace db
