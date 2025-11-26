#include "migration.h"
#include "database.h"
#include "utils.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <algorithm>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace migration {

bool needs_migration() {
    return fs::exists("users.json") || fs::exists("chat_map.json");
}

size_t migrate_users_from_json() {
    if (!fs::exists("users.json")) {
        spdlog::info("[MIGRATE] users.json not found, skipping");
        return 0;
    }

    try {
        std::ifstream file("users.json");
        if (!file.is_open()) {
            spdlog::error("[MIGRATE] Failed to open users.json");
            return 0;
        }

        json j;
        file >> j;
        file.close();

        auto& db = db::Database::instance();
        size_t count = 0;

        for (auto& [discord_id_str, osu_id_str] : j.items()) {
            try {
                uint64_t discord_id = std::stoull(discord_id_str);
                int64_t osu_id = std::stoll(osu_id_str.get<std::string>());

                db.set_user_mapping(dpp::snowflake(discord_id), osu_id);
                count++;
            } catch (const std::exception& e) {
                spdlog::warn("[MIGRATE] Failed to migrate user mapping {}->{}: {}",
                           discord_id_str, osu_id_str.dump(), e.what());
            }
        }

        spdlog::info("[MIGRATE] Migrated {} user mappings from users.json", count);
        return count;
    } catch (const std::exception& e) {
        spdlog::error("[MIGRATE] Error migrating users: {}", e.what());
        return 0;
    }
}

size_t migrate_chat_map_from_json() {
    if (!fs::exists("chat_map.json")) {
        spdlog::info("[MIGRATE] chat_map.json not found, skipping");
        return 0;
    }

    try {
        std::ifstream file("chat_map.json");
        if (!file.is_open()) {
            spdlog::error("[MIGRATE] Failed to open chat_map.json");
            return 0;
        }

        json j;
        file >> j;
        file.close();

        auto& db = db::Database::instance();
        size_t count = 0;

        for (auto& [channel_id_str, beatmap_id_str] : j.items()) {
            try {
                uint64_t channel_id = std::stoull(channel_id_str);
                std::string beatmap_id = beatmap_id_str.get<std::string>();

                // In the old format, message_id wasn't stored, so we'll use 0
                db.set_chat_context(dpp::snowflake(channel_id), dpp::snowflake(0), beatmap_id);
                count++;
            } catch (const std::exception& e) {
                spdlog::warn("[MIGRATE] Failed to migrate chat context {}->{}: {}",
                           channel_id_str, beatmap_id_str.dump(), e.what());
            }
        }

        spdlog::info("[MIGRATE] Migrated {} chat contexts from chat_map.json", count);
        return count;
    } catch (const std::exception& e) {
        spdlog::error("[MIGRATE] Error migrating chat map: {}", e.what());
        return 0;
    }
}

size_t register_existing_beatmap_files() {
    // NOTE: Old migration for individual audio/background files is no longer needed
    // New system uses .osz file storage, migration not applicable
    spdlog::info("[MIGRATE] Skipping beatmap file registration (not applicable for .osz storage)");
    return 0;
}

void backup_json_files() {
    auto backup_file = [](const std::string& filename) {
        if (fs::exists(filename)) {
            std::string backup_name = filename + ".backup";
            try {
                fs::rename(filename, backup_name);
                spdlog::info("[MIGRATE] Backed up {} to {}", filename, backup_name);
            } catch (const std::exception& e) {
                spdlog::error("[MIGRATE] Failed to backup {}: {}", filename, e.what());
            }
        }
    };

    backup_file("users.json");
    backup_file("chat_map.json");
}

MigrationStats perform_migration() {
    MigrationStats stats;

    spdlog::info("[MIGRATE] Starting migration from JSON to PostgreSQL...");

    try {
        // Migrate users
        stats.users_migrated = migrate_users_from_json();

        // Migrate chat map
        stats.chat_contexts_migrated = migrate_chat_map_from_json();

        // Register existing beatmap files
        stats.beatmap_files_registered = register_existing_beatmap_files();

        // Backup JSON files
        backup_json_files();

        stats.success = true;
        spdlog::info("[MIGRATE] Migration completed successfully:");
        spdlog::info("[MIGRATE]   - {} users migrated", stats.users_migrated);
        spdlog::info("[MIGRATE]   - {} chat contexts migrated", stats.chat_contexts_migrated);
        spdlog::info("[MIGRATE]   - {} beatmapsets registered", stats.beatmap_files_registered);

    } catch (const std::exception& e) {
        stats.success = false;
        stats.errors.push_back(e.what());
        spdlog::error("[MIGRATE] Migration failed: {}", e.what());
    }

    return stats;
}

void run_sql_migrations() {
    try {
        auto& db = db::Database::instance();

        std::cout << "[MIGRATE] Running SQL schema migrations..." << std::endl;
        spdlog::info("[MIGRATE] Running SQL schema migrations...");

        // Create schema_migrations table if it doesn't exist
        db.execute([](pqxx::connection& conn) {
            pqxx::work txn(conn);
            txn.exec(R"(
                CREATE TABLE IF NOT EXISTS schema_migrations (
                    version INTEGER PRIMARY KEY,
                    applied_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
                )
            )");
            txn.commit();
        });

        // Check which migrations have been applied
        auto applied_versions = db.execute([](pqxx::connection& conn) {
            pqxx::work txn(conn);
            auto result = txn.exec("SELECT version FROM schema_migrations ORDER BY version");
            std::set<int> versions;
            for (const auto& row : result) {
                versions.insert(row[0].as<int>());
            }
            return versions;
        });

        std::cout << "[MIGRATE] Found " << applied_versions.size() << " already applied migrations" << std::endl;

        // Find migration files - check multiple possible locations
        fs::path migrations_dir;
        std::vector<fs::path> possible_paths = {
            "migrations",           // Running from project root
            "../migrations",        // Running from build/
            "../../migrations"      // Running from nested directory
        };

        bool found = false;
        for (const auto& path : possible_paths) {
            if (fs::exists(path) && fs::is_directory(path)) {
                migrations_dir = path;
                found = true;
                std::cout << "[MIGRATE] Found migrations directory at: " << fs::absolute(path).string() << std::endl;
                break;
            }
        }

        if (!found) {
            std::cout << "[MIGRATE] migrations/ directory not found in any expected location" << std::endl;
            spdlog::warn("[MIGRATE] migrations/ directory not found");
            return;
        }

        std::vector<std::pair<int, fs::path>> migration_files;
        std::regex pattern(R"(^(\d+)_.*\.sql$)");

        for (const auto& entry : fs::directory_iterator(migrations_dir)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();
            std::smatch matches;

            if (std::regex_match(filename, matches, pattern)) {
                int version = std::stoi(matches[1].str());
                migration_files.emplace_back(version, entry.path());
            }
        }

        // Sort by version
        std::sort(migration_files.begin(), migration_files.end());

        std::cout << "[MIGRATE] Found " << migration_files.size() << " migration files" << std::endl;

        // Apply pending migrations
        for (const auto& [version, path] : migration_files) {
            if (applied_versions.count(version)) {
                std::cout << "[MIGRATE] Migration " << version << " already applied, skipping" << std::endl;
                spdlog::debug("[MIGRATE] Migration {} already applied, skipping", version);
                continue;
            }

            std::cout << "[MIGRATE] Applying migration " << version << ": " << path.filename().string() << std::endl;
            spdlog::info("[MIGRATE] Applying migration {}: {}", version, path.filename().string());

            // Read SQL file
            std::ifstream file(path);
            if (!file.is_open()) {
                std::cout << "[MIGRATE] ERROR: Failed to open migration file: " << path.string() << std::endl;
                spdlog::error("[MIGRATE] Failed to open migration file: {}", path.string());
                continue;
            }

            std::string sql((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
            file.close();

            // Execute migration
            try {
                db.execute([&](pqxx::connection& conn) {
                    pqxx::work txn(conn);
                    txn.exec(sql);
                    txn.exec_params("INSERT INTO schema_migrations (version) VALUES ($1)", version);
                    txn.commit();
                });

                std::cout << "[MIGRATE] Successfully applied migration " << version << std::endl;
                spdlog::info("[MIGRATE] Successfully applied migration {}", version);
            } catch (const std::exception& e) {
                std::cout << "[MIGRATE] ERROR applying migration " << version << ": " << e.what() << std::endl;
                throw;
            }
        }

        std::cout << "[MIGRATE] SQL migrations completed" << std::endl;
        spdlog::info("[MIGRATE] SQL migrations completed");

    } catch (const std::exception& e) {
        std::cout << "[MIGRATE] ERROR running SQL migrations: " << e.what() << std::endl;
        spdlog::error("[MIGRATE] Error running SQL migrations: {}", e.what());
        throw;
    }
}

} // namespace migration
