#include "services/chat_context_service.h"
#include "database.h"
#include <spdlog/spdlog.h>
#include <regex>

namespace services {

void ChatContextService::update_context(const std::string& msg, dpp::snowflake channel_id, dpp::snowflake msg_id) {
    // Prefer explicit beatmap ID if present, otherwise fall back to beatmapset ID
    std::regex set_regex(R"(https:\/\/osu\.ppy\.sh\/beatmapsets\/(\d+)(?:[^ ]*?#(?:osu|taiko|fruits|mania)\/(\d+))?)");
    std::regex beatmap_regex(R"(https:\/\/osu\.ppy\.sh\/(?:beatmaps\/|b\/)(\d+))");
    std::smatch m;

    std::string stored_value;

    if (std::regex_search(msg, m, set_regex)) {
        // m[2] is beatmap id if link contains #mode/<beatmap_id>
        if (m.size() > 2 && m[2].matched) {
            stored_value = m.str(2);
        } else {
            stored_value = "set:" + m.str(1);
        }
    } else if (std::regex_search(msg, m, beatmap_regex)) {
        stored_value = m.str(1);
    }

    if (!stored_value.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& p = chat_map_[channel_id];
        p.first = msg_id;
        p.second = stored_value;

        // Save to database
        try {
            auto& db = db::Database::instance();
            db.set_chat_context(channel_id, msg_id, stored_value);
        } catch (const std::exception& e) {
            spdlog::error("Failed to save chat context to database: {}", e.what());
        }
    }
}

std::string ChatContextService::get_beatmap_id(dpp::snowflake channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check in-memory cache first
    auto it = chat_map_.find(channel_id);
    if (it != chat_map_.end()) {
        return it->second.second;
    }

    // Not in memory - try loading from database
    try {
        auto& db = db::Database::instance();
        auto context = db.get_chat_context(channel_id);
        if (context.has_value()) {
            // Cache it in memory for next time
            const_cast<ChatContextService*>(this)->chat_map_[channel_id] = context.value();
            return context.value().second;
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to load chat context from database: {}", e.what());
    }

    return "";
}

dpp::snowflake ChatContextService::get_message_id(dpp::snowflake channel_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = chat_map_.find(channel_id);
    if (it != chat_map_.end()) {
        return it->second.first;
    }

    // Try loading from database
    try {
        auto& db = db::Database::instance();
        auto context = db.get_chat_context(channel_id);
        if (context.has_value()) {
            const_cast<ChatContextService*>(this)->chat_map_[channel_id] = context.value();
            return context.value().first;
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to load chat context from database: {}", e.what());
    }

    return 0;
}

void ChatContextService::clear_channel(dpp::snowflake channel_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    chat_map_.erase(channel_id);
}

} // namespace services
