#pragma once

#include <unordered_map>
#include <string>
#include <optional>
#include <dpp/dpp.h>

namespace services {

/**
 * Manages mapping between Discord user IDs and osu! user IDs.
 * All operations go through PostgreSQL — no in-memory cache.
 */
class UserMappingService {
public:
  UserMappingService()  = default;
  ~UserMappingService() = default;

  UserMappingService(const UserMappingService&)                   = delete;
  UserMappingService&        operator=(const UserMappingService&) = delete;

  bool                       load_from_file(const std::string& filepath);
  bool                       save_to_file(const std::string& filepath);

  void                       set_mapping(dpp::snowflake discord_id, const std::string& osu_user_id);
  std::optional<std::string> get_osu_id(dpp::snowflake discord_id) const;
  bool                       has_mapping(dpp::snowflake discord_id) const;
  bool                       remove_mapping(dpp::snowflake discord_id);
  std::unordered_map<dpp::snowflake, std::string> get_all_mappings() const;
};

} // namespace services
