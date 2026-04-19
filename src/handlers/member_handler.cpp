#include "handlers/member_handler.h"
#include "handlers/slash_command_handler.h"
#include <services/user_mapping_service.h>
#include <spdlog/spdlog.h>

namespace handlers {

MemberHandler::MemberHandler(services::UserMappingService& user_mapping_service,
                             SlashCommandHandler& slash_command_handler, dpp::cluster& bot) :
    user_mapping_service_(user_mapping_service), slash_command_handler_(slash_command_handler),
    bot_(bot), guild_id_(0), autorole_id_(0) {}

void MemberHandler::set_guild_config(dpp::snowflake guild_id, dpp::snowflake autorole_id) {
  guild_id_    = guild_id;
  autorole_id_ = autorole_id;
}

void MemberHandler::handle_add(const dpp::guild_member_add_t& event) {
  const dpp::user* user = event.added.get_user();
  if (user && !user->is_bot() && slash_command_handler_.get_autorole_enabled()) {
    bot_.guild_member_add_role(guild_id_, user->id, autorole_id_);
  }
}

void MemberHandler::handle_remove(const dpp::guild_member_remove_t& event) {
  spdlog::debug("[Member] User {} left server, keeping osu! link", event.removed.id.str());
}

} // namespace handlers
