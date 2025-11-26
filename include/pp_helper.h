#pragma once

#include <osu-pp/calculator.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <optional>
#include <filesystem>
#include <string>
#include <fstream>
#include <cstdlib>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

namespace pp_helper {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Download .osu file directly from osu! servers
inline std::optional<fs::path> download_osu_file(uint32_t beatmap_id, const fs::path& cache_dir) {
    std::string url = fmt::format("https://osu.ppy.sh/osu/{}", beatmap_id);
    fs::path output_path = cache_dir / fmt::format("{}.osu", beatmap_id);

    // Check if already downloaded
    if (fs::exists(output_path)) {
        spdlog::debug("[PP_HELPER] .osu file already cached: {}", output_path.string());
        return output_path;
    }

    spdlog::info("[PP_HELPER] Downloading .osu file for beatmap_id {} from {}", beatmap_id, url);

    try {
        auto response = cpr::Get(cpr::Url{url});

        if (response.status_code != 200) {
            spdlog::warn("[PP_HELPER] Failed to download .osu file, status {}", response.status_code);
            return std::nullopt;
        }

        // Save to file
        std::ofstream file(output_path);
        if (!file.is_open()) {
            spdlog::error("[PP_HELPER] Failed to open file for writing: {}", output_path.string());
            return std::nullopt;
        }

        file << response.text;
        file.close();

        spdlog::info("[PP_HELPER] Successfully downloaded .osu file to {}", output_path.string());
        return output_path;
    } catch (const std::exception& e) {
        spdlog::error("[PP_HELPER] Failed to download .osu file: {}", e.what());
        return std::nullopt;
    }
}

// Get beatmap info from legacy API
inline std::optional<json> get_beatmap_info_legacy(uint32_t beatmap_id) {
    const char* api_key = std::getenv("OSU_API_KEY");
    if (!api_key || strlen(api_key) == 0) {
        spdlog::debug("[PP_HELPER] OSU_API_KEY not set, skipping legacy API");
        return std::nullopt;
    }

    std::string url = fmt::format("https://osu.ppy.sh/api/get_beatmaps?k={}&b={}", api_key, beatmap_id);

    try {
        auto response = cpr::Get(cpr::Url{url});

        if (response.status_code != 200) {
            spdlog::warn("[PP_HELPER] Legacy API returned status {}", response.status_code);
            return std::nullopt;
        }

        auto j = json::parse(response.text, nullptr, false);
        if (j.is_discarded() || !j.is_array() || j.empty()) {
            spdlog::warn("[PP_HELPER] Failed to parse legacy API response");
            return std::nullopt;
        }

        spdlog::debug("[PP_HELPER] Legacy API returned beatmap info for {}", beatmap_id);
        return j[0]; // Return first beatmap
    } catch (const std::exception& e) {
        spdlog::warn("[PP_HELPER] Legacy API request failed: {}", e.what());
        return std::nullopt;
    }
}

// Find .osu file for specific beatmap_id in extracted directory
inline std::optional<fs::path> find_osu_file(const fs::path& extract_dir, uint32_t beatmap_id) {
    if (!fs::exists(extract_dir) || !fs::is_directory(extract_dir)) {
        spdlog::error("[PP_HELPER] Extract directory does not exist: {}", extract_dir.string());
        return std::nullopt;
    }

    // First try: Download the specific .osu file directly from osu! servers
    auto downloaded_file = download_osu_file(beatmap_id, extract_dir);
    if (downloaded_file.has_value()) {
        spdlog::info("[PP_HELPER] Using directly downloaded .osu file");
        return downloaded_file;
    }

    spdlog::debug("[PP_HELPER] Direct download failed, searching in extracted files for beatmap_id {}", beatmap_id);

    // Look for .osu files and parse to find the correct one
    int files_checked = 0;
    for (const auto& entry : fs::directory_iterator(extract_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".osu") {
            files_checked++;
            spdlog::debug("[PP_HELPER] Checking file: {}", entry.path().filename().string());

            // Parse the .osu file to check BeatmapID
            std::ifstream file(entry.path());
            if (!file.is_open()) {
                spdlog::warn("[PP_HELPER] Failed to open file: {}", entry.path().string());
                continue;
            }

            std::string line;
            bool in_metadata = false;
            bool found_beatmap_id = false;

            while (std::getline(file, line)) {
                // Trim whitespace
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);

                // Check for [Metadata] section
                if (line == "[Metadata]") {
                    in_metadata = true;
                    continue;
                }

                // Check if we left metadata section
                if (in_metadata && line.length() > 0 && line[0] == '[') {
                    break;
                }

                // Look for BeatmapID
                if (in_metadata && line.find("BeatmapID:") == 0) {
                    found_beatmap_id = true;
                    std::string id_str = line.substr(10); // "BeatmapID:"
                    id_str.erase(0, id_str.find_first_not_of(" \t"));

                    try {
                        uint32_t file_beatmap_id = std::stoul(id_str);
                        spdlog::debug("[PP_HELPER] Found BeatmapID: {} in file: {}", file_beatmap_id, entry.path().filename().string());
                        if (file_beatmap_id == beatmap_id) {
                            spdlog::info("[PP_HELPER] Matched beatmap_id {} in file: {}", beatmap_id, entry.path().string());
                            return entry.path();
                        }
                    } catch (...) {
                        spdlog::warn("[PP_HELPER] Failed to parse BeatmapID from line: {}", line);
                        break;
                    }
                    break; // Found BeatmapID line, no need to read more
                }
            }

            if (!found_beatmap_id) {
                spdlog::debug("[PP_HELPER] No BeatmapID found in file: {}", entry.path().filename().string());
            }
        }
    }

    spdlog::warn("[PP_HELPER] Could not find beatmap_id {} after checking {} .osu files", beatmap_id, files_checked);

    // Fallback: Try to get version name from legacy API and search by Version field
    auto beatmap_info = get_beatmap_info_legacy(beatmap_id);
    if (beatmap_info.has_value() && beatmap_info->contains("version")) {
        std::string target_version = (*beatmap_info)["version"].get<std::string>();
        spdlog::info("[PP_HELPER] Using legacy API: searching for version '{}'", target_version);

        for (const auto& entry : fs::directory_iterator(extract_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".osu") {
                std::ifstream file(entry.path());
                if (!file.is_open()) continue;

                std::string line;
                bool in_metadata = false;

                while (std::getline(file, line)) {
                    line.erase(0, line.find_first_not_of(" \t\r\n"));
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);

                    if (line == "[Metadata]") {
                        in_metadata = true;
                        continue;
                    }

                    if (in_metadata && line.length() > 0 && line[0] == '[') {
                        break;
                    }

                    if (in_metadata && line.find("Version:") == 0) {
                        std::string version_str = line.substr(8); // "Version:"
                        version_str.erase(0, version_str.find_first_not_of(" \t"));

                        if (version_str == target_version) {
                            spdlog::info("[PP_HELPER] Found by version name: {}", entry.path().string());
                            return entry.path();
                        }
                        break;
                    }
                }
            }
        }

        spdlog::warn("[PP_HELPER] Could not find version '{}' in any file", target_version);
    }

    // Final fallback: return the first .osu file
    spdlog::info("[PP_HELPER] Using final fallback: returning first .osu file found");
    for (const auto& entry : fs::directory_iterator(extract_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".osu") {
            spdlog::info("[PP_HELPER] Fallback file: {}", entry.path().string());
            return entry.path();
        }
    }

    spdlog::error("[PP_HELPER] No .osu files found in directory at all!");
    return std::nullopt;
}

// Helper to convert mod string to Mods struct
inline osupp::Mods parse_mods(const std::string& mod_string) {
    osupp::Mods mods;

    mods.no_fail = mod_string.find("NF") != std::string::npos;
    mods.easy = mod_string.find("EZ") != std::string::npos;
    mods.touch_device = mod_string.find("TD") != std::string::npos;
    mods.hidden = mod_string.find("HD") != std::string::npos;
    mods.hard_rock = mod_string.find("HR") != std::string::npos;
    mods.sudden_death = mod_string.find("SD") != std::string::npos && mod_string.find("PF") == std::string::npos;
    mods.double_time = mod_string.find("DT") != std::string::npos || mod_string.find("NC") != std::string::npos;
    mods.relax = mod_string.find("RX") != std::string::npos;
    mods.half_time = mod_string.find("HT") != std::string::npos;
    mods.nightcore = mod_string.find("NC") != std::string::npos;
    mods.flashlight = mod_string.find("FL") != std::string::npos;
    mods.auto_play = mod_string.find("AU") != std::string::npos;
    mods.spun_out = mod_string.find("SO") != std::string::npos;
    mods.autopilot = mod_string.find("AP") != std::string::npos;
    mods.perfect = mod_string.find("PF") != std::string::npos;
    mods.blinds = mod_string.find("BL") != std::string::npos;
    mods.traceable = mod_string.find("TC") != std::string::npos;

    return mods;
}

// Calculate PP using full beatmap parsing if .osu file is available
// Falls back to compatibility layer if not available
inline osupp::PerformanceAttributes calculate_performance_with_beatmap(
    const std::string& osu_file_path,
    const osupp::ScoreInfo& score
) {
    // Parse beatmap from .osu file
    auto beatmap = osupp::parse_beatmap(osu_file_path);

    // Calculate performance with full implementation
    return osupp::calculate_performance(beatmap, score);
}

// Calculate FC PP using full beatmap parsing
inline osupp::PerformanceAttributes calculate_fc_performance_with_beatmap(
    const std::string& osu_file_path,
    const osupp::ScoreInfo& score
) {
    // Parse beatmap from .osu file
    auto beatmap = osupp::parse_beatmap(osu_file_path);

    // Calculate FC performance
    return osupp::calculate_fc_performance(beatmap, score);
}

// Get modded difficulty attributes (for display purposes)
inline osupp::BeatmapDifficulty get_modded_difficulty(
    const std::string& osu_file_path,
    const std::string& mod_string
) {
    // Parse beatmap
    auto beatmap = osupp::parse_beatmap(osu_file_path);

    // Parse mods
    auto mods = parse_mods(mod_string);

    // Apply mods to difficulty
    return osupp::apply_mods_to_difficulty(beatmap.difficulty, mods);
}

} // namespace pp_helper
