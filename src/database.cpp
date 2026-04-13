#include "database.h"
#include <spdlog/spdlog.h>
#include <format>
#include <sstream>
#include <iomanip>

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
void Database::set_user_mapping(dpp::snowflake discord_id, int64_t osu_user_id, bool is_oauth) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO users (discord_id, osu_user_id, is_oauth_linked) VALUES ($1, $2, $3) "
            "ON CONFLICT (discord_id) DO UPDATE SET osu_user_id = $2, is_oauth_linked = $3, updated_at = CURRENT_TIMESTAMP",
            static_cast<int64_t>(discord_id), osu_user_id, is_oauth
        );
        txn.commit();
        spdlog::debug("Set user mapping: {} -> {} (oauth: {})", discord_id, osu_user_id, is_oauth);
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

bool Database::is_user_oauth_linked(dpp::snowflake discord_id) {
    return execute([&](pqxx::connection& conn) -> bool {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT is_oauth_linked FROM users WHERE discord_id = $1",
            static_cast<int64_t>(discord_id)
        );

        if (result.empty()) {
            return false;
        }

        return result[0][0].as<bool>();
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

size_t Database::count_beatmap_files() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT COUNT(*) FROM beatmap_files");
        txn.commit();
        return result[0][0].as<size_t>();
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
// User settings operations
// ============================================================================

void Database::set_embed_preset(dpp::snowflake discord_id, const std::string& preset) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO user_settings (discord_id, embed_preset) VALUES ($1, $2) "
            "ON CONFLICT (discord_id) DO UPDATE SET embed_preset = $2",
            static_cast<int64_t>(discord_id), preset);
        txn.commit();
    });
}

std::string Database::get_embed_preset(dpp::snowflake discord_id) {
    return execute([&](pqxx::connection& conn) -> std::string {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT embed_preset FROM user_settings WHERE discord_id = $1",
            static_cast<int64_t>(discord_id));
        if (result.empty()) return "classic";
        return result[0][0].as<std::string>();
    });
}

void Database::delete_embed_preset(dpp::snowflake discord_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "DELETE FROM user_settings WHERE discord_id = $1",
            static_cast<int64_t>(discord_id));
        txn.commit();
    });
}

std::unordered_map<uint64_t, std::string> Database::get_all_embed_presets() {
    return execute([](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT discord_id, embed_preset FROM user_settings");
        std::unordered_map<uint64_t, std::string> presets;
        for (const auto& row : result) {
            presets[static_cast<uint64_t>(row[0].as<int64_t>())] = row[1].as<std::string>();
        }
        return presets;
    });
}

// ============================================================================
// User custom template operations
// ============================================================================

std::unordered_map<std::string, std::string> Database::get_user_custom_templates(dpp::snowflake discord_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT command_id, json_config FROM user_custom_templates WHERE discord_id = $1",
            static_cast<int64_t>(discord_id));
        std::unordered_map<std::string, std::string> templates;
        for (const auto& row : result) {
            templates[row[0].as<std::string>()] = row[1].as<std::string>();
        }
        return templates;
    });
}

std::optional<std::string> Database::get_user_custom_template(dpp::snowflake discord_id, const std::string& command_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<std::string> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT json_config FROM user_custom_templates WHERE discord_id = $1 AND command_id = $2",
            static_cast<int64_t>(discord_id), command_id);
        if (result.empty()) return std::nullopt;
        return result[0][0].as<std::string>();
    });
}

void Database::set_user_custom_template(dpp::snowflake discord_id, const std::string& command_id, const std::string& json_config) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO user_custom_templates (discord_id, command_id, json_config) VALUES ($1, $2, $3) "
            "ON CONFLICT (discord_id, command_id) DO UPDATE SET json_config = $3, updated_at = NOW()",
            static_cast<int64_t>(discord_id), command_id, json_config);
        txn.commit();
    });
}

void Database::delete_user_custom_template(dpp::snowflake discord_id, const std::string& command_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "DELETE FROM user_custom_templates WHERE discord_id = $1 AND command_id = $2",
            static_cast<int64_t>(discord_id), command_id);
        txn.commit();
    });
}

void Database::delete_all_user_custom_templates(dpp::snowflake discord_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "DELETE FROM user_custom_templates WHERE discord_id = $1",
            static_cast<int64_t>(discord_id));
        txn.commit();
    });
}

// ============================================================================
// Embed preset template operations
// ============================================================================

std::vector<std::tuple<std::string, std::string, std::string>> Database::get_all_preset_templates() {
    return execute([](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT preset_name, field_name, template FROM embed_preset_templates");
        std::vector<std::tuple<std::string, std::string, std::string>> templates;
        for (const auto& row : result) {
            templates.emplace_back(
                row[0].as<std::string>(),
                row[1].as<std::string>(),
                row[2].as<std::string>());
        }
        return templates;
    });
}

void Database::set_preset_template(const std::string& preset_name,
                                   const std::string& field_name,
                                   const std::string& template_text) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO embed_preset_templates (preset_name, field_name, template) "
            "VALUES ($1, $2, $3) "
            "ON CONFLICT (preset_name, field_name) DO UPDATE SET template = $3",
            preset_name, field_name, template_text);
        txn.commit();
    });
}

void Database::delete_preset_templates(const std::string& preset_name) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "DELETE FROM embed_preset_templates WHERE preset_name = $1",
            preset_name);
        txn.commit();
    });
}

// ============================================================================
// JSON embed template operations
// ============================================================================

std::vector<std::pair<std::string, std::string>> Database::get_all_json_templates() {
    return execute([](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT template_key, json_config FROM embed_json_templates");
        std::vector<std::pair<std::string, std::string>> templates;
        for (const auto& row : result) {
            templates.emplace_back(
                row[0].as<std::string>(),
                row[1].as<std::string>());
        }
        return templates;
    });
}

std::optional<std::string> Database::get_json_template(const std::string& key) {
    return execute([&](pqxx::connection& conn) -> std::optional<std::string> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT json_config FROM embed_json_templates WHERE template_key = $1",
            key);
        if (result.empty()) {
            return std::nullopt;
        }
        return result[0][0].as<std::string>();
    });
}

void Database::set_json_template(const std::string& key, const std::string& json_config) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO embed_json_templates (template_key, json_config) "
            "VALUES ($1, $2) "
            "ON CONFLICT (template_key) DO UPDATE SET json_config = $2",
            key, json_config);
        txn.commit();
    });
}

void Database::delete_json_template(const std::string& key) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "DELETE FROM embed_json_templates WHERE template_key = $1",
            key);
        txn.commit();
    });
}

// ============================================================================
// Discord users cache operations
// ============================================================================

void Database::cache_discord_user(const DiscordUser& user) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO discord_users (user_id, username, global_name, avatar_hash, is_bot, updated_at) "
            "VALUES ($1, $2, $3, $4, $5, CURRENT_TIMESTAMP) "
            "ON CONFLICT (user_id) DO UPDATE SET "
            "username = EXCLUDED.username, "
            "global_name = EXCLUDED.global_name, "
            "avatar_hash = EXCLUDED.avatar_hash, "
            "is_bot = EXCLUDED.is_bot, "
            "updated_at = CURRENT_TIMESTAMP",
            static_cast<int64_t>(user.user_id),
            user.username,
            user.global_name.empty() ? std::optional<std::string>{} : user.global_name,
            user.avatar_hash.empty() ? std::optional<std::string>{} : user.avatar_hash,
            user.is_bot
        );
        txn.commit();
    });
}

void Database::cache_discord_users_batch(const std::vector<DiscordUser>& users) {
    if (users.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& user : users) {
            txn.exec_params(
                "INSERT INTO discord_users (user_id, username, global_name, avatar_hash, is_bot, updated_at) "
                "VALUES ($1, $2, $3, $4, $5, CURRENT_TIMESTAMP) "
                "ON CONFLICT (user_id) DO UPDATE SET "
                "username = EXCLUDED.username, "
                "global_name = EXCLUDED.global_name, "
                "avatar_hash = EXCLUDED.avatar_hash, "
                "is_bot = EXCLUDED.is_bot, "
                "updated_at = CURRENT_TIMESTAMP",
                static_cast<int64_t>(user.user_id),
                user.username,
                user.global_name.empty() ? std::optional<std::string>{} : user.global_name,
                user.avatar_hash.empty() ? std::optional<std::string>{} : user.avatar_hash,
                user.is_bot
            );
        }

        txn.commit();
    });
}

std::optional<DiscordUser> Database::get_discord_user(dpp::snowflake user_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<DiscordUser> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT user_id, username, global_name, avatar_hash, is_bot FROM discord_users WHERE user_id = $1",
            static_cast<int64_t>(user_id)
        );

        if (result.empty()) {
            return std::nullopt;
        }

        DiscordUser user;
        user.user_id = dpp::snowflake(result[0][0].as<int64_t>());
        user.username = result[0][1].as<std::string>();
        user.global_name = result[0][2].is_null() ? "" : result[0][2].as<std::string>();
        user.avatar_hash = result[0][3].is_null() ? "" : result[0][3].as<std::string>();
        user.is_bot = result[0][4].as<bool>();
        return user;
    });
}

size_t Database::get_discord_user_count() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT COUNT(*) FROM discord_users WHERE is_bot = false");
        return result[0][0].as<size_t>();
    });
}

void Database::clear_discord_users() {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec("DELETE FROM discord_users");
        txn.commit();
    });
}

// ============================================================================
// Guild info cache operations
// ============================================================================

void Database::cache_guild_info(const GuildInfo& guild) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO guild_info (guild_id, name, icon_hash, member_count, updated_at) "
            "VALUES ($1, $2, $3, $4, CURRENT_TIMESTAMP) "
            "ON CONFLICT (guild_id) DO UPDATE SET "
            "name = EXCLUDED.name, "
            "icon_hash = EXCLUDED.icon_hash, "
            "member_count = EXCLUDED.member_count, "
            "updated_at = CURRENT_TIMESTAMP",
            static_cast<int64_t>(guild.guild_id),
            guild.name,
            guild.icon_hash.empty() ? std::optional<std::string>{} : guild.icon_hash,
            static_cast<int>(guild.member_count)
        );
        txn.commit();
    });
}

std::optional<GuildInfo> Database::get_guild_info(dpp::snowflake guild_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<GuildInfo> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT guild_id, name, icon_hash, member_count FROM guild_info WHERE guild_id = $1",
            static_cast<int64_t>(guild_id)
        );

        if (result.empty()) {
            return std::nullopt;
        }

        GuildInfo guild;
        guild.guild_id = dpp::snowflake(result[0][0].as<int64_t>());
        guild.name = result[0][1].as<std::string>();
        guild.icon_hash = result[0][2].is_null() ? "" : result[0][2].as<std::string>();
        guild.member_count = result[0][3].as<size_t>();
        return guild;
    });
}

// ============================================================================
// Channel info cache operations
// ============================================================================

void Database::cache_channel_info(const DiscordChannel& channel) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO discord_channels (channel_id, guild_id, channel_name, channel_type) "
            "VALUES ($1, $2, $3, $4) "
            "ON CONFLICT (channel_id) DO UPDATE SET "
            "channel_name = EXCLUDED.channel_name, "
            "channel_type = EXCLUDED.channel_type, "
            "updated_at = CURRENT_TIMESTAMP",
            static_cast<int64_t>(channel.channel_id),
            static_cast<int64_t>(channel.guild_id),
            channel.channel_name,
            channel.channel_type
        );
        txn.commit();
    });
}

void Database::cache_channels_batch(const std::vector<DiscordChannel>& channels) {
    if (channels.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& channel : channels) {
            txn.exec_params(
                "INSERT INTO discord_channels (channel_id, guild_id, channel_name, channel_type) "
                "VALUES ($1, $2, $3, $4) "
                "ON CONFLICT (channel_id) DO UPDATE SET "
                "channel_name = EXCLUDED.channel_name, "
                "channel_type = EXCLUDED.channel_type, "
                "updated_at = CURRENT_TIMESTAMP",
                static_cast<int64_t>(channel.channel_id),
                static_cast<int64_t>(channel.guild_id),
                channel.channel_name,
                channel.channel_type
            );
        }

        txn.commit();
    });
}

std::optional<DiscordChannel> Database::get_channel_info(dpp::snowflake channel_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<DiscordChannel> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT channel_id, guild_id, channel_name, channel_type FROM discord_channels WHERE channel_id = $1",
            static_cast<int64_t>(channel_id)
        );

        if (result.empty()) {
            return std::nullopt;
        }

        DiscordChannel channel;
        channel.channel_id = dpp::snowflake(result[0][0].as<int64_t>());
        channel.guild_id = dpp::snowflake(result[0][1].as<int64_t>());
        channel.channel_name = result[0][2].as<std::string>();
        channel.channel_type = result[0][3].as<int>();
        return channel;
    });
}

// ============================================================================
// Download log operations
// ============================================================================

void Database::log_download(int64_t beatmapset_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO download_log (beatmapset_id) VALUES ($1)",
            beatmapset_id
        );
        txn.commit();
    });
}

size_t Database::get_downloads_since(std::chrono::hours period) const {
    return const_cast<Database*>(this)->execute([&](pqxx::connection& conn) -> size_t {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT COUNT(*) FROM download_log WHERE downloaded_at > NOW() - $1 * INTERVAL '1 hour'",
            period.count()
        );
        return result[0][0].as<size_t>();
    });
}

void Database::cleanup_old_downloads() {
    execute([](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec("DELETE FROM download_log WHERE downloaded_at < NOW() - INTERVAL '24 hours'");
        txn.commit();
    });
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

// ============================================================================
// Message crawler operations
// ============================================================================

void Database::store_message(const CrawledMessage& msg) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        auto created_str = std::format("{:%Y-%m-%d %H:%M:%S}",
            std::chrono::floor<std::chrono::seconds>(msg.created_at));

        // Build attachment URLs as PostgreSQL array literal
        std::string attachment_array = "{}";
        if (!msg.attachment_urls.empty()) {
            attachment_array = "{";
            for (size_t i = 0; i < msg.attachment_urls.size(); ++i) {
                if (i > 0) attachment_array += ",";
                // Escape quotes in URL
                std::string escaped = msg.attachment_urls[i];
                size_t pos = 0;
                while ((pos = escaped.find('"', pos)) != std::string::npos) {
                    escaped.replace(pos, 1, "\\\"");
                    pos += 2;
                }
                attachment_array += "\"" + escaped + "\"";
            }
            attachment_array += "}";
        }

        // Optional edited_at
        std::optional<std::string> edited_str;
        if (msg.edited_at) {
            edited_str = std::format("{:%Y-%m-%d %H:%M:%S}",
                std::chrono::floor<std::chrono::seconds>(*msg.edited_at));
        }

        txn.exec_params(
            "INSERT INTO discord_messages (message_id, channel_id, author_id, content, created_at, is_bot, "
            "reply_to_message_id, has_attachments, attachment_urls, edited_at) "
            "VALUES ($1, $2, $3, $4, $5::timestamp, $6, $7, $8, $9::text[], $10::timestamp) "
            "ON CONFLICT (message_id) DO NOTHING",
            static_cast<int64_t>(msg.message_id),
            static_cast<int64_t>(msg.channel_id),
            static_cast<int64_t>(msg.author_id),
            msg.content,
            created_str,
            msg.is_bot,
            msg.reply_to_message_id != 0 ? std::optional(static_cast<int64_t>(msg.reply_to_message_id)) : std::nullopt,
            msg.has_attachments,
            attachment_array,
            edited_str
        );
        txn.commit();
    });
}

void Database::store_messages_batch(const std::vector<CrawledMessage>& messages) {
    if (messages.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& msg : messages) {
            auto created_str = std::format("{:%Y-%m-%d %H:%M:%S}",
                std::chrono::floor<std::chrono::seconds>(msg.created_at));

            // Build attachment URLs as PostgreSQL array literal
            std::string attachment_array = "{}";
            if (!msg.attachment_urls.empty()) {
                attachment_array = "{";
                for (size_t i = 0; i < msg.attachment_urls.size(); ++i) {
                    if (i > 0) attachment_array += ",";
                    std::string escaped = msg.attachment_urls[i];
                    size_t pos = 0;
                    while ((pos = escaped.find('"', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\\"");
                        pos += 2;
                    }
                    attachment_array += "\"" + escaped + "\"";
                }
                attachment_array += "}";
            }

            // Optional edited_at
            std::optional<std::string> edited_str;
            if (msg.edited_at) {
                edited_str = std::format("{:%Y-%m-%d %H:%M:%S}",
                    std::chrono::floor<std::chrono::seconds>(*msg.edited_at));
            }

            txn.exec_params(
                "INSERT INTO discord_messages (message_id, channel_id, author_id, content, created_at, is_bot, "
                "reply_to_message_id, has_attachments, attachment_urls, edited_at) "
                "VALUES ($1, $2, $3, $4, $5::timestamp, $6, $7, $8, $9::text[], $10::timestamp) "
                "ON CONFLICT (message_id) DO NOTHING",
                static_cast<int64_t>(msg.message_id),
                static_cast<int64_t>(msg.channel_id),
                static_cast<int64_t>(msg.author_id),
                msg.content,
                created_str,
                msg.is_bot,
                msg.reply_to_message_id != 0 ? std::optional(static_cast<int64_t>(msg.reply_to_message_id)) : std::nullopt,
                msg.has_attachments,
                attachment_array,
                edited_str
            );
        }
        txn.commit();
        spdlog::debug("[DB] Stored batch of {} messages", messages.size());
    });
}

bool Database::message_exists(dpp::snowflake message_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT 1 FROM discord_messages WHERE message_id = $1",
            static_cast<int64_t>(message_id)
        );
        return !result.empty();
    });
}

void Database::update_message(dpp::snowflake message_id, const std::string& new_content,
                              std::chrono::system_clock::time_point edited_at) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        auto edited_str = std::format("{:%Y-%m-%d %H:%M:%S}",
            std::chrono::floor<std::chrono::seconds>(edited_at));

        txn.exec_params(
            "UPDATE discord_messages SET content = $1, edited_at = $2::timestamp "
            "WHERE message_id = $3",
            new_content,
            edited_str,
            static_cast<int64_t>(message_id)
        );
        txn.commit();
        spdlog::debug("[DB] Updated message {} with edited_at", message_id);
    });
}

size_t Database::get_message_count() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT COUNT(*) FROM discord_messages");
        return result[0][0].as<size_t>();
    });
}

void Database::process_all_messages(std::function<bool(const std::string&)> callback) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec(
            "WITH deduplicated AS ("
            "    SELECT content, created_at,"
            "           LAG(created_at) OVER (PARTITION BY author_id, content ORDER BY created_at) as prev_time"
            "    FROM discord_messages"
            "    WHERE content != '' AND is_bot = false"
            ") "
            "SELECT content FROM deduplicated "
            "WHERE prev_time IS NULL OR created_at - prev_time > INTERVAL '30 seconds'"
        );

        for (const auto& row : result) {
            if (!callback(row[0].as<std::string>())) break;
        }
    });
}

void Database::process_user_messages(dpp::snowflake author_id, std::function<bool(const std::string&)> callback) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "WITH deduplicated AS ("
            "    SELECT content, created_at,"
            "           LAG(created_at) OVER (PARTITION BY content ORDER BY created_at) as prev_time"
            "    FROM discord_messages"
            "    WHERE author_id = $1 AND content != '' AND is_bot = false"
            ") "
            "SELECT content FROM deduplicated "
            "WHERE prev_time IS NULL OR created_at - prev_time > INTERVAL '30 seconds'",
            static_cast<int64_t>(author_id)
        );

        for (const auto& row : result) {
            if (!callback(row[0].as<std::string>())) break;
        }
    });
}

std::vector<MessageAuthor> Database::get_message_authors() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec(
            "SELECT m.author_id, COUNT(*) as msg_count, u.username, u.global_name, u.avatar_hash "
            "FROM discord_messages m "
            "LEFT JOIN discord_users u ON m.author_id = u.user_id "
            "WHERE m.is_bot = false "
            "GROUP BY m.author_id, u.username, u.global_name, u.avatar_hash "
            "ORDER BY msg_count DESC"
        );

        std::vector<MessageAuthor> authors;
        authors.reserve(result.size());

        for (const auto& row : result) {
            MessageAuthor author;
            author.author_id = dpp::snowflake(row[0].as<uint64_t>());
            author.message_count = row[1].as<size_t>();
            author.username = row[2].is_null() ? "" : row[2].as<std::string>();
            author.display_name = row[3].is_null() ? "" : row[3].as<std::string>();
            author.avatar_hash = row[4].is_null() ? "" : row[4].as<std::string>();
            authors.push_back(author);
        }

        return authors;
    });
}

void Database::process_channel_messages(dpp::snowflake channel_id, std::function<bool(const std::string&)> callback) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "WITH deduplicated AS ("
            "    SELECT content, created_at,"
            "           LAG(created_at) OVER (PARTITION BY author_id, content ORDER BY created_at) as prev_time"
            "    FROM discord_messages"
            "    WHERE channel_id = $1 AND content != '' AND is_bot = false"
            ") "
            "SELECT content FROM deduplicated "
            "WHERE prev_time IS NULL OR created_at - prev_time > INTERVAL '30 seconds'",
            static_cast<int64_t>(channel_id)
        );

        for (const auto& row : result) {
            if (!callback(row[0].as<std::string>())) break;
        }
    });
}

void Database::process_user_channel_messages(dpp::snowflake author_id, dpp::snowflake channel_id, std::function<bool(const std::string&)> callback) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "WITH deduplicated AS ("
            "    SELECT content, created_at,"
            "           LAG(created_at) OVER (PARTITION BY content ORDER BY created_at) as prev_time"
            "    FROM discord_messages"
            "    WHERE author_id = $1 AND channel_id = $2 AND content != '' AND is_bot = false"
            ") "
            "SELECT content FROM deduplicated "
            "WHERE prev_time IS NULL OR created_at - prev_time > INTERVAL '30 seconds'",
            static_cast<int64_t>(author_id),
            static_cast<int64_t>(channel_id)
        );

        for (const auto& row : result) {
            if (!callback(row[0].as<std::string>())) break;
        }
    });
}

std::vector<MessageChannel> Database::get_message_channels() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec(
            "SELECT m.channel_id, COUNT(*) as msg_count, COALESCE(c.channel_name, '') as channel_name "
            "FROM discord_messages m "
            "LEFT JOIN discord_channels c ON m.channel_id = c.channel_id "
            "WHERE m.is_bot = false "
            "GROUP BY m.channel_id, c.channel_name "
            "ORDER BY msg_count DESC"
        );

        std::vector<MessageChannel> channels;
        channels.reserve(result.size());

        for (const auto& row : result) {
            MessageChannel channel;
            channel.channel_id = dpp::snowflake(row[0].as<uint64_t>());
            channel.message_count = row[1].as<size_t>();
            channel.channel_name = row[2].as<std::string>();
            channels.push_back(channel);
        }

        return channels;
    });
}

// ============================================================================
// Crawl progress operations
// ============================================================================

void Database::save_crawl_progress(const ChannelCrawlProgress& progress) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        auto last_crawl_str = std::format("{:%Y-%m-%d %H:%M:%S}",
            std::chrono::floor<std::chrono::seconds>(progress.last_crawl));

        txn.exec_params(
            "INSERT INTO crawl_progress (channel_id, guild_id, oldest_message_id, newest_message_id, "
            "total_messages_crawled, last_crawl_at, initial_crawl_complete) "
            "VALUES ($1, $2, $3, $4, $5, $6::timestamp, $7) "
            "ON CONFLICT (channel_id) DO UPDATE SET "
            "oldest_message_id = EXCLUDED.oldest_message_id, "
            "newest_message_id = EXCLUDED.newest_message_id, "
            "total_messages_crawled = EXCLUDED.total_messages_crawled, "
            "last_crawl_at = EXCLUDED.last_crawl_at, "
            "initial_crawl_complete = EXCLUDED.initial_crawl_complete",
            static_cast<int64_t>(progress.channel_id),
            static_cast<int64_t>(progress.guild_id),
            progress.oldest_message_id != 0 ? std::optional(static_cast<int64_t>(progress.oldest_message_id)) : std::nullopt,
            progress.newest_message_id != 0 ? std::optional(static_cast<int64_t>(progress.newest_message_id)) : std::nullopt,
            static_cast<int64_t>(progress.total_messages),
            last_crawl_str,
            progress.initial_crawl_complete
        );
        txn.commit();
        spdlog::debug("[DB] Saved crawl progress for channel {}: {} messages",
                     progress.channel_id, progress.total_messages);
    });
}

std::optional<ChannelCrawlProgress> Database::get_crawl_progress(dpp::snowflake channel_id) {
    return execute([&](pqxx::connection& conn) -> std::optional<ChannelCrawlProgress> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT channel_id, guild_id, oldest_message_id, newest_message_id, "
            "total_messages_crawled, last_crawl_at, initial_crawl_complete "
            "FROM crawl_progress WHERE channel_id = $1",
            static_cast<int64_t>(channel_id)
        );

        if (result.empty()) {
            return std::nullopt;
        }

        const auto& row = result[0];
        ChannelCrawlProgress progress;
        progress.channel_id = dpp::snowflake(row[0].as<uint64_t>());
        progress.guild_id = dpp::snowflake(row[1].as<uint64_t>());
        progress.oldest_message_id = row[2].is_null() ? dpp::snowflake(0) : dpp::snowflake(row[2].as<uint64_t>());
        progress.newest_message_id = row[3].is_null() ? dpp::snowflake(0) : dpp::snowflake(row[3].as<uint64_t>());
        progress.total_messages = row[4].as<size_t>();

        if (!row[5].is_null()) {
            auto ts = row[5].as<std::string>();
            std::tm tm = {};
            std::istringstream ss(ts);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            progress.last_crawl = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        }

        progress.initial_crawl_complete = row[6].as<bool>();
        return progress;
    });
}

std::vector<ChannelCrawlProgress> Database::get_all_crawl_progress(dpp::snowflake guild_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT channel_id, guild_id, oldest_message_id, newest_message_id, "
            "total_messages_crawled, last_crawl_at, initial_crawl_complete "
            "FROM crawl_progress WHERE guild_id = $1 ORDER BY channel_id",
            static_cast<int64_t>(guild_id)
        );

        std::vector<ChannelCrawlProgress> progress_list;
        progress_list.reserve(result.size());

        for (const auto& row : result) {
            ChannelCrawlProgress progress;
            progress.channel_id = dpp::snowflake(row[0].as<uint64_t>());
            progress.guild_id = dpp::snowflake(row[1].as<uint64_t>());
            progress.oldest_message_id = row[2].is_null() ? dpp::snowflake(0) : dpp::snowflake(row[2].as<uint64_t>());
            progress.newest_message_id = row[3].is_null() ? dpp::snowflake(0) : dpp::snowflake(row[3].as<uint64_t>());
            progress.total_messages = row[4].as<size_t>();

            if (!row[5].is_null()) {
                auto ts = row[5].as<std::string>();
                std::tm tm = {};
                std::istringstream ss(ts);
                ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
                progress.last_crawl = std::chrono::system_clock::from_time_t(std::mktime(&tm));
            }

            progress.initial_crawl_complete = row[6].as<bool>();
            progress_list.push_back(progress);
        }

        return progress_list;
    });
}

CrawlStatusSummary Database::get_crawl_status_summary(dpp::snowflake guild_id) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT "
            "COUNT(*) as total_channels, "
            "SUM(CASE WHEN initial_crawl_complete THEN 1 ELSE 0 END) as completed_channels, "
            "COALESCE(SUM(total_messages_crawled), 0) as total_messages "
            "FROM crawl_progress WHERE guild_id = $1",
            static_cast<int64_t>(guild_id)
        );

        CrawlStatusSummary summary;
        if (!result.empty()) {
            const auto& row = result[0];
            summary.total_channels = row[0].as<size_t>();
            summary.completed_channels = row[1].is_null() ? 0 : row[1].as<size_t>();
            summary.total_messages = row[2].as<size_t>();
        }
        return summary;
    });
}

// ============================================================================
// Word statistics operations
// ============================================================================

std::vector<WordStatEntry> Database::get_top_words(size_t limit, const std::string& language, bool exclude_stopwords) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        pqxx::result result;

        // Build query based on filters
        std::string base_query = "SELECT word, count, language FROM word_stats ";
        std::string where_clause;
        std::string order_limit = " ORDER BY count DESC LIMIT ";

        if (exclude_stopwords && !language.empty()) {
            result = txn.exec_params(
                base_query +
                "WHERE language = $1 AND word NOT IN (SELECT word FROM stopwords WHERE language = $1) " +
                order_limit + "$2",
                language, static_cast<int64_t>(limit)
            );
        } else if (exclude_stopwords) {
            result = txn.exec_params(
                base_query +
                "WHERE word NOT IN (SELECT word FROM stopwords) " +
                order_limit + "$1",
                static_cast<int64_t>(limit)
            );
        } else if (!language.empty()) {
            result = txn.exec_params(
                base_query +
                "WHERE language = $1 " +
                order_limit + "$2",
                language, static_cast<int64_t>(limit)
            );
        } else {
            result = txn.exec_params(
                base_query + order_limit + "$1",
                static_cast<int64_t>(limit)
            );
        }

        std::vector<WordStatEntry> entries;
        entries.reserve(result.size());

        for (const auto& row : result) {
            WordStatEntry entry;
            entry.word = row[0].as<std::string>();
            entry.count = row[1].as<size_t>();
            entry.language = row[2].as<std::string>();
            entries.push_back(entry);
        }

        return entries;
    });
}

size_t Database::get_unique_word_count(const std::string& language, bool exclude_stopwords) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        pqxx::result result;

        if (exclude_stopwords && !language.empty()) {
            result = txn.exec_params(
                "SELECT COUNT(*) FROM word_stats "
                "WHERE language = $1 AND word NOT IN (SELECT word FROM stopwords WHERE language = $1)",
                language
            );
        } else if (exclude_stopwords) {
            result = txn.exec(
                "SELECT COUNT(*) FROM word_stats "
                "WHERE word NOT IN (SELECT word FROM stopwords)"
            );
        } else if (!language.empty()) {
            result = txn.exec_params(
                "SELECT COUNT(*) FROM word_stats WHERE language = $1",
                language
            );
        } else {
            result = txn.exec("SELECT COUNT(*) FROM word_stats");
        }

        return result[0][0].as<size_t>();
    });
}

void Database::update_word_stats(const std::vector<std::tuple<std::string, size_t, std::string>>& words) {
    if (words.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& [word, count, language] : words) {
            txn.exec_params(
                "INSERT INTO word_stats (word, count, language, last_updated) "
                "VALUES ($1, $2, $3, CURRENT_TIMESTAMP) "
                "ON CONFLICT (word, language) DO UPDATE SET "
                "count = word_stats.count + EXCLUDED.count, "
                "last_updated = CURRENT_TIMESTAMP",
                word, static_cast<int64_t>(count), language
            );
        }
        txn.commit();
        spdlog::debug("[DB] Updated {} word stats entries", words.size());
    });
}

void Database::clear_word_stats() {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE word_stats");
        txn.commit();
        spdlog::info("[DB] Cleared word stats table");
    });
}

std::unordered_set<std::string> Database::get_stopwords() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT word FROM stopwords");

        std::unordered_set<std::string> stopwords;
        stopwords.reserve(result.size());

        for (const auto& row : result) {
            stopwords.insert(row[0].as<std::string>());
        }

        return stopwords;
    });
}

// ============================================================================
// Phrase statistics operations
// ============================================================================

void Database::update_phrase_stats(const std::vector<std::tuple<
    std::string, std::vector<std::string>, int, size_t, std::string
>>& phrases) {
    if (phrases.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& [phrase, words, word_count, count, language] : phrases) {
            // Convert words vector to PostgreSQL array format
            std::string words_array = "{";
            for (size_t i = 0; i < words.size(); i++) {
                if (i > 0) words_array += ",";
                words_array += "\"" + txn.esc(words[i]) + "\"";
            }
            words_array += "}";

            txn.exec_params(
                "INSERT INTO phrase_stats (phrase, words, word_count, count, language, last_updated) "
                "VALUES ($1, $2::text[], $3, $4, $5, CURRENT_TIMESTAMP) "
                "ON CONFLICT (phrase, language) DO UPDATE SET "
                "count = EXCLUDED.count, "
                "words = EXCLUDED.words, "
                "word_count = EXCLUDED.word_count, "
                "last_updated = CURRENT_TIMESTAMP",
                phrase, words_array, word_count, static_cast<int64_t>(count), language
            );
        }
        txn.commit();
        spdlog::debug("[DB] Updated {} phrase stats entries", phrases.size());
    });
}

void Database::update_phrase_pmi_scores(const std::vector<std::tuple<std::string, std::string, double>>& phrase_pmi) {
    if (phrase_pmi.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& [phrase, language, pmi] : phrase_pmi) {
            txn.exec_params(
                "UPDATE phrase_stats SET pmi_score = $1, last_updated = CURRENT_TIMESTAMP "
                "WHERE phrase = $2 AND language = $3",
                pmi, phrase, language
            );
        }
        txn.commit();
        spdlog::debug("[DB] Updated PMI scores for {} phrases", phrase_pmi.size());
    });
}

void Database::update_phrase_npmi_llr_scores(
    const std::vector<std::tuple<std::string, std::string, double, double>>& scores
) {
    if (scores.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& [phrase, language, npmi, llr] : scores) {
            txn.exec_params(
                "UPDATE phrase_stats SET npmi_score = $1, llr_score = $2, last_updated = CURRENT_TIMESTAMP "
                "WHERE phrase = $3 AND language = $4",
                npmi, llr, phrase, language
            );
        }
        txn.commit();
        spdlog::debug("[DB] Updated NPMI/LLR scores for {} phrases", scores.size());
    });
}

std::vector<PhraseStatEntry> Database::get_top_phrases(
    size_t limit,
    const std::string& language,
    bool sort_by_pmi,
    int word_count_filter,
    size_t min_count
) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        std::string query = "SELECT phrase, words, word_count, count, pmi_score, npmi_score, llr_score, "
                           "trend_score, first_seen, language "
                           "FROM phrase_stats WHERE count >= " + std::to_string(min_count);

        if (!language.empty()) {
            query += " AND language = " + txn.quote(language);
        }

        if (word_count_filter > 0) {
            query += " AND word_count = " + std::to_string(word_count_filter);
        }

        if (sort_by_pmi) {
            query += " ORDER BY pmi_score DESC NULLS LAST";
        } else {
            query += " ORDER BY count DESC";
        }

        query += " LIMIT " + std::to_string(limit);

        auto result = txn.exec(query);

        std::vector<PhraseStatEntry> phrases;
        phrases.reserve(result.size());

        auto now = std::chrono::system_clock::now();
        auto week_ago = now - std::chrono::hours(7 * 24);

        for (const auto& row : result) {
            PhraseStatEntry entry;
            entry.phrase = row["phrase"].as<std::string>();
            entry.word_count = row["word_count"].as<int>();
            entry.count = row["count"].as<size_t>();
            entry.language = row["language"].as<std::string>();

            if (!row["pmi_score"].is_null()) {
                entry.pmi_score = row["pmi_score"].as<double>();
            }
            if (!row["npmi_score"].is_null()) {
                entry.npmi_score = row["npmi_score"].as<double>();
            }
            if (!row["llr_score"].is_null()) {
                entry.llr_score = row["llr_score"].as<double>();
            }
            if (!row["trend_score"].is_null()) {
                entry.trend_score = row["trend_score"].as<double>();
            }
            if (!row["first_seen"].is_null()) {
                auto ts = row["first_seen"].as<std::string>();
                std::tm tm = {};
                std::istringstream ss(ts);
                ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
                entry.first_seen = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                // Check if it's new (appeared in last 7 days)
                if (*entry.first_seen > week_ago) {
                    entry.is_new = true;
                }
            }

            // Parse words array
            std::string words_str = row["words"].as<std::string>();
            // Remove { and }
            if (words_str.size() >= 2) {
                words_str = words_str.substr(1, words_str.size() - 2);
            }
            // Split by comma (simple parsing, assumes no commas in words)
            std::string word;
            for (char c : words_str) {
                if (c == ',') {
                    if (!word.empty()) {
                        // Remove quotes if present
                        if (word.front() == '"') word = word.substr(1);
                        if (!word.empty() && word.back() == '"') word.pop_back();
                        entry.words.push_back(word);
                        word.clear();
                    }
                } else {
                    word += c;
                }
            }
            if (!word.empty()) {
                if (word.front() == '"') word = word.substr(1);
                if (!word.empty() && word.back() == '"') word.pop_back();
                entry.words.push_back(word);
            }

            phrases.push_back(std::move(entry));
        }

        return phrases;
    });
}

size_t Database::get_unique_phrase_count(const std::string& language, int word_count_filter) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        std::string query = "SELECT COUNT(*) FROM phrase_stats WHERE 1=1";

        if (!language.empty()) {
            query += " AND language = " + txn.quote(language);
        }

        if (word_count_filter > 0) {
            query += " AND word_count = " + std::to_string(word_count_filter);
        }

        auto result = txn.exec1(query);
        return result[0].as<size_t>();
    });
}

void Database::clear_phrase_stats() {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE phrase_stats");
        txn.commit();
        spdlog::debug("[DB] Cleared phrase_stats table");
    });
}

std::unordered_map<std::string, size_t> Database::get_global_phrase_frequencies(
    const std::string& language,
    int word_count_filter,
    size_t min_count
) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        std::string query = "SELECT phrase, count FROM phrase_stats WHERE count >= " +
                           std::to_string(min_count);

        if (!language.empty()) {
            query += " AND language = " + txn.quote(language);
        }

        if (word_count_filter > 0) {
            query += " AND word_count = " + std::to_string(word_count_filter);
        }

        auto result = txn.exec(query);

        std::unordered_map<std::string, size_t> frequencies;
        frequencies.reserve(result.size());

        for (const auto& row : result) {
            frequencies[row[0].as<std::string>()] = row[1].as<size_t>();
        }

        return frequencies;
    });
}

// ============================================================================
// Phrase history operations
// ============================================================================

void Database::save_phrase_history_snapshot(
    const std::vector<std::tuple<std::string, std::string, size_t>>& phrases
) {
    if (phrases.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& [phrase, language, count] : phrases) {
            txn.exec_params(
                "INSERT INTO phrase_history (phrase, language, count, recorded_date, recorded_at) "
                "VALUES ($1, $2, $3, CURRENT_DATE, CURRENT_TIMESTAMP) "
                "ON CONFLICT (phrase, language, recorded_date) DO UPDATE SET "
                "count = EXCLUDED.count, recorded_at = EXCLUDED.recorded_at",
                phrase, language, static_cast<int64_t>(count)
            );
        }
        txn.commit();
        spdlog::info("[DB] Saved phrase history snapshot with {} phrases", phrases.size());
    });
}

std::optional<std::chrono::system_clock::time_point> Database::get_last_phrase_snapshot_time() {
    return execute([&](pqxx::connection& conn) -> std::optional<std::chrono::system_clock::time_point> {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT MAX(recorded_at) FROM phrase_history");

        if (result.empty() || result[0][0].is_null()) {
            return std::nullopt;
        }

        auto ts = result[0][0].as<std::string>();
        std::tm tm = {};
        std::istringstream ss(ts);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    });
}

std::unordered_map<std::string, size_t> Database::get_phrase_counts_from_days_ago(int days) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        auto result = txn.exec_params(
            "SELECT phrase, count FROM phrase_history "
            "WHERE recorded_date = (CURRENT_DATE - $1::integer)",
            days
        );

        std::unordered_map<std::string, size_t> counts;
        counts.reserve(result.size());

        for (const auto& row : result) {
            counts[row[0].as<std::string>()] = row[1].as<size_t>();
        }

        return counts;
    });
}

void Database::update_phrase_trend_scores(
    const std::vector<std::tuple<std::string, std::string, double>>& trends
) {
    if (trends.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& [phrase, language, trend_score] : trends) {
            txn.exec_params(
                "UPDATE phrase_stats SET trend_score = $1 WHERE phrase = $2 AND language = $3",
                trend_score, phrase, language
            );
        }
        txn.commit();
        spdlog::debug("[DB] Updated trend scores for {} phrases", trends.size());
    });
}

void Database::update_phrase_first_seen(
    const std::vector<std::tuple<std::string, std::string>>& phrases
) {
    if (phrases.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& [phrase, language] : phrases) {
            txn.exec_params(
                "UPDATE phrase_stats SET first_seen = CURRENT_TIMESTAMP "
                "WHERE phrase = $1 AND language = $2 AND first_seen IS NULL",
                phrase, language
            );
        }
        txn.commit();
        spdlog::debug("[DB] Set first_seen for {} new phrases", phrases.size());
    });
}

// ============================================================================
// Incremental stats processing
// ============================================================================

int64_t Database::get_stats_last_message_id(const std::string& stats_key) {
    return execute([&](pqxx::connection& conn) -> int64_t {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT last_message_id FROM stats_processing_state WHERE key = $1",
            stats_key
        );
        if (result.empty()) {
            return 0;
        }
        return result[0][0].as<int64_t>();
    });
}

void Database::set_stats_last_message_id(const std::string& stats_key, int64_t message_id) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO stats_processing_state (key, last_message_id, last_processed_at) "
            "VALUES ($1, $2, CURRENT_TIMESTAMP) "
            "ON CONFLICT (key) DO UPDATE SET last_message_id = $2, last_processed_at = CURRENT_TIMESTAMP",
            stats_key, message_id
        );
        txn.commit();
    });
}

int64_t Database::process_messages_incremental(
    int64_t last_message_id,
    std::function<bool(int64_t, const std::string&)> callback
) {
    return execute([&](pqxx::connection& conn) -> int64_t {
        pqxx::work txn(conn);
        // Use CTE to filter spam (same author, same content within 30 seconds)
        auto result = txn.exec_params(
            "WITH deduplicated AS ("
            "    SELECT message_id, content, author_id, created_at,"
            "           LAG(created_at) OVER (PARTITION BY author_id, content ORDER BY created_at) as prev_time"
            "    FROM discord_messages"
            "    WHERE message_id > $1 AND content != '' AND is_bot = false"
            ") "
            "SELECT message_id, content FROM deduplicated "
            "WHERE prev_time IS NULL OR created_at - prev_time > INTERVAL '30 seconds' "
            "ORDER BY message_id",
            last_message_id
        );

        int64_t max_id = last_message_id;
        for (const auto& row : result) {
            int64_t msg_id = row[0].as<int64_t>();
            max_id = std::max(max_id, msg_id);
            if (!callback(msg_id, row[1].as<std::string>())) {
                break;
            }
        }
        return max_id;
    });
}

void Database::upsert_word_stats(const std::vector<std::tuple<std::string, size_t, std::string>>& words) {
    if (words.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& [word, count, language] : words) {
            txn.exec_params(
                "INSERT INTO word_stats (word, count, language) VALUES ($1, $2, $3) "
                "ON CONFLICT (word, language) DO UPDATE SET count = word_stats.count + $2",
                word, static_cast<int64_t>(count), language
            );
        }

        txn.commit();
        spdlog::debug("[DB] Upserted {} word stats entries", words.size());
    });
}

void Database::upsert_phrase_stats(const std::vector<std::tuple<
    std::string, std::vector<std::string>, int, size_t, std::string
>>& phrases) {
    if (phrases.empty()) return;

    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);

        for (const auto& [phrase, words, word_count, count, language] : phrases) {
            // Convert words to PostgreSQL array format
            std::string words_array = "{";
            for (size_t i = 0; i < words.size(); i++) {
                if (i > 0) words_array += ",";
                words_array += "\"" + txn.esc(words[i]) + "\"";
            }
            words_array += "}";

            txn.exec_params(
                "INSERT INTO phrase_stats (phrase, words, word_count, count, language) "
                "VALUES ($1, $2::text[], $3, $4, $5) "
                "ON CONFLICT (phrase, language) DO UPDATE SET count = phrase_stats.count + $4",
                phrase, words_array, word_count, static_cast<int64_t>(count), language
            );
        }

        txn.commit();
        spdlog::debug("[DB] Upserted {} phrase stats entries", phrases.size());
    });
}

std::unordered_map<std::string, size_t> Database::get_word_stats_map() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT word, count, language FROM word_stats");

        std::unordered_map<std::string, size_t> word_map;
        word_map.reserve(result.size());

        for (const auto& row : result) {
            std::string key = row[0].as<std::string>() + "_" + row[2].as<std::string>();
            word_map[key] = row[1].as<size_t>();
        }

        return word_map;
    });
}

size_t Database::get_total_word_count() {
    return execute([&](pqxx::connection& conn) -> size_t {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT COALESCE(SUM(count), 0) FROM word_stats");
        return result[0][0].as<size_t>();
    });
}

size_t Database::get_total_bigram_count() {
    return execute([&](pqxx::connection& conn) -> size_t {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT COALESCE(SUM(count), 0) FROM phrase_stats WHERE word_count = 2");
        return result[0][0].as<size_t>();
    });
}

std::vector<Database::PhraseForPMI> Database::get_phrases_for_pmi(size_t min_count) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(
            "SELECT phrase, words, count, language FROM phrase_stats WHERE count >= $1",
            static_cast<int64_t>(min_count)
        );

        std::vector<PhraseForPMI> phrases;
        phrases.reserve(result.size());

        for (const auto& row : result) {
            PhraseForPMI p;
            p.phrase = row[0].as<std::string>();
            p.count = row[2].as<size_t>();
            p.language = row[3].as<std::string>();

            // Parse PostgreSQL array
            std::string words_str = row[1].as<std::string>();
            // Remove braces
            if (words_str.size() >= 2 && words_str.front() == '{' && words_str.back() == '}') {
                words_str = words_str.substr(1, words_str.size() - 2);
            }
            // Split by comma (simple parsing, assumes no commas in words)
            std::stringstream ss(words_str);
            std::string word;
            while (std::getline(ss, word, ',')) {
                // Remove quotes if present
                if (word.size() >= 2 && word.front() == '"' && word.back() == '"') {
                    word = word.substr(1, word.size() - 2);
                }
                if (!word.empty()) {
                    p.words.push_back(word);
                }
            }

            phrases.push_back(std::move(p));
        }

        return phrases;
    });
}

// ============================================================================
// Word blacklist operations
// ============================================================================

std::unordered_set<std::string> Database::get_word_blacklist() {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec("SELECT word FROM word_blacklist");

        std::unordered_set<std::string> blacklist;
        blacklist.reserve(result.size());

        for (const auto& row : result) {
            blacklist.insert(row[0].as<std::string>());
        }

        return blacklist;
    });
}

void Database::add_word_to_blacklist(const std::string& word, const std::string& language) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(
            "INSERT INTO word_blacklist (word, language) VALUES ($1, $2) "
            "ON CONFLICT (word) DO NOTHING",
            word, language
        );
        txn.commit();
        spdlog::info("[DB] Added word '{}' to blacklist", word);
    });
}

void Database::remove_word_from_blacklist(const std::string& word) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params("DELETE FROM word_blacklist WHERE word = $1", word);
        txn.commit();
        spdlog::info("[DB] Removed word '{}' from blacklist", word);
    });
}

// ============================================================================
// Template audit log operations
// ============================================================================

void Database::log_template_change(
    dpp::snowflake admin_id,
    const std::string& admin_username,
    const std::string& action,
    const std::string& command_id,
    const std::string& preset,
    const std::string& old_fields_json,
    const std::string& new_fields_json
) {
    execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        txn.exec_params(R"(
            INSERT INTO template_audit_log
                (discord_id, discord_username, action, command_id, preset, old_fields, new_fields)
            VALUES ($1, $2, $3, $4, $5, $6::jsonb, $7::jsonb)
        )",
            static_cast<int64_t>(admin_id),
            admin_username,
            action,
            command_id,
            preset.empty() ? std::optional<std::string>{} : preset,
            old_fields_json.empty() ? std::optional<std::string>{} : old_fields_json,
            new_fields_json.empty() ? std::optional<std::string>{} : new_fields_json
        );
        txn.commit();
        spdlog::info("[DB] Template audit: {} by {} ({}) - {}:{}",
            action, admin_username, static_cast<uint64_t>(admin_id), command_id, preset);
    });
}

std::vector<TemplateAuditEntry> Database::get_template_audit_log(size_t limit, size_t offset) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(R"(
            SELECT id, discord_id, discord_username, action, command_id, preset,
                   old_fields::text, new_fields::text, created_at
            FROM template_audit_log
            ORDER BY created_at DESC
            LIMIT $1 OFFSET $2
        )", limit, offset);

        std::vector<TemplateAuditEntry> entries;
        entries.reserve(result.size());

        for (const auto& row : result) {
            TemplateAuditEntry entry;
            entry.id = row[0].as<int64_t>();
            entry.discord_id = dpp::snowflake(row[1].as<int64_t>());
            entry.discord_username = row[2].is_null() ? "" : row[2].as<std::string>();
            entry.action = row[3].as<std::string>();
            entry.command_id = row[4].as<std::string>();
            entry.preset = row[5].is_null() ? "" : row[5].as<std::string>();
            entry.old_fields_json = row[6].is_null() ? "" : row[6].as<std::string>();
            entry.new_fields_json = row[7].is_null() ? "" : row[7].as<std::string>();

            auto ts_str = row[8].as<std::string>();
            std::tm tm = {};
            std::istringstream ss(ts_str);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            entry.created_at = std::chrono::system_clock::from_time_t(std::mktime(&tm));

            entries.push_back(std::move(entry));
        }

        return entries;
    });
}

std::vector<TemplateAuditEntry> Database::get_template_audit_log_by_admin(
    dpp::snowflake admin_id,
    size_t limit
) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(R"(
            SELECT id, discord_id, discord_username, action, command_id, preset,
                   old_fields::text, new_fields::text, created_at
            FROM template_audit_log
            WHERE discord_id = $1
            ORDER BY created_at DESC
            LIMIT $2
        )", static_cast<int64_t>(admin_id), limit);

        std::vector<TemplateAuditEntry> entries;
        entries.reserve(result.size());

        for (const auto& row : result) {
            TemplateAuditEntry entry;
            entry.id = row[0].as<int64_t>();
            entry.discord_id = dpp::snowflake(row[1].as<int64_t>());
            entry.discord_username = row[2].is_null() ? "" : row[2].as<std::string>();
            entry.action = row[3].as<std::string>();
            entry.command_id = row[4].as<std::string>();
            entry.preset = row[5].is_null() ? "" : row[5].as<std::string>();
            entry.old_fields_json = row[6].is_null() ? "" : row[6].as<std::string>();
            entry.new_fields_json = row[7].is_null() ? "" : row[7].as<std::string>();

            auto ts_str = row[8].as<std::string>();
            std::tm tm = {};
            std::istringstream ss(ts_str);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            entry.created_at = std::chrono::system_clock::from_time_t(std::mktime(&tm));

            entries.push_back(std::move(entry));
        }

        return entries;
    });
}

std::vector<TemplateAuditEntry> Database::get_template_audit_log_by_command(
    const std::string& command_id,
    size_t limit
) {
    return execute([&](pqxx::connection& conn) {
        pqxx::work txn(conn);
        auto result = txn.exec_params(R"(
            SELECT id, discord_id, discord_username, action, command_id, preset,
                   old_fields::text, new_fields::text, created_at
            FROM template_audit_log
            WHERE command_id = $1
            ORDER BY created_at DESC
            LIMIT $2
        )", command_id, limit);

        std::vector<TemplateAuditEntry> entries;
        entries.reserve(result.size());

        for (const auto& row : result) {
            TemplateAuditEntry entry;
            entry.id = row[0].as<int64_t>();
            entry.discord_id = dpp::snowflake(row[1].as<int64_t>());
            entry.discord_username = row[2].is_null() ? "" : row[2].as<std::string>();
            entry.action = row[3].as<std::string>();
            entry.command_id = row[4].as<std::string>();
            entry.preset = row[5].is_null() ? "" : row[5].as<std::string>();
            entry.old_fields_json = row[6].is_null() ? "" : row[6].as<std::string>();
            entry.new_fields_json = row[7].is_null() ? "" : row[7].as<std::string>();

            auto ts_str = row[8].as<std::string>();
            std::tm tm = {};
            std::istringstream ss(ts_str);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            entry.created_at = std::chrono::system_clock::from_time_t(std::mktime(&tm));

            entries.push_back(std::move(entry));
        }

        return entries;
    });
}

std::optional<TemplateAuditEntry> Database::get_template_audit_entry(int64_t id) {
    return execute([&](pqxx::connection& conn) -> std::optional<TemplateAuditEntry> {
        pqxx::work txn(conn);
        auto result = txn.exec_params(R"(
            SELECT id, discord_id, discord_username, action, command_id, preset,
                   old_fields::text, new_fields::text, created_at
            FROM template_audit_log
            WHERE id = $1
        )", id);

        if (result.empty()) {
            return std::nullopt;
        }

        const auto& row = result[0];
        TemplateAuditEntry entry;
        entry.id = row[0].as<int64_t>();
        entry.discord_id = dpp::snowflake(row[1].as<int64_t>());
        entry.discord_username = row[2].is_null() ? "" : row[2].as<std::string>();
        entry.action = row[3].as<std::string>();
        entry.command_id = row[4].as<std::string>();
        entry.preset = row[5].is_null() ? "" : row[5].as<std::string>();
        entry.old_fields_json = row[6].is_null() ? "" : row[6].as<std::string>();
        entry.new_fields_json = row[7].is_null() ? "" : row[7].as<std::string>();

        auto ts_str = row[8].as<std::string>();
        std::tm tm = {};
        std::istringstream ss(ts_str);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        entry.created_at = std::chrono::system_clock::from_time_t(std::mktime(&tm));

        return entry;
    });
}

} // namespace db
