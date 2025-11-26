#include "services/user_mapping_service.h"
#include "database.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>

namespace services {

bool UserMappingService::load_from_file(const std::string& filepath) {
    // Note: We're now loading from database instead of file
    // This method is kept for backward compatibility but delegates to database
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& db = db::Database::instance();
        auto mappings = db.get_all_user_mappings();

        mappings_.clear();
        for (const auto& [discord_id, osu_id] : mappings) {
            mappings_[discord_id] = std::to_string(osu_id);
        }

        spdlog::info("Loaded {} user mappings from database", mappings_.size());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to load user mappings from database: {}", e.what());
        return false;
    }
}

bool UserMappingService::save_to_file(const std::string& filepath) {
    // Note: Mappings are now automatically saved to database via set_mapping()
    // This method is kept for backward compatibility but does nothing
    return true;
}

void UserMappingService::set_mapping(dpp::snowflake discord_id, const std::string& osu_user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    mappings_[discord_id] = osu_user_id;
}

std::optional<std::string> UserMappingService::get_osu_id(dpp::snowflake discord_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mappings_.find(discord_id);
    if (it != mappings_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool UserMappingService::has_mapping(dpp::snowflake discord_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mappings_.find(discord_id) != mappings_.end();
}

bool UserMappingService::remove_mapping(dpp::snowflake discord_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return mappings_.erase(discord_id) > 0;
}

std::unordered_map<dpp::snowflake, std::string> UserMappingService::get_all_mappings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mappings_;
}

} // namespace services
