#include "services/chat_context_service.h"
#include "database.h"
#include <spdlog/spdlog.h>
#include <regex>

namespace services {

void ChatContextService::set_beatmapset_callback(BeatmapsetCallback callback) {
    beatmapset_callback_ = std::move(callback);
}

void ChatContextService::set_beatmap_callback(BeatmapCallback callback) {
    beatmap_callback_ = std::move(callback);
}

void ChatContextService::update_context(const std::string& raw_event, const std::string& content, dpp::snowflake channel_id, dpp::snowflake msg_id) {
    // Prefer explicit beatmap ID if present, otherwise fall back to beatmapset ID
    std::regex set_regex(R"(https:\/\/osu\.ppy\.sh\/beatmapsets\/(\d+)(?:[^ ]*?#(?:osu|taiko|fruits|mania)\/(\d+))?)");
    std::regex beatmap_regex(R"(https:\/\/osu\.ppy\.sh\/(?:beatmaps\/|b\/)(\d+))");
    std::smatch m;

    std::string stored_value;
    uint32_t beatmapset_id = 0;
    uint32_t beatmap_id_only = 0;  // When we only have beatmap_id (no beatmapset)

    // Helper to search in a string
    auto search_in = [&](const std::string& text, const std::string& source) -> bool {
        if (std::regex_search(text, m, set_regex)) {
            beatmapset_id = std::stoul(m.str(1));
            if (m.size() > 2 && m[2].matched) {
                stored_value = m.str(2);
            } else {
                stored_value = "set:" + m.str(1);
            }
            spdlog::info("[CONTEXT] Found beatmapset {} in {} (stored: {})", beatmapset_id, source, stored_value);
            return true;
        } else if (std::regex_search(text, m, beatmap_regex)) {
            stored_value = m.str(1);
            beatmap_id_only = std::stoul(stored_value);
            spdlog::info("[CONTEXT] Found beatmap {} in {} (beatmap-only URL)", stored_value, source);
            return true;
        }
        return false;
    };

    // First try content (most common), then raw_event (for embeds)
    if (!search_in(content, "content")) {
        search_in(raw_event, "raw_event");
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

    // Trigger callbacks for proactive caching (outside of lock)
    if (beatmapset_id > 0 && beatmapset_callback_) {
        spdlog::info("[CONTEXT] Triggering beatmapset cache callback for {}", beatmapset_id);
        beatmapset_callback_(beatmapset_id);
    } else if (beatmap_id_only > 0 && beatmap_callback_) {
        spdlog::info("[CONTEXT] Triggering beatmap cache callback for {}", beatmap_id_only);
        beatmap_callback_(beatmap_id_only);
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
