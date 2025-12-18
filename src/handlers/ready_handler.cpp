#include "handlers/ready_handler.h"
#include "handlers/slash_command_handler.h"
#include "handlers/member_handler.h"
#include <services/user_mapping_service.h>
#include <services/leaderboard_service.h>
#include <services/beatmap_cache_service.h>
#include <database.h>
#include <utils.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>

namespace handlers {

ReadyHandler::ReadyHandler(
    services::UserMappingService& user_mapping_service,
    services::LeaderboardService& leaderboard_service,
    services::BeatmapCacheService* beatmap_cache_service,
    SlashCommandHandler& slash_command_handler,
    MemberHandler& member_handler,
    dpp::cluster& bot
)
    : user_mapping_service_(user_mapping_service)
    , leaderboard_service_(leaderboard_service)
    , beatmap_cache_service_(beatmap_cache_service)
    , slash_command_handler_(slash_command_handler)
    , member_handler_(member_handler)
    , bot_(bot)
{}

void ReadyHandler::handle(const dpp::ready_t& event, bool delete_commands) {
    if (dpp::run_once<struct register_bot_commands>()) {
        slash_command_handler_.register_commands(delete_commands);
    }

    // Read guild and autorole config
    dpp::snowflake guild_id = utils::read_field("GUILD_ID", "config.json");
    dpp::snowflake autorole_id = utils::read_field("AUTOROLE_ID", "config.json");
    member_handler_.set_guild_config(guild_id, autorole_id);

    // Load user mappings from database via service
    user_mapping_service_.load_from_file("");  // Empty path - loads from database

    // Chat map loading is not needed - will be populated on-demand from database
    spdlog::info("Chat map will be populated on-demand from database");

    // Process pending button removals from database (for persistence across restarts)
    process_pending_button_removals();

    // Note: Leaderboard states are now stored in Memcached with automatic expiry
    // No periodic cleanup needed - Memcached handles TTL automatically

    // Set initial presence and start periodic updates
    if (beatmap_cache_service_) {
        auto update_presence = [this]() {
            std::string status = beatmap_cache_service_->get_status_string();
            bot_.set_presence(dpp::presence(dpp::ps_online, dpp::at_watching, status));
        };

        // Initial presence
        update_presence();

        // Update every 60 seconds
        bot_.start_timer([this, update_presence](const dpp::timer&) {
            update_presence();
        }, 60);
    }
}

void ReadyHandler::process_pending_button_removals() {
    spdlog::info("Processing pending button removals from database...");

    try {
        auto& db = db::Database::instance();

        // Get all expired removals and process them immediately
        auto expired = db.get_expired_button_removals();
        if (!expired.empty()) {
            spdlog::info("Found {} expired button removals, processing immediately", expired.size());
            for (const auto& [channel_id, message_id, removal_type] : expired) {
                leaderboard_service_.remove_message_components(channel_id, message_id);
                db.remove_pending_button_removal(channel_id, message_id);
            }
        }

        // Get all future removals and schedule them
        auto pending = db.get_all_pending_removals();
        if (!pending.empty()) {
            auto now = std::chrono::system_clock::now();
            spdlog::info("Found {} pending button removals to schedule", pending.size());

            for (const auto& [channel_id, message_id, expires_at, removal_type] : pending) {
                // Skip if already expired (shouldn't happen, but safety check)
                if (expires_at <= now) {
                    leaderboard_service_.remove_message_components(channel_id, message_id);
                    db.remove_pending_button_removal(channel_id, message_id);
                    continue;
                }

                auto time_until = std::chrono::duration_cast<std::chrono::minutes>(expires_at - now);

                // Schedule removal thread
                std::jthread([this, channel_id, message_id, expires_at]() {
                    auto now_local = std::chrono::system_clock::now();
                    if (expires_at > now_local) {
                        auto wait_duration = std::chrono::duration_cast<std::chrono::milliseconds>(expires_at - now_local);
                        std::this_thread::sleep_for(wait_duration);
                    }

                    leaderboard_service_.remove_message_components(channel_id, message_id);

                    try {
                        auto& db = db::Database::instance();
                        db.remove_pending_button_removal(channel_id, message_id);
                    } catch (const std::exception& e) {
                        spdlog::warn("Failed to remove pending button removal from database: {}", e.what());
                    }
                }).detach();

                spdlog::debug("Scheduled button removal for message {} in {}min", message_id.str(), time_until.count());
            }
        }

        spdlog::info("Finished processing pending button removals");
    } catch (const std::exception& e) {
        spdlog::error("Failed to process pending button removals: {}", e.what());
    }
}

} // namespace handlers
