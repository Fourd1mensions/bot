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
    target_guild_id_ = guild_id;

    // Load user mappings from database via service
    user_mapping_service_.load_from_file("");  // Empty path - loads from database

    // Chat map loading is not needed - will be populated on-demand from database
    spdlog::info("Chat map will be populated on-demand from database");

    // Process pending button removals from database (for persistence across restarts)
    process_pending_button_removals();

    // Note: Leaderboard states are now stored in Memcached with automatic expiry
    // No periodic cleanup needed - Memcached handles TTL automatically

    // Set initial presence and start periodic updates (only once)
    if (dpp::run_once<struct setup_presence_timer>()) {
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

    // Sync guild members to cache (initial + periodic refresh every hour, only once)
    if (dpp::run_once<struct setup_sync_timer>()) {
        sync_guild_members();
        bot_.start_timer([this](const dpp::timer&) {
            sync_guild_members();
        }, 3600);  // Every hour
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

void ReadyHandler::sync_guild_members() {
    dpp::snowflake guild_id = utils::read_field("GUILD_ID", "config.json");
    if (guild_id == 0) {
        spdlog::warn("[MemberSync] No GUILD_ID configured, skipping member sync");
        return;
    }

    // Prevent concurrent syncs
    if (sync_in_progress_.exchange(true)) {
        spdlog::warn("[MemberSync] Sync already in progress, skipping");
        return;
    }

    spdlog::info("[MemberSync] Starting guild members sync for guild {}", guild_id.str());

    // Reset sync counter
    sync_member_count_ = 0;

    // Clear discord_users cache before re-syncing
    // This ensures users who left the server are removed from cache
    try {
        db::Database::instance().clear_discord_users();
        spdlog::info("[MemberSync] Cleared discord_users cache");
    } catch (const std::exception& e) {
        spdlog::error("[MemberSync] Failed to clear discord_users cache: {}", e.what());
        sync_in_progress_ = false;
        return;
    }

    // Fetch guild info first
    bot_.guild_get(guild_id, [guild_id](const dpp::confirmation_callback_t& callback) {
        if (callback.is_error()) {
            spdlog::warn("[MemberSync] Failed to get guild info: {}", callback.get_error().message);
            return;
        }

        auto guild = callback.get<dpp::guild>();
        db::GuildInfo gi;
        gi.guild_id = guild.id;
        gi.name = guild.name;
        if (guild.icon.is_iconhash()) {
            gi.icon_hash = guild.icon.as_iconhash().to_string();
        }
        // Don't set member_count here - it will be updated after guild_get_members
        gi.member_count = 0;

        try {
            // Only update if no guild info exists yet, or just update name/icon
            auto existing = db::Database::instance().get_guild_info(guild.id);
            if (existing) {
                gi.member_count = existing->member_count;  // Preserve existing count
            }
            db::Database::instance().cache_guild_info(gi);
            spdlog::info("[MemberSync] Cached guild info: {}", guild.name);
        } catch (const std::exception& e) {
            spdlog::error("[MemberSync] Failed to cache guild info: {}", e.what());
        }
    });

    // Fetch guild members with pagination
    fetch_members_page(guild_id, 0);
}

void ReadyHandler::fetch_members_page(dpp::snowflake guild_id, dpp::snowflake after) {
    bot_.guild_get_members(guild_id, 1000, after, [this, guild_id](const dpp::confirmation_callback_t& callback) {
        if (callback.is_error()) {
            spdlog::error("[MemberSync] Failed to get guild members: {}", callback.get_error().message);
            sync_in_progress_ = false;
            return;
        }

        auto members = callback.get<dpp::guild_member_map>();
        if (members.empty()) {
            // Done fetching, update final count
            size_t synced = sync_member_count_.load();
            try {
                auto guild_info = db::Database::instance().get_guild_info(guild_id);
                if (guild_info) {
                    size_t total = db::Database::instance().get_discord_user_count();
                    guild_info->member_count = total;
                    db::Database::instance().cache_guild_info(*guild_info);
                    spdlog::info("[MemberSync] Completed! Total members in DB: {}, synced this run: {}", total, synced);
                }
            } catch (const std::exception& e) {
                spdlog::error("[MemberSync] Failed to update member count: {}", e.what());
            }

            // Validate tracked users (with safety threshold)
            validate_tracked_users(synced);
            sync_in_progress_ = false;
            return;
        }

        std::vector<db::DiscordUser> users;
        users.reserve(members.size());
        dpp::snowflake last_id = 0;

        for (const auto& [user_id, member] : members) {
            const dpp::user* user = member.get_user();
            if (!user) continue;

            db::DiscordUser du;
            du.user_id = user->id;
            du.username = user->username;
            du.global_name = user->global_name;
            du.avatar_hash = user->avatar.to_string();
            du.is_bot = user->is_bot();
            users.push_back(du);

            if (user->id > last_id) last_id = user->id;
        }

        // Track synced count
        sync_member_count_ += users.size();

        try {
            db::Database::instance().cache_discord_users_batch(users);
            spdlog::info("[MemberSync] Synced {} members (total so far: {})", users.size(), sync_member_count_.load());
        } catch (const std::exception& e) {
            spdlog::error("[MemberSync] Failed to cache members: {}", e.what());
            sync_in_progress_ = false;
            return;
        }

        // Fetch next page if we got a full page
        if (members.size() == 1000) {
            fetch_members_page(guild_id, last_id);
        } else {
            // Last page, update count
            size_t synced = sync_member_count_.load();
            try {
                auto guild_info = db::Database::instance().get_guild_info(guild_id);
                if (guild_info) {
                    size_t total = db::Database::instance().get_discord_user_count();
                    guild_info->member_count = total;
                    db::Database::instance().cache_guild_info(*guild_info);
                    spdlog::info("[MemberSync] Completed! Total members in DB: {}, synced this run: {}", total, synced);
                }
            } catch (const std::exception& e) {
                spdlog::error("[MemberSync] Failed to update member count: {}", e.what());
            }

            // Validate tracked users (with safety threshold)
            validate_tracked_users(synced);
            sync_in_progress_ = false;
        }
    });
}

void ReadyHandler::handle_member_chunk(const dpp::guild_members_chunk_t& event) {
    if (!event.members || !event.adding) return;

    // Only process chunks for our target guild
    if (event.adding->id != target_guild_id_) {
        return;
    }

    std::vector<db::DiscordUser> users;
    users.reserve(event.members->size());

    for (const auto& [user_id, member] : *event.members) {
        const dpp::user* user = member.get_user();
        if (!user) continue;

        db::DiscordUser du;
        du.user_id = user->id;
        du.username = user->username;
        du.global_name = user->global_name;
        du.avatar_hash = user->avatar.to_string();
        du.is_bot = user->is_bot();
        users.push_back(du);
    }

    if (users.empty()) return;

    // Add to sync counter if sync is in progress
    if (sync_in_progress_) {
        sync_member_count_ += users.size();
    }

    try {
        db::Database::instance().cache_discord_users_batch(users);
        spdlog::info("[MemberSync] Received chunk: {} members for guild {} (total: {})",
            users.size(), event.adding->id.str(), sync_member_count_.load());

        // Update member count
        auto guild_info = db::Database::instance().get_guild_info(event.adding->id);
        if (guild_info) {
            size_t total = db::Database::instance().get_discord_user_count();
            guild_info->member_count = total;
            db::Database::instance().cache_guild_info(*guild_info);
        }
    } catch (const std::exception& e) {
        spdlog::error("[MemberSync] Failed to cache member chunk: {}", e.what());
    }
}

void ReadyHandler::validate_tracked_users(size_t synced_count) {
    spdlog::info("[MemberValidation] Starting validation (synced {} members)...", synced_count);

    // Safety check: don't validate if we synced too few members
    // This prevents mass deletion due to API failures
    if (synced_count < MIN_MEMBERS_FOR_VALIDATION) {
        spdlog::warn("[MemberValidation] SKIPPED - only synced {} members (min: {}). "
                     "This may indicate an API issue.",
                     synced_count, MIN_MEMBERS_FOR_VALIDATION);
        return;
    }

    try {
        auto& db = db::Database::instance();

        // Get all tracked user mappings from database
        auto mappings = db.get_all_user_mappings();
        if (mappings.empty()) {
            spdlog::info("[MemberValidation] No tracked users to validate");
            return;
        }

        size_t removed_count = 0;
        for (const auto& [discord_id, osu_id] : mappings) {
            // Check if user exists in discord_users cache (i.e., is on the server)
            auto user_opt = db.get_discord_user(discord_id);
            if (!user_opt) {
                // User is not on the server - remove from tracking
                spdlog::info("[MemberValidation] Removing user {} (osu! {}) - no longer on server",
                    discord_id.str(), osu_id);
                user_mapping_service_.remove_mapping(discord_id);
                removed_count++;
            }
        }

        if (removed_count > 0) {
            spdlog::info("[MemberValidation] Removed {} users who left the server", removed_count);
        } else {
            spdlog::info("[MemberValidation] All {} tracked users are still on the server", mappings.size());
        }
    } catch (const std::exception& e) {
        spdlog::error("[MemberValidation] Failed to validate tracked users: {}", e.what());
    }
}

} // namespace handlers
