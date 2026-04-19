#include "services/user_mapping_service.h"
#include "database.h"
#include <spdlog/spdlog.h>

namespace services {

void UserMappingService::set_mapping(dpp::snowflake discord_id, const std::string& osu_user_id) {
  try {
    auto& db = db::Database::instance();
    db.set_user_mapping(discord_id, std::stoll(osu_user_id));
  } catch (const std::exception& e) {
    spdlog::error("[UserMapping] Failed to set mapping: {}", e.what());
  }
}

std::optional<std::string> UserMappingService::get_osu_id(dpp::snowflake discord_id) const {
  try {
    auto& db         = db::Database::instance();
    auto  osu_id_opt = db.get_osu_user_id(discord_id);
    if (osu_id_opt) {
      return std::to_string(*osu_id_opt);
    }
  } catch (const std::exception& e) {
    spdlog::error("[UserMapping] DB lookup failed: {}", e.what());
  }
  return std::nullopt;
}

bool UserMappingService::has_mapping(dpp::snowflake discord_id) const {
  return get_osu_id(discord_id).has_value();
}

bool UserMappingService::remove_mapping(dpp::snowflake discord_id) {
  try {
    auto& db = db::Database::instance();
    return db.remove_user_mapping(discord_id);
  } catch (const std::exception& e) {
    spdlog::error("[UserMapping] Failed to remove mapping: {}", e.what());
    return false;
  }
}

std::unordered_map<dpp::snowflake, std::string> UserMappingService::get_all_mappings() const {
  try {
    auto&                                           db          = db::Database::instance();
    auto                                            db_mappings = db.get_all_user_mappings();

    std::unordered_map<dpp::snowflake, std::string> result;
    for (const auto& [discord_id, osu_id] : db_mappings) {
      result[discord_id] = std::to_string(osu_id);
    }
    return result;
  } catch (const std::exception& e) {
    spdlog::error("[UserMapping] Failed to get mappings from DB: {}", e.what());
    return {};
  }
}

} // namespace services
