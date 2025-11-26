#pragma once

#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>
#include <dpp/dpp.h>

namespace services {

/**
 * Manages chat context mapping between channels, messages, and beatmaps.
 * Tracks the most recent beatmap mentioned in each channel.
 */
class ChatContextService {
public:
    ChatContextService() = default;
    ~ChatContextService() = default;

    // Disable copy and move
    ChatContextService(const ChatContextService&) = delete;
    ChatContextService& operator=(const ChatContextService&) = delete;

    /**
     * Update chat context when a beatmap is mentioned in a message.
     * @param msg The message content
     * @param channel_id The channel where the message was sent
     * @param msg_id The message ID
     */
    void update_context(const std::string& msg, dpp::snowflake channel_id, dpp::snowflake msg_id);

    /**
     * Get the beatmap ID for the most recent beatmap mentioned in a channel.
     * @param channel_id The channel to look up
     * @return The beatmap ID, or empty string if no beatmap found
     */
    std::string get_beatmap_id(dpp::snowflake channel_id) const;

    /**
     * Get the message ID associated with a beatmap in a channel.
     * @param channel_id The channel to look up
     * @return The message ID, or 0 if not found
     */
    dpp::snowflake get_message_id(dpp::snowflake channel_id) const;

    /**
     * Clear context for a specific channel.
     * @param channel_id The channel to clear
     */
    void clear_channel(dpp::snowflake channel_id);

private:
    // Contains channel_id : {message_id : beatmap_id}
    std::unordered_map<dpp::snowflake, std::pair<dpp::snowflake, std::string>> chat_map_;
    mutable std::mutex mutex_;
};

} // namespace services
