#pragma once

#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <dpp/dpp.h>

namespace services {

enum class EmbedPreset { Compact, Classic, Extended, Custom };

inline std::string embed_preset_to_string(EmbedPreset preset) {
    switch (preset) {
        case EmbedPreset::Compact: return "compact";
        case EmbedPreset::Classic: return "classic";
        case EmbedPreset::Extended: return "extended";
        case EmbedPreset::Custom: return "custom";
        default: return "classic";
    }
}

inline EmbedPreset embed_preset_from_string(const std::string& str) {
    if (str == "compact") return EmbedPreset::Compact;
    if (str == "extended") return EmbedPreset::Extended;
    if (str == "custom") return EmbedPreset::Custom;
    return EmbedPreset::Classic;
}

class UserSettingsService {
public:
    UserSettingsService() = default;

    void load_from_db();

    EmbedPreset get_preset(dpp::snowflake discord_id) const;
    void set_preset(dpp::snowflake discord_id, EmbedPreset preset);
    void remove_preset(dpp::snowflake discord_id);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<uint64_t, EmbedPreset> cache_;
};

} // namespace services
