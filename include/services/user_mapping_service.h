#pragma once

#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>
#include <dpp/dpp.h>

namespace services {

/**
 * Manages mapping between Discord user IDs and osu! user IDs.
 * Handles loading from and saving to map.json.
 */
class UserMappingService {
public:
    UserMappingService() = default;
    ~UserMappingService() = default;

    // Disable copy and move
    UserMappingService(const UserMappingService&) = delete;
    UserMappingService& operator=(const UserMappingService&) = delete;

    /**
     * Load mappings from map.json file.
     * @param filepath Path to the map.json file
     * @return true if loaded successfully, false otherwise
     */
    bool load_from_file(const std::string& filepath);

    /**
     * Save mappings to map.json file.
     * @param filepath Path to the map.json file
     * @return true if saved successfully, false otherwise
     */
    bool save_to_file(const std::string& filepath);

    /**
     * Add or update a Discord ID to osu! user ID mapping.
     * @param discord_id The Discord user ID
     * @param osu_user_id The osu! user ID
     */
    void set_mapping(dpp::snowflake discord_id, const std::string& osu_user_id);

    /**
     * Get the osu! user ID for a Discord user ID.
     * @param discord_id The Discord user ID
     * @return The osu! user ID, or empty optional if not found
     */
    std::optional<std::string> get_osu_id(dpp::snowflake discord_id) const;

    /**
     * Check if a Discord user has a mapping.
     * @param discord_id The Discord user ID
     * @return true if mapping exists, false otherwise
     */
    bool has_mapping(dpp::snowflake discord_id) const;

    /**
     * Remove a mapping.
     * @param discord_id The Discord user ID to remove
     * @return true if removed, false if not found
     */
    bool remove_mapping(dpp::snowflake discord_id);

    /**
     * Get all mappings (for debugging/admin purposes).
     * @return Copy of all mappings
     */
    std::unordered_map<dpp::snowflake, std::string> get_all_mappings() const;

private:
    // Contains discord_member_id: osu_user_id
    std::unordered_map<dpp::snowflake, std::string> mappings_;
    mutable std::mutex mutex_;
};

} // namespace services
