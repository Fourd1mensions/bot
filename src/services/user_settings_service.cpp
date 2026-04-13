#include "services/user_settings_service.h"
#include <database.h>
#include <spdlog/spdlog.h>

namespace services {

void UserSettingsService::load_from_db() {
    try {
        auto presets = db::Database::instance().get_all_embed_presets();
        std::unique_lock lock(mutex_);
        for (const auto& [id, preset_str] : presets) {
            cache_[id] = embed_preset_from_string(preset_str);
        }
        spdlog::info("[Settings] Loaded {} user presets from DB", cache_.size());
    } catch (const std::exception& e) {
        spdlog::warn("[Settings] Failed to load presets from DB: {}", e.what());
    }
}

EmbedPreset UserSettingsService::get_preset(dpp::snowflake discord_id) const {
    std::shared_lock lock(mutex_);
    auto it = cache_.find(static_cast<uint64_t>(discord_id));
    if (it != cache_.end()) return it->second;
    return EmbedPreset::Classic;
}

void UserSettingsService::set_preset(dpp::snowflake discord_id, EmbedPreset preset) {
    {
        std::unique_lock lock(mutex_);
        cache_[static_cast<uint64_t>(discord_id)] = preset;
    }

    try {
        db::Database::instance().set_embed_preset(discord_id, embed_preset_to_string(preset));
    } catch (const std::exception& e) {
        spdlog::error("[Settings] Failed to save preset to DB: {}", e.what());
    }
}

void UserSettingsService::remove_preset(dpp::snowflake discord_id) {
    {
        std::unique_lock lock(mutex_);
        cache_.erase(static_cast<uint64_t>(discord_id));
    }

    try {
        db::Database::instance().delete_embed_preset(discord_id);
    } catch (const std::exception& e) {
        spdlog::error("[Settings] Failed to delete preset from DB: {}", e.what());
    }
}

} // namespace services
