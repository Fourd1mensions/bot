#pragma once

#include <string>
#include <vector>

namespace migration {

struct MigrationStats {
    size_t users_migrated = 0;
    size_t chat_contexts_migrated = 0;
    size_t beatmap_files_registered = 0;
    bool success = false;
    std::vector<std::string> errors;
};

// Check if migration is needed (JSON files exist)
bool needs_migration();

// Perform full migration from JSON files to PostgreSQL
MigrationStats perform_migration();

// Run SQL schema migrations
void run_sql_migrations();

// Individual migration functions
size_t migrate_users_from_json();
size_t migrate_chat_map_from_json();
size_t register_existing_beatmap_files();

// Backup JSON files after successful migration
void backup_json_files();

} // namespace migration
