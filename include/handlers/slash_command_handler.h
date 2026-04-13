#pragma once

#include <dpp/dpp.h>
#include <commands/command_router.h>
#include <string>
#include <vector>

class Request;
class Random;
class Config;
class BeatmapDownloader;

namespace services {
class ChatContextService;
class UserMappingService;
class BeatmapResolverService;
class MessagePresenterService;
}

namespace handlers {

/**
 * Handles Discord slash commands.
 * Routes commands to existing ICommand implementations when possible,
 * and handles slash-only commands directly.
 */
class SlashCommandHandler {
public:
    SlashCommandHandler(
        commands::CommandRouter& command_router,
        Request& request,
        Random& rand,
        const Config& config,
        services::ChatContextService& chat_context_service,
        services::UserMappingService& user_mapping_service,
        services::BeatmapResolverService& beatmap_resolver_service,
        services::MessagePresenterService& message_presenter,
        BeatmapDownloader& beatmap_downloader,
        dpp::cluster& bot
    );

    /**
     * Handle a slash command event.
     * First tries to route to registered ICommand, then falls back to built-in handlers.
     */
    void handle(const dpp::slashcommand_t& event);

    /**
     * Register all slash commands with Discord.
     * Called during bot ready event.
     */
    void register_commands(bool delete_existing = false);

    /**
     * Check if a user is an admin.
     */
    bool is_admin(const std::string& user_id) const;

    /**
     * Get/set autorole state
     */
    bool get_autorole_enabled() const { return autorole_enabled_; }
    void set_autorole_enabled(bool enabled) { autorole_enabled_ = enabled; }

private:
    // Slash-only command handlers
    void handle_gandon(const dpp::slashcommand_t& event);
    void handle_avatar(const dpp::slashcommand_t& event);
    void handle_update_token(const dpp::slashcommand_t& event);
    void handle_score(const dpp::slashcommand_t& event);
    void handle_autorole_switch(const dpp::slashcommand_t& event);
    void handle_weather(const dpp::slashcommand_t& event);

    commands::CommandRouter& command_router_;
    Request& request_;
    Random& rand_;
    const Config& config_;
    services::ChatContextService& chat_context_service_;
    services::UserMappingService& user_mapping_service_;
    services::BeatmapResolverService& beatmap_resolver_service_;
    services::MessagePresenterService& message_presenter_;
    BeatmapDownloader& beatmap_downloader_;
    dpp::cluster& bot_;

    bool autorole_enabled_ = true;
};

} // namespace handlers
