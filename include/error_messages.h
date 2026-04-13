#pragma once

#include <string_view>

namespace error_messages {

// Beatmap resolution errors
constexpr std::string_view NO_BEATMAP_IN_CHANNEL =
    "No beatmap found in this channel. Send a beatmap link first, then try again.";

// API errors
constexpr std::string_view API_NO_RESPONSE =
    "❌ osu! API is not responding. Try again in a few seconds.";
constexpr std::string_view API_TIMEOUT_FORMAT =
    "❌ osu! API timeout after {}s. The servers might be busy, please try again later.";

// Download errors
constexpr std::string_view DOWNLOAD_FAILED =
    "❌ Failed to download beatmap. All mirrors are unavailable or the beatmap doesn't exist.";
constexpr std::string_view EXTRACT_FAILED =
    "❌ Failed to extract beatmap files. The .osz file might be corrupted.";
constexpr std::string_view EXTRACT_NOT_FOUND =
    "❌ Beatmap extract not found. Try running the command again.";
constexpr std::string_view NO_BACKGROUND =
    "❌ No background image found in this beatmap's files.";
constexpr std::string_view NO_AUDIO =
    "❌ No audio file found in this beatmap's files.";

// Score errors
constexpr std::string_view NO_RECENT_SCORES =
    "No recent scores found for this user.";
constexpr std::string_view FETCH_SCORES_FAILED =
    "❌ Failed to fetch scores from osu! API.";
constexpr std::string_view PARSE_SCORES_FAILED =
    "❌ Failed to parse scores data. This might be an API issue.";
constexpr std::string_view NO_SCORES_ON_BEATMAP_FORMAT =
    "No scores found on {}";
constexpr std::string_view NO_SCORES_ON_BEATMAP =
    "No scores found for this beatmap";
constexpr std::string_view NO_SCORES_WITH_MODS_FORMAT =
    "No scores found with +{} mods";
constexpr std::string_view SCORE_INDEX_OUT_OF_RANGE_FORMAT =
    "Score index {} out of range (max: {})";

// Parameter validation errors
constexpr std::string_view UNKNOWN_FLAG_FORMAT =
    "Unknown flag `{}`. ";
constexpr std::string_view UNKNOWN_FLAGS_FORMAT =
    "Unknown flags: {}. ";
constexpr std::string_view INVALID_INDEX_FORMAT =
    "Invalid index `{}`. Must be a positive number.";
constexpr std::string_view INVALID_MODE_FORMAT =
    "Invalid mode `{}`. Supported: osu, taiko, catch, mania.";
constexpr std::string_view FLAG_MISSING_VALUE_FORMAT =
    "Flag `{}` requires a value.";

// Command usage hints
constexpr std::string_view RS_USAGE =
    "Usage: `!rs [username] [-i INDEX] [-p] [-b] [-m MODE]`\n"
    "• `-i N` — show score #N (default: 1)\n"
    "• `-p` — passed scores only\n"
    "• `-b` — best scores instead of recent\n"
    "• `-m MODE` — osu, taiko, catch, mania";
constexpr std::string_view LB_USAGE =
    "Usage: `!lb [+MODS] [-s SORT] [BEATMAP_URL]`\n"
    "• `+MODS` — filter by mods (e.g., +HDDT)\n"
    "• `-s SORT` — sort by: pp, score, acc, combo, date";
constexpr std::string_view COMPARE_USAGE =
    "Usage: `!c [username] [+MODS]`\n"
    "• `+MODS` — filter by mods (e.g., +HD)";

} // namespace error_messages
