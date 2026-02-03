#pragma once

#include <atomic>
#include <string>

namespace debug {

// Runtime-toggleable debug flags
struct Settings {
    // Log full request/response for osu! API calls
    std::atomic<bool> verbose_osu_api{false};

    // Log full request/response for rosu-pp gRPC calls
    std::atomic<bool> verbose_rosu_pp{false};

    // Max response body length to log (0 = unlimited)
    std::atomic<size_t> max_response_log_length{2000};

    // Singleton access
    static Settings& instance() {
        static Settings s;
        return s;
    }

    // Helper to truncate response for logging
    static std::string truncate(const std::string& text, size_t max_len) {
        if (max_len == 0 || text.length() <= max_len) {
            return text;
        }
        return text.substr(0, max_len) + "... [truncated, total " + std::to_string(text.length()) + " bytes]";
    }

private:
    Settings() = default;
};

} // namespace debug
