#include "services/embed_template_service.h"
#include <database.h>
#include <dpp/dpp.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <algorithm>
#include <charconv>
#include <set>
#include <cctype>

namespace services {

// ---------------------------------------------------------------------------
// Command registry — defines fields, defaults, and placeholders per command
// ---------------------------------------------------------------------------

static CommandTemplateConfig make_rs_config() {
    CommandTemplateConfig c;
    c.command_id = "rs";
    c.label = "Recent Score (!rs)";
    c.has_presets = true;
    c.field_names = {"content", "title", "description", "beatmap_info", "footer", "footer_icon", "color"};

    c.placeholders = {
        {"title", "Song title"}, {"artist", "Artist name"}, {"version", "Difficulty name"},
        {"creator", "Mapper name"}, {"status", "Ranked/Loved/etc"},
        {"beatmap_id", "Beatmap ID"}, {"beatmapset_id", "Beatmapset ID"},
        {"beatmap_url", "Beatmap URL"}, {"image_url", "Cover image URL"}, {"thumbnail_url", "Thumbnail URL"},
        {"mode", "Game mode (osu/taiko/fruits/mania)"}, {"gamemode_string", "Game mode name (osu! Standard, Taiko...)"},
        {"sr", "Star rating (modded)"}, {"sr_raw", "Star rating (original)"},
        {"mods", "Raw mod string"}, {"mods_suffix", "+HDDT or empty if NM"},
        {"rank", "Rank emoji"}, {"rank_raw", "Rank letter (S, A, F...)"},
        {"rank_line", "\"▸ rank (completion%)\" — composite, legacy"}, {"completion", "Completion % or empty if passed"},
        {"passed", "true/false"},
        {"pp", "Current PP (2 decimals)"}, {"pp_int", "Current PP (rounded)"},
        {"fc_pp", "FC PP (2 decimals)"}, {"fc_pp_int", "FC PP (rounded)"},
        {"fc_acc", "FC accuracy %"}, {"fc_line", "(fc_pp PP for fc_acc% FC) or empty"},
        {"aim_pp", "Aim PP"}, {"speed_pp", "Speed PP"}, {"acc_pp", "Accuracy PP"},
        {"acc", "Accuracy %"}, {"combo", "Player combo"}, {"max_combo", "Map max combo"},
        {"score", "Total score (formatted)"}, {"score_raw", "Total score (number)"},
        {"300", "300 count"}, {"100", "100 count"}, {"50", "50 count"}, {"miss", "Miss count"},
        {"bpm", "BPM (modded, int)"}, {"bpm_raw", "BPM (original, float)"},
        {"length", "Length mm:ss (modded)"}, {"length_raw", "Length in seconds (original)"},
        {"ar", "Approach Rate"}, {"od", "Overall Difficulty"}, {"cs", "Circle Size"}, {"hp", "HP Drain"},
        {"aim_diff", "Aim difficulty"}, {"speed_diff", "Speed difficulty"},
        {"total_objects", "Total hit objects"}, {"max_combo_diff", "Max combo from difficulty calc"},
        {"weight_pct", "Weight % in top scores"}, {"weight_pp", "Weighted PP value"},
        {"page", "Current score # (1-based)"}, {"total", "Total scores count"},
        {"username", "Player username"}, {"user_id", "Player user ID"},
        {"date", "Score date (ISO8601)"}, {"date_unix", "Unix timestamp"},
        {"date_relative", "Discord relative time <t:N:R>"}, {"score_type", "Recent/Best"},
        {"try_number", "Try counter (0 if not tracked)"}, {"try_line", "\"Try #N\" or empty"},
        {"map_rank", "Global position on map leaderboard or empty"},
        {"pb_pp", "Personal best PP on this map (2 decimals) or empty"},
        {"pb_pp_int", "Personal best PP on this map (rounded) or empty"},
        {"sr_color", "Star rating color (hex like #ff66aa)"},
        {"rank_color", "Rank-based color (SS=gold, S=silver, etc.)"}
    };

    // --- compact ---
    TemplateFields compact;
    compact["content"] = "{try_line}";
    compact["title"] = "{title} [{version}] {sr}\xe2\x98\x85 {mods_suffix}";
    compact["description"] =
        "\xe2\x96\xb8 {rank} {completion} \xe2\x96\xb8 **{pp}PP** \xe2\x96\xb8 {acc}%\n"
        "\xe2\x96\xb8 {score} \xe2\x96\xb8 **x{combo}/{max_combo}** \xe2\x96\xb8 [{300}/{100}/{50}/{miss}]";
    compact["beatmap_info"] = "";
    compact["footer"] = "{status} map by {creator}";
    compact["footer_icon"] = "";
    compact["color"] = "{rank_color}";
    c.defaults["compact"] = compact;

    // --- classic ---
    TemplateFields classic;
    classic["content"] = "{try_line}";
    classic["title"] = "{title} [{version}] {sr}\xe2\x98\x85 {mods_suffix}";
    classic["description"] =
        "\xe2\x96\xb8 {rank} {completion} \xe2\x96\xb8 **{pp}PP** {fc_line} \xe2\x96\xb8 {acc}%\n"
        "\xe2\x96\xb8 {score} \xe2\x96\xb8 **x{combo}/{max_combo}** \xe2\x96\xb8 [{300}/{100}/{50}/{miss}]";
    classic["beatmap_info"] = "\xe2\x96\xb8 {bpm} BPM \xe2\x80\xa2 {length} \xe2\x96\xb8 AR `{ar}` OD `{od}` CS `{cs}` HP `{hp}`";
    classic["footer"] = "{score_type} score";
    classic["footer_icon"] = "";
    classic["color"] = "{rank_color}";
    c.defaults["classic"] = classic;

    // --- extended ---
    TemplateFields extended;
    extended["content"] = "{try_line}";
    extended["title"] = "{title} [{version}] {sr}\xe2\x98\x85 {mods_suffix}";
    extended["description"] =
        "\xe2\x96\xb8 {rank} {completion} \xe2\x96\xb8 **{pp}PP** {fc_line} \xe2\x96\xb8 {acc}%\n"
        "\xe2\x96\xb8 {score} \xe2\x96\xb8 **x{combo}/{max_combo}** \xe2\x96\xb8 [{300}/{100}/{50}/{miss}]\n"
        "\xe2\x96\xb8 Aim: **{aim_pp}**pp \xe2\x80\xa2 Speed: **{speed_pp}**pp \xe2\x80\xa2 Acc: **{acc_pp}**pp";
    extended["beatmap_info"] = "\xe2\x96\xb8 {bpm} BPM \xe2\x80\xa2 {length} \xe2\x96\xb8 AR `{ar}` OD `{od}` CS `{cs}` HP `{hp}`";
    extended["footer"] = "{score_type} score";
    extended["footer_icon"] = "";
    extended["color"] = "{rank_color}";
    c.defaults["extended"] = extended;

    return c;
}

static CommandTemplateConfig make_compare_config() {
    CommandTemplateConfig c;
    c.command_id = "compare";
    c.label = "Compare (!c)";
    c.has_presets = true;
    c.field_names = {"color", "title", "header", "field_name", "field_value", "footer"};

    c.placeholders = {
        // Color (hex #rrggbb or named: viola_purple, osu_pink, star_rating, etc.)
        {"color", "Embed color: #rrggbb or named (viola_purple, osu_pink)"},
        // Common (title/description/footer)
        {"title", "Song title"}, {"artist", "Artist name"}, {"version", "Difficulty name"},
        {"sr", "Star rating"}, {"status", "Ranked/Loved/etc"}, {"creator", "Mapper name"},
        {"mods_suffix", "+HDDT or empty if NM"}, {"beatmap_url", "Beatmap URL"},
        {"thumbnail_url", "Thumbnail URL"}, {"image_url", "Cover image URL"},
        {"beatmap_id", "Beatmap ID"}, {"beatmapset_id", "Beatmapset ID"},
        {"mode", "Game mode (osu/taiko/fruits/mania)"}, {"gamemode_string", "Game mode name"},
        {"bpm", "BPM"}, {"length", "Length mm:ss"}, {"max_combo", "Map max combo"},
        {"ar", "Approach Rate"}, {"od", "Overall Difficulty"}, {"cs", "Circle Size"}, {"hp", "HP Drain"},
        {"username", "Player username"}, {"score_count", "Total scores found"},
        {"page", "Current page"}, {"total", "Total pages"},
        // Per-score fields (each score becomes one embed field)
        {"index", "Score position (1-based)"}, {"rank", "Rank emoji"}, {"rank_raw", "Rank letter"},
        {"mods", "Score mods"}, {"mods_suffix_score", "+HD or empty"},
        {"pp", "PP value (2 decimals)"}, {"pp_int", "PP value (rounded)"},
        {"acc", "Accuracy %"},
        {"combo", "Player combo"},
        {"score", "Total score (formatted)"}, {"score_raw", "Total score (number)"},
        {"300", "300 count"}, {"100", "100 count"}, {"50", "50 count"}, {"miss", "Miss count"},
        {"passed", "true/false"}, {"failed_line", "\" • **FAILED**\" or empty"},
        {"date", "Score date (ISO8601)"}, {"date_unix", "Unix timestamp"},
        {"date_relative", "Discord relative time <t:N:R>"},
        {"user_id", "Player user ID"},
        {"weight_pct", "Weight % in top scores"}, {"weight_pp", "Weighted PP value"}
    };

    for (const auto& preset : {"compact", "classic", "extended"}) {
        TemplateFields f;
        f["color"] = "viola_purple";
        f["title"] = "{title} [{version}] {mods_suffix}";
        f["header"] = "**{username}**'s scores ({score_count} found)";
        f["field_name"] = "**#{index}** {rank} **+{mods}** \xe2\x80\xa2 **{pp}pp**";
        f["field_value"] = "{acc}% \xe2\x80\xa2 x{combo}/{max_combo} \xe2\x80\xa2 [{300}/{100}/{50}/{miss}]{failed_line}";
        f["footer"] = "Page {page}/{total}";
        c.defaults[preset] = f;
    }

    return c;
}

static CommandTemplateConfig make_map_config() {
    CommandTemplateConfig c;
    c.command_id = "map";
    c.label = "Map Info (!m)";
    c.has_presets = true;
    c.field_names = {"title", "description",
                     "pp_field_name", "pp_field",
                     "difficulty_field_name", "difficulty_field",
                     "map_info_field_name", "map_info_field",
                     "download_field_name", "download_field",
                     "media_field_name", "media_field"};

    c.placeholders = {
        {"title", "Song title"}, {"artist", "Artist name"}, {"version", "Difficulty name"},
        {"sr", "Star rating"}, {"status", "Ranked/Loved/etc"},
        {"mode", "Game mode"}, {"gamemode_string", "Game mode name"},
        {"mods_suffix", "+HDDT or empty if NM"},
        {"ar", "Approach Rate"}, {"od", "Overall Difficulty"}, {"cs", "Circle Size"}, {"hp", "HP Drain"},
        {"aim_diff", "Aim difficulty"}, {"speed_diff", "Speed difficulty"}, {"max_combo", "Max combo"},
        {"bpm", "BPM (modded)"}, {"length", "Length mm:ss (modded)"},
        {"pp_90", "PP at 90%"}, {"pp_95", "PP at 95%"}, {"pp_99", "PP at 99%"}, {"pp_100", "PP at 100%"},
        {"beatmapset_id", "Beatmapset ID"}, {"beatmap_url", "Beatmap URL"}, {"image_url", "Cover image URL"}
    };

    std::string dl = "[osu!direct](https://osu.ppy.sh/d/{beatmapset_id}) \xe2\x80\xa2 "
                     "[Kana](https://kana.nisemonic.net/osu/d/{beatmapset_id}) \xe2\x80\xa2 "
                     "[Nerinyan](https://api.nerinyan.moe/d/{beatmapset_id}) \xe2\x80\xa2 "
                     "[Catboy](https://catboy.best/d/{beatmapset_id})";

    std::string media = "[Background](https://kana.nisemonic.net/osu/bg/{beatmapset_id}) \xe2\x80\xa2 "
                        "[Audio](https://kana.nisemonic.net/osu/audio/{beatmapset_id})";

    for (const auto& preset : {"compact", "classic", "extended"}) {
        TemplateFields f;
        f["title"] = "{title} [{version}] {mods_suffix}";
        f["description"] = ":star: **{sr}\xe2\x98\x85** \xe2\x80\xa2 {status}";
        f["pp_field_name"] = "Performance Points";
        f["pp_field"] = "**90%** \xe2\x80\x94 {pp_90}pp\n**95%** \xe2\x80\x94 {pp_95}pp\n**99%** \xe2\x80\x94 {pp_99}pp\n**100%** \xe2\x80\x94 {pp_100}pp";
        f["difficulty_field_name"] = "Difficulty";
        f["difficulty_field"] = "**AR** {ar} \xe2\x80\xa2 **OD** {od}\n**CS** {cs} \xe2\x80\xa2 **HP** {hp}\n**Aim** {aim_diff}\xe2\x98\x85 \xe2\x80\xa2 **Speed** {speed_diff}\xe2\x98\x85\n**Max Combo:** {max_combo}x";
        f["map_info_field_name"] = "Map Info";
        f["map_info_field"] = "**BPM:** {bpm}\n**Length:** {length}";
        f["download_field_name"] = "Download";
        f["download_field"] = dl;
        f["media_field_name"] = "Media";
        f["media_field"] = media;
        c.defaults[preset] = f;
    }

    return c;
}

static CommandTemplateConfig make_leaderboard_config() {
    CommandTemplateConfig c;
    c.command_id = "leaderboard";
    c.label = "Leaderboard (!lb)";
    c.has_presets = false;
    c.field_names = {"color", "field_name", "field_value", "footer"};

    c.placeholders = {
        // Color (hex #rrggbb or named: viola_purple, osu_pink, star_rating, etc.)
        {"color", "Embed color: #rrggbb or named (viola_purple, osu_pink)"},
        // Common values
        {"title", "Beatmap title"}, {"mods_filter", "Mods filter or empty"},
        {"page", "Current page"}, {"total_pages", "Total pages"},
        {"shown", "Scores shown on page"}, {"total_scores", "Total scores"},
        {"filter", "\" • Filter: +HD\" or empty"}, {"filter_mods", "Raw mods string or empty"},
        {"sort", "\" • sorted by score\" or empty"}, {"sort_method", "Raw sort method or empty"},
        {"caller_rank", "\" • your rank: #N\" or empty"}, {"caller_rank_num", "Rank number or empty"},
        // Per-score fields (each score becomes one embed field)
        {"rank", "Position in leaderboard"}, {"username", "Player username"}, {"user_id", "Player user ID"},
        {"pp", "PP value"}, {"mods", "Score mods"}, {"rank_emoji", "Rank emoji"},
        {"acc", "Accuracy %"}, {"combo", "Player combo"}, {"max_combo", "Map max combo"},
        {"score", "Total score"}, {"300", "300 count"}, {"100", "100 count"}, {"50", "50 count"}, {"miss", "Miss count"},
        {"date", "Score date (relative)"}
    };

    TemplateFields f;
    f["color"] = "viola_purple";
    f["field_name"] = "{rank}) {username} `{pp}pp` +{mods}";
    f["field_value"] = "**\xe2\x96\xb8**{rank_emoji}({acc}%) \xe2\x80\xa2 {score} \xe2\x80\xa2 **x{combo}/{max_combo}** \xe2\x80\xa2 [{300}/{100}/{50}/{miss}]\n**\xe2\x96\xb8** Score set {date}";
    f["footer"] = "Page {page}/{total_pages} \xe2\x80\xa2 {shown}/{total_scores} scores shown{filter}{sort}{caller_rank}";
    c.defaults["default"] = f;

    return c;
}

static CommandTemplateConfig make_sim_config() {
    CommandTemplateConfig c;
    c.command_id = "sim";
    c.label = "Simulate (!sim)";
    c.has_presets = false;
    c.field_names = {"body"};

    c.placeholders = {
        {"title", "Beatmap title"}, {"mode", "Game mode"}, {"gamemode_string", "Game mode name"},
        {"sr", "Star rating"}, {"acc", "Accuracy %"}, {"mods", "Mods"},
        {"combo", "Combo (0 if not set)"}, {"max_combo", "Max combo"},
        {"count_100", "100 count or empty"}, {"count_50", "50 count or empty"}, {"misses", "Miss count or empty"},
        {"ratio", "Mania ratio or empty"},
        {"pp", "Total PP"}, {"aim_pp", "Aim PP"}, {"speed_pp", "Speed PP"}, {"accuracy_pp", "Accuracy PP"},
        {"aim_diff", "Aim difficulty"}, {"speed_diff", "Speed difficulty"},
        {"hit_counts_line", "Pre-formatted hit counts line"},
        {"combo_line", "Pre-formatted combo line"},
        {"mode_line", "\"[MODE]\" or empty if osu"},
        {"ratio_line", "Ratio line or empty"}
    };

    TemplateFields f;
    f["body"] =
        "**Simulated Play on {title}**{mode_line}\n"
        ":star: **{sr}\xe2\x98\x85**\n\n"
        "**Score Parameters:**\n"
        "\xe2\x80\xa2 Accuracy: **{acc}%**\n"
        "{hit_counts_line}"
        "\xe2\x80\xa2 Mods: **{mods}**\n"
        "{combo_line}"
        "{ratio_line}"
        "\n"
        "**Performance:**\n"
        "\xe2\x80\xa2 **{pp}pp** total\n"
        "\xe2\x80\xa2 Aim: **{aim_pp}pp**\n"
        "\xe2\x80\xa2 Speed: **{speed_pp}pp**\n"
        "\xe2\x80\xa2 Accuracy: **{accuracy_pp}pp**\n\n"
        "**Difficulty:**\n"
        "\xe2\x80\xa2 Aim: **{aim_diff}\xe2\x98\x85**\n"
        "\xe2\x80\xa2 Speed: **{speed_diff}\xe2\x98\x85**";
    c.defaults["default"] = f;

    return c;
}

static CommandTemplateConfig make_osc_config() {
    CommandTemplateConfig c;
    c.command_id = "osc";
    c.label = "Score Counts (!osc)";
    c.has_presets = false;
    c.field_names = {"title", "description"};

    c.placeholders = {
        {"username", "Player username"},
        {"mode", "Game mode (osu!, taiko, etc.)"},
        {"top1", "Top 1 count"},
        {"top8", "Top 8 count"},
        {"top15", "Top 15 count"},
        {"top25", "Top 25 count"},
        {"top50", "Top 50 count"},
        {"top100", "Top 100 count"}
    };

    TemplateFields f;
    f["title"] = "In how many top X {mode}map leaderboards is {username}?";
    f["description"] =
        "```\n"
        "Top 1  : {top1}\n"
        "Top 8  : {top8}\n"
        "Top 15 : {top15}\n"
        "Top 25 : {top25}\n"
        "Top 50 : {top50}\n"
        "Top 100: {top100}\n"
        "```";
    c.defaults["default"] = f;

    return c;
}

static CommandTemplateConfig make_profile_config() {
    CommandTemplateConfig c;
    c.command_id = "profile";
    c.label = "Profile (!osu)";
    c.has_presets = false;
    c.field_names = {"description", "footer"};

    c.placeholders = {
        {"username", "Player username"},
        {"pp", "Total PP"},
        {"global_rank", "Global rank"},
        {"country_code", "Country code"},
        {"country_rank", "Country rank"},
        {"accuracy", "Hit accuracy %"},
        {"level", "Level (e.g. 101)"},
        {"level_progress", "Level progress (0-99)"},
        {"playcount", "Play count"},
        {"playtime_hours", "Playtime in hours"},
        {"medal_count", "Medal count"},
        {"peak_rank", "Peak rank"},
        {"peak_date", "Peak rank date (Unix timestamp)"},
        {"mode", "Game mode"},
        {"join_duration", "Pre-formatted join duration (e.g. '10 years 5 months')"},
        {"join_date_unix", "Join date Unix timestamp"}
    };

    TemplateFields f;
    f["description"] =
        "Accuracy: `{accuracy}%` \xe2\x80\xa2 Level: `{level}.{level_progress}`\n"
        "Playcount: `{playcount}` (`{playtime_hours} hrs`)\n"
        "Medals: `{medal_count}`"
        "{?peak_rank}\nPeak rank: `#{peak_rank}` (<t:{peak_date}:d>){/peak_rank}";
    f["footer"] = "{?join_duration}Joined osu! {join_duration} ago{/join_duration}";
    c.defaults["default"] = f;

    return c;
}

static CommandTemplateConfig make_top_config() {
    CommandTemplateConfig c;
    c.command_id = "top";
    c.label = "Top Scores (!top)";
    c.has_presets = false;
    c.field_names = {"color", "field_name", "field_value", "footer"};

    c.placeholders = {
        // Color
        {"color", "Embed color: #rrggbb or named (viola_purple, osu_pink)"},
        // Common values
        {"username", "Player username"}, {"user_id", "Player user ID"},
        {"mode", "Game mode"}, {"gamemode_string", "Game mode name"},
        {"total_scores", "Total scores after filtering"},
        {"page", "Current page"}, {"total_pages", "Total pages"},
        {"shown", "Scores shown on page"},
        {"filter", "Active filters summary or empty"},
        {"mods_filter", "Mods filter or empty"}, {"grade_filter", "Grade filter or empty"},
        {"sort", "Sort method"}, {"reverse", "Reversed or empty"},
        // Per-score fields (each score becomes one embed field)
        {"index", "Original position (1-100)"}, {"position", "Position on current page"},
        {"title", "Song title"}, {"artist", "Artist name"}, {"version", "Difficulty name"},
        {"beatmap_id", "Beatmap ID"}, {"beatmapset_id", "Beatmapset ID"},
        {"beatmap_url", "Beatmap URL"},
        {"rank", "Rank emoji"}, {"rank_raw", "Rank letter"},
        {"pp", "PP value"}, {"pp_int", "PP value (rounded)"},
        {"weight_pct", "Weight % (100, 95, 90.25...)"}, {"weight_pp", "Weighted PP"},
        {"mods", "Score mods"}, {"mods_suffix", "+HD or empty"},
        {"acc", "Accuracy %"},
        {"combo", "Player combo"}, {"max_combo", "Map max combo"},
        {"score", "Total score"}, {"score_raw", "Total score (number)"},
        {"300", "300 count"}, {"100", "100 count"}, {"50", "50 count"}, {"miss", "Miss count"},
        {"date", "Score date (relative)"}, {"date_raw", "Score date (ISO8601)"}
    };

    TemplateFields f;
    f["color"] = "viola_purple";
    f["field_name"] = "**#{index}** {title} [{version}]";
    f["field_value"] = "**\xe2\x96\xb8** {rank} **{pp}pp** ({weight_pp}pp) \xe2\x80\xa2 {acc}% **+{mods}**\n**\xe2\x96\xb8** x{combo}/{max_combo} \xe2\x80\xa2 [{300}/{100}/{50}/{miss}] \xe2\x80\xa2 {date}";
    f["footer"] = "Page {page}/{total_pages} \xe2\x80\xa2 {shown}/{total_scores} scores{filter}";
    c.defaults["default"] = f;

    return c;
}

std::vector<CommandTemplateConfig> EmbedTemplateService::get_all_commands() {
    return {
        make_rs_config(),
        make_compare_config(),
        make_map_config(),
        make_leaderboard_config(),
        make_sim_config(),
        make_osc_config(),
        make_profile_config(),
        make_top_config()
    };
}

// ---------------------------------------------------------------------------
// Key helpers
// ---------------------------------------------------------------------------

static std::string make_key(const std::string& command_id, const std::string& preset) {
    if (preset.empty() || preset == "default") return command_id;
    return command_id + ":" + preset;
}

TemplateFields EmbedTemplateService::get_default_fields(const std::string& key) {
    // key is "rs:compact", "leaderboard", etc.
    auto commands = get_all_commands();
    for (const auto& cmd : commands) {
        if (cmd.has_presets) {
            for (const auto& preset : {"compact", "classic", "extended"}) {
                if (key == make_key(cmd.command_id, preset)) {
                    auto it = cmd.defaults.find(preset);
                    return (it != cmd.defaults.end()) ? it->second : TemplateFields{};
                }
            }
        } else {
            if (key == cmd.command_id) {
                auto it = cmd.defaults.find("default");
                return (it != cmd.defaults.end()) ? it->second : TemplateFields{};
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// DB operations
// ---------------------------------------------------------------------------

void EmbedTemplateService::load_from_db() {
    try {
        auto& db = db::Database::instance();

        // Load legacy field-based templates
        auto rows = db.get_all_preset_templates();

        std::unique_lock lock(mutex_);
        cache_.clear();
        json_cache_.clear();

        // Load legacy templates from DB
        for (const auto& [preset_name, field_name, tmpl_text] : rows) {
            cache_[preset_name][field_name] = tmpl_text;
        }

        // Load JSON templates from DB
        auto json_rows = db.get_all_json_templates();
        for (const auto& [key, json_str] : json_rows) {
            json_cache_[key] = json_str;
        }

        // Seed any missing commands/presets with defaults
        seed_all_defaults();

        size_t total_fields = 0;
        for (const auto& [k, v] : cache_) total_fields += v.size();
        spdlog::info("[Templates] Loaded {} fields across {} legacy template sets, {} JSON templates",
            total_fields, cache_.size(), json_cache_.size());

    } catch (const std::exception& e) {
        spdlog::warn("[Templates] Failed to load templates from DB: {}", e.what());
    }
}

void EmbedTemplateService::seed_all_defaults() {
    auto& db = db::Database::instance();
    auto commands = get_all_commands();

    for (const auto& cmd : commands) {
        for (const auto& [preset, defaults] : cmd.defaults) {
            std::string key = make_key(cmd.command_id, preset);
            auto& cached = cache_[key]; // creates if missing

            for (const auto& [field, default_val] : defaults) {
                if (cached.find(field) == cached.end()) {
                    cached[field] = default_val;
                    db.set_preset_template(key, field, default_val);
                    spdlog::info("[Templates] Seeded '{}'.'{}'", key, field);
                }
            }
        }
    }
}

TemplateFields EmbedTemplateService::get_fields(const std::string& key) const {
    std::shared_lock lock(mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    return get_default_fields(key);
}

void EmbedTemplateService::set_fields(const std::string& key, const TemplateFields& fields) {
    // Write to DB first — if this fails, cache stays unchanged
    auto& db = db::Database::instance();
    for (const auto& [field_name, tmpl_text] : fields) {
        db.set_preset_template(key, field_name, tmpl_text);
    }

    // Only update cache after successful DB write
    std::unique_lock lock(mutex_);
    auto& cached = cache_[key];
    for (const auto& [field_name, tmpl_text] : fields) {
        cached[field_name] = tmpl_text;
    }
}

void EmbedTemplateService::reset_to_default(const std::string& key) {
    auto defaults = get_default_fields(key);
    if (defaults.empty()) {
        spdlog::warn("[Templates] No defaults found for key '{}'", key);
        return;
    }

    // Write to DB first — delete old rows, then write defaults
    auto& db = db::Database::instance();
    db.delete_preset_templates(key);
    for (const auto& [field_name, tmpl_text] : defaults) {
        db.set_preset_template(key, field_name, tmpl_text);
    }

    // Only update cache after successful DB write
    std::unique_lock lock(mutex_);
    cache_[key] = defaults;
}

std::unordered_map<std::string, TemplateFields> EmbedTemplateService::get_all() const {
    std::shared_lock lock(mutex_);
    return cache_;
}

// ---------------------------------------------------------------------------
// Legacy !rs wrappers
// ---------------------------------------------------------------------------

EmbedTemplate EmbedTemplateService::get_template(const std::string& preset_name) const {
    auto fields = get_fields("rs:" + preset_name);
    EmbedTemplate tmpl;
    tmpl.content = fields.count("content") ? fields.at("content") : "";
    tmpl.title = fields.count("title") ? fields.at("title") : "";
    tmpl.description = fields.count("description") ? fields.at("description") : "";
    tmpl.beatmap_info = fields.count("beatmap_info") ? fields.at("beatmap_info") : "";
    tmpl.footer = fields.count("footer") ? fields.at("footer") : "";
    tmpl.footer_icon = fields.count("footer_icon") ? fields.at("footer_icon") : "";
    tmpl.color = fields.count("color") ? fields.at("color") : "{rank_color}";
    return tmpl;
}

void EmbedTemplateService::set_template(const std::string& preset_name, const EmbedTemplate& tmpl) {
    TemplateFields fields;
    fields["content"] = tmpl.content;
    fields["title"] = tmpl.title;
    fields["description"] = tmpl.description;
    fields["beatmap_info"] = tmpl.beatmap_info;
    fields["footer"] = tmpl.footer;
    fields["footer_icon"] = tmpl.footer_icon;
    fields["color"] = tmpl.color;
    set_fields("rs:" + preset_name, fields);
}

void EmbedTemplateService::reset_to_default_legacy(const std::string& preset_name) {
    reset_to_default("rs:" + preset_name);
}

// ---------------------------------------------------------------------------
// User custom template lookup
// ---------------------------------------------------------------------------

// Map registry command IDs (internal) to API command IDs (user-facing/DB keys)
static std::string to_api_command_id(const std::string& internal_id) {
    static const std::unordered_map<std::string, std::string> REVERSE_ALIASES = {
        {"leaderboard", "lb"},
    };
    auto it = REVERSE_ALIASES.find(internal_id);
    return (it != REVERSE_ALIASES.end()) ? it->second : internal_id;
}

TemplateFields EmbedTemplateService::get_user_fields(
    dpp::snowflake discord_id,
    const std::string& command_id,
    const std::string& preset_name) const {

    // If preset is "custom", try to get user's custom template
    if (preset_name == "custom") {
        try {
            auto& db_inst = db::Database::instance();
            // Try both the given command_id and its API alias (e.g. "leaderboard" -> "lb")
            std::string api_id = to_api_command_id(command_id);
            auto custom_json = db_inst.get_user_custom_template(discord_id, api_id);
            if (!custom_json && api_id != command_id) {
                custom_json = db_inst.get_user_custom_template(discord_id, command_id);
            }

            if (custom_json) {
                // Parse JSON and extract fields
                auto j = nlohmann::json::parse(*custom_json);
                TemplateFields fields;

                // Check if this is a flat format (direct field -> value mapping)
                // Flat format: { "content": "...", "title": "...", "description": "...", ... }
                bool is_flat_format = j.is_object() && !j.contains("embed");

                if (is_flat_format) {
                    // Direct flat format from frontend
                    for (auto& [key, value] : j.items()) {
                        if (value.is_string()) {
                            fields[key] = value.get<std::string>();
                        }
                    }
                } else {
                    // Nested format: { "content": "...", "embed": { "title": "...", ... } }

                    // Extract content
                    if (j.contains("content") && j["content"].is_string()) {
                        fields["content"] = j["content"].get<std::string>();
                    }

                    // Extract embed fields
                    if (j.contains("embed") && j["embed"].is_object()) {
                        const auto& embed = j["embed"];
                        if (embed.contains("title") && embed["title"].is_string()) {
                            fields["title"] = embed["title"].get<std::string>();
                        }
                        if (embed.contains("description") && embed["description"].is_string()) {
                            fields["description"] = embed["description"].get<std::string>();
                        }
                        if (embed.contains("footer") && embed["footer"].is_object()) {
                            if (embed["footer"].contains("text") && embed["footer"]["text"].is_string()) {
                                fields["footer"] = embed["footer"]["text"].get<std::string>();
                            }
                        }
                        // For rs command, also extract beatmap_info if present
                        if (embed.contains("fields") && embed["fields"].is_array()) {
                            for (const auto& field : embed["fields"]) {
                                if (field.contains("name") && field["name"].is_string() &&
                                    field.contains("value") && field["value"].is_string()) {
                                    std::string name = field["name"].get<std::string>();
                                    if (name.find("Map") != std::string::npos || name.find("Info") != std::string::npos) {
                                        fields["beatmap_info"] = field["value"].get<std::string>();
                                    }
                                }
                            }
                        }
                    }
                }

                if (!fields.empty()) {
                    return fields;
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("[EmbedTemplateService] Failed to load custom template for user {}: {}",
                discord_id.str(), e.what());
        }

        // Fall back to classic if no custom template or parse error
        return get_fields(command_id + ":classic");
    }

    // For non-custom presets, use the standard lookup
    // Check if command has presets
    auto commands = get_all_commands();
    for (const auto& cmd : commands) {
        if (cmd.command_id == command_id) {
            if (cmd.has_presets) {
                return get_fields(command_id + ":" + preset_name);
            } else {
                return get_fields(command_id);
            }
        }
    }

    // Fallback
    return get_fields(command_id + ":" + preset_name);
}

EmbedTemplate EmbedTemplateService::get_user_template(
    dpp::snowflake discord_id,
    const std::string& preset_name) const {

    auto fields = get_user_fields(discord_id, "rs", preset_name);

    EmbedTemplate tmpl;
    tmpl.content = fields.count("content") ? fields.at("content") : "";
    tmpl.title = fields.count("title") ? fields.at("title") : "";
    tmpl.description = fields.count("description") ? fields.at("description") : "";
    tmpl.beatmap_info = fields.count("beatmap_info") ? fields.at("beatmap_info") : "";
    tmpl.footer = fields.count("footer") ? fields.at("footer") : "";
    tmpl.footer_icon = fields.count("footer_icon") ? fields.at("footer_icon") : "";
    tmpl.color = fields.count("color") ? fields.at("color") : "{rank_color}";
    return tmpl;
}

// ---------------------------------------------------------------------------
// Template rendering engine
// ---------------------------------------------------------------------------

namespace {

enum class CondOp { NON_EMPTY, EQ, NEQ, GT, LT, GTE, LTE, IN };

struct ParsedCondition {
    std::string key;
    CondOp op = CondOp::NON_EMPTY;
    std::string compare_value;
    std::vector<std::string> compare_values;  // For IN operator
    bool negated = false;
};

ParsedCondition parse_condition(const std::string& cond_str, bool negated) {
    ParsedCondition c;
    c.negated = negated;

    // Check for array syntax: key=[val1,val2,val3]
    size_t eq_pos = cond_str.find('=');
    if (eq_pos != std::string::npos && eq_pos > 0 && eq_pos + 1 < cond_str.size()) {
        // Make sure it's not != operator
        if (eq_pos == 0 || cond_str[eq_pos - 1] != '!') {
            std::string after_eq = cond_str.substr(eq_pos + 1);
            if (!after_eq.empty() && after_eq.front() == '[' && after_eq.back() == ']') {
                // Parse array: [val1,val2,val3]
                c.key = cond_str.substr(0, eq_pos);
                c.op = CondOp::IN;
                std::string inner = after_eq.substr(1, after_eq.size() - 2);
                // Split by comma
                size_t start = 0;
                size_t comma_pos;
                while ((comma_pos = inner.find(',', start)) != std::string::npos) {
                    c.compare_values.push_back(inner.substr(start, comma_pos - start));
                    start = comma_pos + 1;
                }
                if (start < inner.size()) {
                    c.compare_values.push_back(inner.substr(start));
                }
                return c;
            }
        }
    }

    // Check two-char operators first, then single-char
    static const struct { const char* token; size_t len; CondOp op; } ops[] = {
        {"!=", 2, CondOp::NEQ}, {">=", 2, CondOp::GTE}, {"<=", 2, CondOp::LTE},
        {"=",  1, CondOp::EQ},  {">",  1, CondOp::GT},  {"<",  1, CondOp::LT},
    };

    for (const auto& o : ops) {
        size_t p = cond_str.find(o.token);
        if (p != std::string::npos && p > 0) {
            c.key = cond_str.substr(0, p);
            c.op = o.op;
            c.compare_value = cond_str.substr(p + o.len);
            return c;
        }
    }

    // No operator — simple non-empty / empty check
    c.key = cond_str;
    c.op = CondOp::NON_EMPTY;
    return c;
}

bool evaluate_condition(const ParsedCondition& cond,
                        const std::unordered_map<std::string, std::string>& values) {
    auto it = values.find(cond.key);
    std::string val = (it != values.end()) ? it->second : "";

    bool result = false;
    switch (cond.op) {
        case CondOp::NON_EMPTY:
            result = !val.empty();
            break;
        case CondOp::EQ:
            result = (val == cond.compare_value);
            break;
        case CondOp::NEQ:
            result = (val != cond.compare_value);
            break;
        case CondOp::IN:
            // Check if val is in the array of compare_values
            result = std::find(cond.compare_values.begin(), cond.compare_values.end(), val)
                     != cond.compare_values.end();
            break;
        case CondOp::GT:
        case CondOp::LT:
        case CondOp::GTE:
        case CondOp::LTE: {
            // Numeric comparison — parse both sides as double
            char* end_a = nullptr;
            char* end_b = nullptr;
            double a = std::strtod(val.c_str(), &end_a);
            double b = std::strtod(cond.compare_value.c_str(), &end_b);
            // If either side fails to parse, condition is false
            if (end_a == val.c_str() || end_b == cond.compare_value.c_str()) {
                result = false;
            } else {
                switch (cond.op) {
                    case CondOp::GT:  result = (a > b);  break;
                    case CondOp::LT:  result = (a < b);  break;
                    case CondOp::GTE: result = (a >= b); break;
                    case CondOp::LTE: result = (a <= b); break;
                    default: break;
                }
            }
            break;
        }
    }

    return cond.negated ? !result : result;
}

// Process loop blocks: {#array}...{.field}...{/array}
// Returns the template with all loops expanded
std::string process_loops(const std::string& tmpl,
                          const std::unordered_map<std::string, std::string>& /* values */,
                          const TemplateArrays& arrays) {
    // Check if there are any loop tags at all
    if (tmpl.find("{#") == std::string::npos) {
        return tmpl;
    }

    std::string result;
    result.reserve(tmpl.size() * 2);

    size_t pos = 0;
    while (pos < tmpl.size()) {
        // Find next loop opening {#
        size_t loop_open = tmpl.find("{#", pos);

        if (loop_open == std::string::npos) {
            result.append(tmpl, pos);
            break;
        }

        // Append text before loop
        result.append(tmpl, pos, loop_open - pos);

        // Find array name
        size_t name_end = tmpl.find('}', loop_open + 2);
        if (name_end == std::string::npos) {
            result.append(tmpl, loop_open);
            break;
        }

        std::string array_name(tmpl, loop_open + 2, name_end - loop_open - 2);

        // Skip special loop variables (#index, #position)
        if (array_name == "index" || array_name == "position") {
            // This is a loop variable reference, not a loop start
            // Keep it as-is, will be replaced during iteration
            result.append(tmpl, loop_open, name_end - loop_open + 1);
            pos = name_end + 1;
            continue;
        }

        // Find matching closing tag {/array}
        std::string end_tag = "{/" + array_name + "}";
        size_t loop_close = tmpl.find(end_tag, name_end + 1);

        if (loop_close == std::string::npos) {
            // No matching end tag — treat as literal
            result.append(tmpl, loop_open, name_end - loop_open + 1);
            pos = name_end + 1;
            continue;
        }

        // Extract loop body
        std::string loop_body(tmpl, name_end + 1, loop_close - name_end - 1);

        // Find the array in arrays map
        auto arr_it = arrays.find(array_name);
        if (arr_it != arrays.end() && !arr_it->second.empty()) {
            size_t index = 0;
            for (const auto& item : arr_it->second) {
                // For each item, create a merged values map
                std::string item_result = loop_body;

                // Replace {.field} with item values
                size_t dot_pos = 0;
                while ((dot_pos = item_result.find("{.", dot_pos)) != std::string::npos) {
                    size_t dot_close = item_result.find('}', dot_pos + 2);
                    if (dot_close == std::string::npos) break;

                    std::string field_name(item_result, dot_pos + 2, dot_close - dot_pos - 2);
                    auto field_it = item.find(field_name);
                    if (field_it != item.end()) {
                        item_result.replace(dot_pos, dot_close - dot_pos + 1, field_it->second);
                        dot_pos += field_it->second.size();
                    } else {
                        // Field not found, leave as-is or replace with empty
                        item_result.replace(dot_pos, dot_close - dot_pos + 1, "");
                    }
                }

                // Replace {#index} and {#position}
                size_t idx_pos;
                while ((idx_pos = item_result.find("{#index}")) != std::string::npos) {
                    item_result.replace(idx_pos, 8, std::to_string(index));
                }
                while ((idx_pos = item_result.find("{#position}")) != std::string::npos) {
                    item_result.replace(idx_pos, 11, std::to_string(index + 1));
                }

                result.append(item_result);
                ++index;
            }
        }
        // If array not found or empty, loop produces no output

        pos = loop_close + end_tag.size();
    }

    return result;
}

} // anonymous namespace

std::string render_template(const std::string& tmpl,
                            const std::unordered_map<std::string, std::string>& values,
                            const TemplateArrays& arrays) {
    // Pass 0: Process loop blocks {#array}...{/array}
    std::string with_loops = process_loops(tmpl, values, arrays);

    // Pass 1: Process conditional blocks
    // Supports: {?key}, {!key}, {?key=val}, {?key!=val}, {?key>val}, {?key<val},
    //           {?key>=val}, {?key<=val}, {?key=[val1,val2,val3]} (array membership),
    //           with optional {:key} else clause, closed by {/key}
    std::string processed;
    processed.reserve(with_loops.size());

    size_t pos = 0;
    while (pos < with_loops.size()) {
        // Find next {? or {!
        size_t cond_q = with_loops.find("{?", pos);
        size_t cond_n = with_loops.find("{!", pos);
        size_t cond_open = std::min(cond_q, cond_n);

        if (cond_open == std::string::npos) {
            processed.append(with_loops, pos);
            break;
        }

        processed.append(with_loops, pos, cond_open - pos);

        bool negated = (with_loops[cond_open + 1] == '!');
        size_t cond_close = with_loops.find('}', cond_open + 2);
        if (cond_close == std::string::npos) {
            processed.append(with_loops, cond_open);
            break;
        }

        std::string cond_str(with_loops, cond_open + 2, cond_close - cond_open - 2);
        auto cond = parse_condition(cond_str, negated);

        std::string end_tag = "{/" + cond.key + "}";
        size_t block_end = with_loops.find(end_tag, cond_close + 1);

        if (block_end == std::string::npos) {
            // No matching end tag — treat as literal text
            processed.append(with_loops, cond_open, cond_close - cond_open + 1);
            pos = cond_close + 1;
            continue;
        }

        // Look for optional else tag {:key} between opening and end
        std::string else_tag = "{:" + cond.key + "}";
        size_t else_pos = with_loops.find(else_tag, cond_close + 1);
        // Only valid if else_tag is before end_tag
        if (else_pos != std::string::npos && else_pos >= block_end) {
            else_pos = std::string::npos;
        }

        bool condition_result = evaluate_condition(cond, values);

        if (else_pos != std::string::npos) {
            // Has else branch
            if (condition_result) {
                processed.append(with_loops, cond_close + 1, else_pos - cond_close - 1);
            } else {
                processed.append(with_loops, else_pos + else_tag.size(), block_end - else_pos - else_tag.size());
            }
        } else {
            // No else branch
            if (condition_result) {
                processed.append(with_loops, cond_close + 1, block_end - cond_close - 1);
            }
        }

        pos = block_end + end_tag.size();
    }

    // Pass 2: Replace regular {key} placeholders
    std::string result;
    result.reserve(processed.size() * 2);

    pos = 0;
    while (pos < processed.size()) {
        size_t open = processed.find('{', pos);
        if (open == std::string::npos) {
            result.append(processed, pos);
            break;
        }

        result.append(processed, pos, open - pos);

        size_t close = processed.find('}', open + 1);
        if (close == std::string::npos) {
            result.append(processed, open);
            break;
        }

        std::string key(processed, open + 1, close - open - 1);
        auto it = values.find(key);
        if (it != values.end()) {
            result.append(it->second);
        } else {
            result.append(processed, open, close - open + 1);
        }

        pos = close + 1;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Template validation engine
// ---------------------------------------------------------------------------

namespace {

std::string extract_key_from_condition(const std::string& cond_str) {
    static const struct { const char* token; size_t len; } ops[] = {
        {"!=", 2}, {">=", 2}, {"<=", 2}, {"=", 1}, {">", 1}, {"<", 1},
    };
    for (const auto& o : ops) {
        size_t p = cond_str.find(o.token);
        if (p != std::string::npos && p > 0) {
            return cond_str.substr(0, p);
        }
    }
    return cond_str;
}

// Discord embed field length limits
size_t discord_field_limit(const std::string& field_name) {
    if (field_name == "content") return 2000;  // message content
    if (field_name == "title") return 256;     // embed title / author name
    if (field_name == "description" || field_name == "body") return 4096;
    if (field_name == "footer") return 2048;
    // field names (score_header, pp_field_name, etc.)
    if (field_name.find("_name") != std::string::npos) return 256;
    return 1024;
}

} // anonymous namespace

std::vector<ValidationIssue> EmbedTemplateService::validate_template(
    const std::string& field_name,
    const std::string& tmpl,
    const std::vector<PlaceholderInfo>& known_placeholders) {

    std::vector<ValidationIssue> issues;
    if (tmpl.empty()) return issues;

    // --- ERRORS ---

    // 1. Length check
    if (tmpl.size() > 4000) {
        issues.push_back({
            ValidationIssue::Level::Error, field_name,
            fmt::format("Field exceeds 4000 character limit ({} chars)", tmpl.size()),
            4000
        });
    }

    // 2. Security: @everyone, @here (case-insensitive)
    {
        std::string lower = tmpl;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        auto check_mention = [&](const std::string& pattern, const std::string& msg) {
            size_t pos = 0;
            while ((pos = lower.find(pattern, pos)) != std::string::npos) {
                issues.push_back({ValidationIssue::Level::Error, field_name, msg, pos});
                pos += pattern.size();
            }
        };
        check_mention("@everyone", "Contains @everyone — potential ping abuse");
        check_mention("@here", "Contains @here — potential ping abuse");
    }

    // 3. Security: <@...> mention patterns (<@123>, <@!123>, <@&123>)
    for (size_t i = 0; i + 2 < tmpl.size(); ++i) {
        if (tmpl[i] == '<' && tmpl[i + 1] == '@') {
            size_t j = i + 2;
            // Skip optional & or !
            if (j < tmpl.size() && (tmpl[j] == '&' || tmpl[j] == '!')) ++j;
            // Must have at least one digit
            if (j < tmpl.size() && tmpl[j] >= '0' && tmpl[j] <= '9') {
                size_t close = tmpl.find('>', j);
                if (close != std::string::npos) {
                    issues.push_back({
                        ValidationIssue::Level::Error, field_name,
                        "Contains Discord mention pattern <@...>",
                        i
                    });
                    i = close;
                }
            }
        }
    }

    // 4. Empty key detection
    for (size_t i = 0; i + 1 < tmpl.size(); ++i) {
        if (tmpl[i] == '{' && tmpl[i + 1] == '}') {
            issues.push_back({
                ValidationIssue::Level::Error, field_name,
                "Empty key name: {}",
                i
            });
        }
        if (i + 2 < tmpl.size() && tmpl[i] == '{' &&
            (tmpl[i + 1] == '?' || tmpl[i + 1] == '!' || tmpl[i + 1] == '/' || tmpl[i + 1] == ':') &&
            tmpl[i + 2] == '}') {
            issues.push_back({
                ValidationIssue::Level::Error, field_name,
                fmt::format("Empty key name: {{{}}}", tmpl[i + 1]),
                i
            });
        }
    }

    // 5. Conditional block matching (stack-based)
    struct StackEntry { std::string key; size_t position; std::string tag; };
    std::vector<StackEntry> stack;

    size_t pos = 0;
    while (pos < tmpl.size()) {
        size_t open = tmpl.find('{', pos);
        if (open == std::string::npos) break;

        size_t close = tmpl.find('}', open + 1);
        if (close == std::string::npos) break;

        if (open + 1 >= tmpl.size()) break;
        char first = tmpl[open + 1];
        std::string tag_content(tmpl, open + 1, close - open - 1);
        std::string full_tag(tmpl, open, close - open + 1);

        if (first == '?' || first == '!') {
            std::string cond_str = tag_content.substr(1);
            if (!cond_str.empty()) {
                std::string key = extract_key_from_condition(cond_str);
                stack.push_back({key, open, full_tag});
            }
        } else if (first == '#') {
            // Loop opening {#array} or loop variable {#index}/{#position}
            std::string key = tag_content.substr(1);
            // Skip loop variables - they're not block openers
            if (!key.empty() && key != "index" && key != "position") {
                stack.push_back({key, open, full_tag});
            }
        } else if (first == ':') {
            std::string key = tag_content.substr(1);
            if (!key.empty()) {
                if (stack.empty()) {
                    issues.push_back({
                        ValidationIssue::Level::Error, field_name,
                        fmt::format("Else tag {{:{}}} without opening block", key),
                        open
                    });
                } else if (stack.back().key != key) {
                    issues.push_back({
                        ValidationIssue::Level::Error, field_name,
                        fmt::format("Mismatched else tag: expected {{:{}}}, got {{:{}}}", stack.back().key, key),
                        open
                    });
                }
            }
        } else if (first == '/') {
            std::string key = tag_content.substr(1);
            if (!key.empty()) {
                if (stack.empty()) {
                    issues.push_back({
                        ValidationIssue::Level::Error, field_name,
                        fmt::format("Closing tag {{/{}}} without opening block", key),
                        open
                    });
                } else if (stack.back().key != key) {
                    issues.push_back({
                        ValidationIssue::Level::Error, field_name,
                        fmt::format("Mismatched closing tag: expected {{/{}}}, got {{/{}}}", stack.back().key, key),
                        open
                    });
                } else {
                    stack.pop_back();
                }
            }
        }

        pos = close + 1;
    }

    // Unclosed blocks
    for (const auto& entry : stack) {
        issues.push_back({
            ValidationIssue::Level::Error, field_name,
            fmt::format("Unclosed block: {} without {{/{}}}", entry.tag, entry.key),
            entry.position
        });
    }

    // --- WARNINGS ---

    // 6. Unknown placeholders
    std::unordered_set<std::string> known_set;
    for (const auto& ph : known_placeholders) {
        known_set.insert(ph.name);
    }

    pos = 0;
    while (pos < tmpl.size()) {
        size_t open = tmpl.find('{', pos);
        if (open == std::string::npos) break;

        size_t close = tmpl.find('}', open + 1);
        if (close == std::string::npos) break;

        if (open + 1 < tmpl.size()) {
            char first = tmpl[open + 1];
            if (first != '?' && first != '!' && first != '/' && first != ':') {
                std::string key(tmpl, open + 1, close - open - 1);
                if (!key.empty() && known_set.find(key) == known_set.end()) {
                    issues.push_back({
                        ValidationIssue::Level::Warning, field_name,
                        fmt::format("Unknown placeholder: {{{}}}", key),
                        open
                    });
                }
            }
        }

        pos = close + 1;
    }

    // 7. Discord length hint
    // Estimate static text length (strip all {...} tags)
    size_t static_len = 0;
    pos = 0;
    while (pos < tmpl.size()) {
        size_t open = tmpl.find('{', pos);
        if (open == std::string::npos) {
            static_len += tmpl.size() - pos;
            break;
        }
        static_len += open - pos;
        size_t close = tmpl.find('}', open + 1);
        if (close == std::string::npos) {
            static_len += tmpl.size() - open;
            break;
        }
        pos = close + 1;
    }

    size_t limit = discord_field_limit(field_name);
    if (static_len > limit * 80 / 100) {
        issues.push_back({
            ValidationIssue::Level::Warning, field_name,
            fmt::format("Static text ({} chars) approaches Discord {} limit ({} chars)",
                static_len, field_name, limit),
            0
        });
    }

    return issues;
}

std::vector<ValidationIssue> EmbedTemplateService::validate_fields(
    const std::string& command_id,
    const std::string& preset,
    const TemplateFields& fields) {

    std::vector<ValidationIssue> all_issues;

    // Find the command config for known placeholders
    auto commands = get_all_commands();
    const CommandTemplateConfig* cmd = nullptr;
    for (const auto& c : commands) {
        if (c.command_id == command_id) {
            cmd = &c;
            break;
        }
    }

    if (!cmd) return all_issues;

    for (const auto& [field_name, tmpl_text] : fields) {
        auto field_issues = validate_template(field_name, tmpl_text, cmd->placeholders);
        all_issues.insert(all_issues.end(), field_issues.begin(), field_issues.end());
    }

    return all_issues;
}

// ---------------------------------------------------------------------------
// User Custom Template Validation
// ---------------------------------------------------------------------------

namespace {

// Discord field length limits
constexpr size_t LIMIT_CONTENT = 2000;
constexpr size_t LIMIT_TITLE = 256;
constexpr size_t LIMIT_DESCRIPTION = 4096;
constexpr size_t LIMIT_AUTHOR_NAME = 256;
constexpr size_t LIMIT_FOOTER_TEXT = 2048;
constexpr size_t LIMIT_FIELD_NAME = 256;
constexpr size_t LIMIT_FIELD_VALUE = 1024;
constexpr size_t LIMIT_TOTAL_EMBED = 6000;
constexpr size_t LIMIT_JSON_SIZE = 50 * 1024;  // 50KB

// Whitelisted URL domains
const std::vector<std::string> WHITELISTED_DOMAINS = {
    "osu.ppy.sh",
    "ppy.sh",
    "a.ppy.sh",
    "b.ppy.sh",
    "assets.ppy.sh",
    "s.ppy.sh",
    "i.ppy.sh"
};

// Simple regex patterns for dangerous content
bool matches_pattern(const std::string& text, const std::string& pattern, bool case_insensitive = true) {
    if (case_insensitive) {
        std::string lower_text = text;
        std::string lower_pattern = pattern;
        std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
        std::transform(lower_pattern.begin(), lower_pattern.end(), lower_pattern.begin(), ::tolower);
        return lower_text.find(lower_pattern) != std::string::npos;
    }
    return text.find(pattern) != std::string::npos;
}

bool matches_mention_pattern(const std::string& text) {
    // Check for <@123>, <@!123>, <@&123>, <#123>
    size_t pos = 0;
    while ((pos = text.find('<', pos)) != std::string::npos) {
        if (pos + 2 < text.length()) {
            char c1 = text[pos + 1];
            if (c1 == '@' || c1 == '#') {
                size_t end = text.find('>', pos);
                if (end != std::string::npos) {
                    // Check if content between < and > is valid mention format
                    std::string inner = text.substr(pos + 1, end - pos - 1);
                    if (inner[0] == '@' || inner[0] == '#') {
                        // Skip @ or # and optional !
                        size_t num_start = 1;
                        if (inner.length() > 1 && (inner[1] == '!' || inner[1] == '&')) {
                            num_start = 2;
                        }
                        // Check if rest is digits
                        bool all_digits = true;
                        for (size_t i = num_start; i < inner.length(); ++i) {
                            if (!std::isdigit(inner[i])) {
                                all_digits = false;
                                break;
                            }
                        }
                        if (all_digits && inner.length() > num_start) {
                            return true;
                        }
                    }
                }
            }
        }
        ++pos;
    }
    return false;
}

} // anonymous namespace

bool EmbedTemplateService::contains_dangerous_content(const std::string& text, std::vector<std::string>& found) {
    bool has_dangerous = false;

    // @everyone and @here
    if (matches_pattern(text, "@everyone")) {
        found.push_back("@everyone");
        has_dangerous = true;
    }
    if (matches_pattern(text, "@here")) {
        found.push_back("@here");
        has_dangerous = true;
    }

    // Discord invites
    if (matches_pattern(text, "discord.gg/")) {
        found.push_back("Discord invite link");
        has_dangerous = true;
    }
    if (matches_pattern(text, "discord.com/invite/")) {
        found.push_back("Discord invite link");
        has_dangerous = true;
    }

    // Mentions
    if (matches_mention_pattern(text)) {
        found.push_back("User/role/channel mention");
        has_dangerous = true;
    }

    return has_dangerous;
}

bool EmbedTemplateService::is_url_whitelisted(const std::string& url) {
    // Extract domain from URL
    size_t start = url.find("://");
    if (start == std::string::npos) {
        start = 0;
    } else {
        start += 3;
    }

    size_t end = url.find('/', start);
    if (end == std::string::npos) {
        end = url.length();
    }

    std::string domain = url.substr(start, end - start);

    // Remove port if present
    size_t colon = domain.find(':');
    if (colon != std::string::npos) {
        domain = domain.substr(0, colon);
    }

    // Convert to lowercase
    std::transform(domain.begin(), domain.end(), domain.begin(), ::tolower);

    // Check against whitelist
    for (const auto& allowed : WHITELISTED_DOMAINS) {
        if (domain == allowed) return true;
        // Check for subdomain (e.g., "foo.osu.ppy.sh" matches "osu.ppy.sh")
        if (domain.length() > allowed.length() + 1) {
            if (domain.substr(domain.length() - allowed.length() - 1) == "." + allowed) {
                return true;
            }
        }
    }

    return false;
}

std::vector<std::string> EmbedTemplateService::extract_urls(const std::string& text) {
    std::vector<std::string> urls;

    // Simple URL detection - look for http:// or https://
    std::vector<std::string> prefixes = {"https://", "http://"};

    for (const auto& prefix : prefixes) {
        size_t pos = 0;
        while ((pos = text.find(prefix, pos)) != std::string::npos) {
            size_t end = pos;
            // Find end of URL (space, newline, or end of string)
            while (end < text.length() && !std::isspace(text[end]) &&
                   text[end] != '"' && text[end] != '\'' && text[end] != '>' &&
                   text[end] != ')' && text[end] != ']') {
                ++end;
            }
            urls.push_back(text.substr(pos, end - pos));
            pos = end;
        }
    }

    return urls;
}

// Map API command IDs (user-facing) to registry command IDs
static std::string resolve_command_id(const std::string& api_id) {
    static const std::unordered_map<std::string, std::string> ALIASES = {
        {"lb", "leaderboard"},
    };
    auto it = ALIASES.find(api_id);
    return (it != ALIASES.end()) ? it->second : api_id;
}

std::vector<std::string> EmbedTemplateService::get_allowed_placeholders(const std::string& command_id) {
    std::vector<std::string> placeholders;

    std::string resolved = resolve_command_id(command_id);

    auto commands = get_all_commands();
    for (const auto& cmd : commands) {
        if (cmd.command_id == resolved) {
            for (const auto& ph : cmd.placeholders) {
                placeholders.push_back(ph.name);
            }
            break;
        }
    }

    return placeholders;
}

UserTemplateValidationResult EmbedTemplateService::validate_user_template(
    const std::string& command_id,
    const std::string& json_config
) {
    UserTemplateValidationResult result;

    // 1. Check JSON size limit
    if (json_config.length() > LIMIT_JSON_SIZE) {
        result.add_error(fmt::format("Template too large: {} bytes (max {})", json_config.length(), LIMIT_JSON_SIZE));
        return result;  // Early return - can't parse
    }

    // 2. Parse JSON
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_config);
    } catch (const nlohmann::json::exception& e) {
        result.add_error(fmt::format("Invalid JSON: {}", e.what()));
        return result;
    }

    if (!j.is_object()) {
        result.add_error("Template must be a JSON object");
        return result;
    }

    // Get allowed placeholders for this command (resolve aliases like lb->leaderboard)
    auto allowed_placeholders = get_allowed_placeholders(command_id);
    std::set<std::string> allowed_set(allowed_placeholders.begin(), allowed_placeholders.end());

    // Helper to validate a text field
    auto validate_field = [&](const std::string& field_name, const std::string& text, size_t limit) {
        // Check length (estimate - placeholders will expand)
        if (text.length() > limit) {
            result.add_warning(fmt::format("Field '{}' is {} chars (limit {})", field_name, text.length(), limit));
        }

        // Check for dangerous content
        std::vector<std::string> dangerous;
        if (contains_dangerous_content(text, dangerous)) {
            for (const auto& d : dangerous) {
                result.add_error(fmt::format("Field '{}' contains forbidden content: {}", field_name, d));
            }
        }

        // Check URLs
        auto urls = extract_urls(text);
        for (const auto& url : urls) {
            if (!is_url_whitelisted(url)) {
                result.add_error(fmt::format("Field '{}' contains non-whitelisted URL: {}", field_name, url));
            }
        }

        // Check placeholders
        size_t pos = 0;
        while ((pos = text.find('{', pos)) != std::string::npos) {
            size_t end = text.find('}', pos);
            if (end == std::string::npos) {
                result.add_error(fmt::format("Field '{}' has unclosed placeholder at position {}", field_name, pos));
                break;
            }

            std::string placeholder = text.substr(pos + 1, end - pos - 1);

            // Skip empty
            if (placeholder.empty()) {
                result.add_error(fmt::format("Field '{}' has empty placeholder {{}}", field_name));
                pos = end + 1;
                continue;
            }

            // Skip control placeholders (conditionals, loops)
            if (placeholder[0] == '?' || placeholder[0] == '!' || placeholder[0] == '#' ||
                placeholder[0] == ':' || placeholder[0] == '/' || placeholder[0] == '.') {
                pos = end + 1;
                continue;
            }

            // Check if placeholder is allowed
            if (allowed_set.find(placeholder) == allowed_set.end()) {
                result.add_error(fmt::format("Field '{}' uses unknown placeholder: {{{}}}", field_name, placeholder));
            }

            pos = end + 1;
        }
    };

    // Determine format: flat (direct field->value) or nested (has "embed" key)
    bool is_flat_format = j.is_object() && !j.contains("embed");

    if (is_flat_format) {
        // Flat format: { "content": "...", "title": "...", "description": "...", ... }
        // Map field names to Discord limits
        static const std::unordered_map<std::string, size_t> FIELD_LIMITS = {
            {"content", LIMIT_CONTENT},
            {"title", LIMIT_TITLE},
            {"description", LIMIT_DESCRIPTION},
            {"footer", LIMIT_FOOTER_TEXT},
            {"footer_icon", LIMIT_FOOTER_TEXT},
            {"color", 256},  // Color strings are short
        };

        size_t field_count = 0;
        for (auto& [key, value] : j.items()) {
            if (!value.is_string()) continue;
            ++field_count;

            std::string text = value.get<std::string>();
            size_t limit = LIMIT_DESCRIPTION;  // Default limit for unknown fields
            auto lit = FIELD_LIMITS.find(key);
            if (lit != FIELD_LIMITS.end()) {
                limit = lit->second;
            }

            validate_field(key, text, limit);
        }

        if (field_count > 20) {
            result.add_error(fmt::format("Too many template fields: {} (max 20)", field_count));
        }
    } else {
        // Nested format: { "content": "...", "embed": { "title": "...", ... } }

        // 3. Validate content (message text)
        if (j.contains("content") && j["content"].is_string()) {
            validate_field("content", j["content"].get<std::string>(), LIMIT_CONTENT);
        }

        // 4. Validate embed
        if (j.contains("embed") && j["embed"].is_object()) {
            const auto& embed = j["embed"];

            if (embed.contains("title") && embed["title"].is_string()) {
                validate_field("title", embed["title"].get<std::string>(), LIMIT_TITLE);
            }
            if (embed.contains("description") && embed["description"].is_string()) {
                validate_field("description", embed["description"].get<std::string>(), LIMIT_DESCRIPTION);
            }
            if (embed.contains("author") && embed["author"].is_object()) {
                if (embed["author"].contains("name") && embed["author"]["name"].is_string()) {
                    validate_field("author.name", embed["author"]["name"].get<std::string>(), LIMIT_AUTHOR_NAME);
                }
                if (embed["author"].contains("url") && embed["author"]["url"].is_string()) {
                    auto url = embed["author"]["url"].get<std::string>();
                    if (!url.empty() && url.find('{') == std::string::npos && !is_url_whitelisted(url)) {
                        result.add_error(fmt::format("author.url contains non-whitelisted URL: {}", url));
                    }
                }
            }
            if (embed.contains("footer") && embed["footer"].is_object()) {
                if (embed["footer"].contains("text") && embed["footer"]["text"].is_string()) {
                    validate_field("footer.text", embed["footer"]["text"].get<std::string>(), LIMIT_FOOTER_TEXT);
                }
            }
            if (embed.contains("thumbnail") && embed["thumbnail"].is_string()) {
                auto url = embed["thumbnail"].get<std::string>();
                if (!url.empty() && url.find('{') == std::string::npos && !is_url_whitelisted(url)) {
                    result.add_error(fmt::format("thumbnail contains non-whitelisted URL: {}", url));
                }
            }
            if (embed.contains("image") && embed["image"].is_string()) {
                auto url = embed["image"].get<std::string>();
                if (!url.empty() && url.find('{') == std::string::npos && !is_url_whitelisted(url)) {
                    result.add_error(fmt::format("image contains non-whitelisted URL: {}", url));
                }
            }

            // Validate fields array
            if (embed.contains("fields") && embed["fields"].is_array()) {
                size_t field_count = embed["fields"].size();
                if (field_count > 10) {
                    result.add_error(fmt::format("Too many embed fields: {} (max 10)", field_count));
                }

                size_t idx = 0;
                for (const auto& field : embed["fields"]) {
                    if (field.contains("name") && field["name"].is_string()) {
                        validate_field(fmt::format("fields[{}].name", idx), field["name"].get<std::string>(), LIMIT_FIELD_NAME);
                    }
                    if (field.contains("value") && field["value"].is_string()) {
                        validate_field(fmt::format("fields[{}].value", idx), field["value"].get<std::string>(), LIMIT_FIELD_VALUE);
                    }
                    ++idx;
                }
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Color Palette
// ---------------------------------------------------------------------------

const std::unordered_map<std::string, uint32_t> COLOR_PALETTE = {
    // osu! colors
    {"osu_pink", 0xff66aa},
    {"osu_purple", 0xcc5288},

    // Star rating colors
    {"star_easy", 0x4290f5},
    {"star_normal", 0x4fc0ff},
    {"star_hard", 0xffb641},
    {"star_insane", 0xff6682},
    {"star_expert", 0xc967e5},
    {"star_expert_plus", 0x000000},

    // Discord blurple & brand
    {"blurple", 0x5865f2},
    {"discord_green", 0x57f287},
    {"discord_yellow", 0xfee75c},
    {"discord_fuchsia", 0xeb459e},
    {"discord_red", 0xed4245},
    {"discord_black", 0x23272a},
    {"discord_dark", 0x2c2f33},

    // DPP/Material colors
    {"viola_purple", 0x7c4dff},
    {"red", 0xf44336},
    {"pink", 0xe91e63},
    {"purple", 0x9c27b0},
    {"deep_purple", 0x673ab7},
    {"indigo", 0x3f51b5},
    {"blue", 0x2196f3},
    {"light_blue", 0x03a9f4},
    {"cyan", 0x00bcd4},
    {"teal", 0x009688},
    {"green", 0x4caf50},
    {"light_green", 0x8bc34a},
    {"lime", 0xcddc39},
    {"yellow", 0xffeb3b},
    {"amber", 0xffc107},
    {"orange", 0xff9800},
    {"deep_orange", 0xff5722},
    {"brown", 0x795548},
    {"grey", 0x9e9e9e},
    {"blue_grey", 0x607d8b},
    {"white", 0xffffff},
    {"black", 0x000000},

    // Status colors
    {"error", 0xff4444},
    {"success", 0x2ecc71},
    {"warning", 0xf1c40f},
    {"info", 0x3498db},

    // Default
    {"default", 0x7c4dff}
};

uint32_t resolve_color(const std::string& color_str,
                       const std::unordered_map<std::string, std::string>& values) {
    if (color_str.empty()) {
        return COLOR_PALETTE.at("default");
    }

    // First, try to render as template (might contain placeholders)
    std::string resolved = render_template(color_str, values);

    // Check if it's a named color
    auto it = COLOR_PALETTE.find(resolved);
    if (it != COLOR_PALETTE.end()) {
        return it->second;
    }

    // Try to parse as hex
    std::string hex_str = resolved;

    // Remove # prefix if present
    if (!hex_str.empty() && hex_str[0] == '#') {
        hex_str = hex_str.substr(1);
    }
    // Remove 0x prefix if present
    if (hex_str.size() > 2 && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        hex_str = hex_str.substr(2);
    }

    // Parse hex value
    uint32_t color = 0;
    auto result = std::from_chars(hex_str.data(), hex_str.data() + hex_str.size(), color, 16);
    if (result.ec == std::errc()) {
        return color;
    }

    // Fallback to default
    return COLOR_PALETTE.at("default");
}

// ---------------------------------------------------------------------------
// JSON Parsing and Serialization
// ---------------------------------------------------------------------------

using json = nlohmann::json;

FullEmbedTemplate parse_json_template(const std::string& json_str) {
    FullEmbedTemplate tmpl;

    try {
        json j = json::parse(json_str);

        // Message content
        tmpl.content = j.value("content", "");

        // Embed object
        if (j.contains("embed") && j["embed"].is_object()) {
            const auto& e = j["embed"];

            tmpl.color = e.value("color", "default");

            // Author
            if (e.contains("author") && e["author"].is_object()) {
                const auto& a = e["author"];
                tmpl.author_name = a.value("name", "");
                tmpl.author_url = a.value("url", "");
                tmpl.author_icon = a.value("icon_url", "");
            }

            // Main content
            tmpl.title = e.value("title", "");
            tmpl.title_url = e.value("url", "");
            tmpl.description = e.value("description", "");

            // Media
            tmpl.thumbnail = e.value("thumbnail", "");
            tmpl.image = e.value("image", "");

            // Fields
            if (e.contains("fields") && e["fields"].is_array()) {
                for (const auto& f : e["fields"]) {
                    EmbedFieldTemplate field;
                    field.name = f.value("name", "");
                    field.value = f.value("value", "");
                    field.is_inline = f.value("inline", false);
                    field.loop_array = f.value("loop", "");
                    field.condition = f.value("condition", "");
                    tmpl.fields.push_back(field);
                }
            }

            // Footer
            if (e.contains("footer") && e["footer"].is_object()) {
                const auto& f = e["footer"];
                tmpl.footer_text = f.value("text", "");
                tmpl.footer_icon = f.value("icon_url", "");
            } else if (e.contains("footer") && e["footer"].is_string()) {
                tmpl.footer_text = e["footer"].get<std::string>();
            }

            // Timestamp
            tmpl.show_timestamp = e.value("timestamp", false);
        }

    } catch (const std::exception& ex) {
        spdlog::warn("[Templates] Failed to parse JSON template: {}", ex.what());
    }

    return tmpl;
}

std::string serialize_template(const FullEmbedTemplate& tmpl) {
    json j;

    if (!tmpl.content.empty()) {
        j["content"] = tmpl.content;
    }

    json embed;

    if (!tmpl.color.empty()) {
        embed["color"] = tmpl.color;
    }

    // Author
    if (!tmpl.author_name.empty() || !tmpl.author_url.empty() || !tmpl.author_icon.empty()) {
        json author;
        if (!tmpl.author_name.empty()) author["name"] = tmpl.author_name;
        if (!tmpl.author_url.empty()) author["url"] = tmpl.author_url;
        if (!tmpl.author_icon.empty()) author["icon_url"] = tmpl.author_icon;
        embed["author"] = author;
    }

    // Main content
    if (!tmpl.title.empty()) embed["title"] = tmpl.title;
    if (!tmpl.title_url.empty()) embed["url"] = tmpl.title_url;
    if (!tmpl.description.empty()) embed["description"] = tmpl.description;

    // Media
    if (!tmpl.thumbnail.empty()) embed["thumbnail"] = tmpl.thumbnail;
    if (!tmpl.image.empty()) embed["image"] = tmpl.image;

    // Fields
    if (!tmpl.fields.empty()) {
        json fields_arr = json::array();
        for (const auto& f : tmpl.fields) {
            json field;
            field["name"] = f.name;
            field["value"] = f.value;
            if (f.is_inline) field["inline"] = true;
            if (!f.loop_array.empty()) field["loop"] = f.loop_array;
            if (!f.condition.empty()) field["condition"] = f.condition;
            fields_arr.push_back(field);
        }
        embed["fields"] = fields_arr;
    }

    // Footer
    if (!tmpl.footer_text.empty() || !tmpl.footer_icon.empty()) {
        json footer;
        if (!tmpl.footer_text.empty()) footer["text"] = tmpl.footer_text;
        if (!tmpl.footer_icon.empty()) footer["icon_url"] = tmpl.footer_icon;
        embed["footer"] = footer;
    }

    // Timestamp
    if (tmpl.show_timestamp) {
        embed["timestamp"] = true;
    }

    j["embed"] = embed;

    return j.dump(2);
}

// ---------------------------------------------------------------------------
// Full Embed Rendering
// ---------------------------------------------------------------------------

dpp::embed render_full_embed(
    const FullEmbedTemplate& tmpl,
    const std::unordered_map<std::string, std::string>& values,
    const TemplateArrays& arrays) {

    dpp::embed embed;

    // Color
    embed.set_color(resolve_color(tmpl.color, values));

    // Author
    std::string author_name = render_template(tmpl.author_name, values);
    if (!author_name.empty()) {
        std::string author_url = render_template(tmpl.author_url, values);
        std::string author_icon = render_template(tmpl.author_icon, values);
        embed.set_author(author_name, author_url, author_icon);
    }

    // Title
    std::string title = render_template(tmpl.title, values);
    if (!title.empty()) {
        embed.set_title(title);
        std::string title_url = render_template(tmpl.title_url, values);
        if (!title_url.empty()) {
            embed.set_url(title_url);
        }
    }

    // Description
    std::string description = render_template(tmpl.description, values);
    if (!description.empty()) {
        embed.set_description(description);
    }

    // Thumbnail
    std::string thumbnail = render_template(tmpl.thumbnail, values);
    if (!thumbnail.empty()) {
        embed.set_thumbnail(thumbnail);
    }

    // Image
    std::string image = render_template(tmpl.image, values);
    if (!image.empty()) {
        embed.set_image(image);
    }

    // Fields
    for (const auto& field_tmpl : tmpl.fields) {
        // Check condition
        if (!field_tmpl.condition.empty()) {
            auto it = values.find(field_tmpl.condition);
            if (it == values.end() || it->second.empty() || it->second == "false" || it->second == "0") {
                continue;  // Skip this field
            }
        }

        // Check if this is a loop field
        if (!field_tmpl.loop_array.empty()) {
            auto arr_it = arrays.find(field_tmpl.loop_array);
            if (arr_it != arrays.end()) {
                size_t index = 0;
                for (const auto& item : arr_it->second) {
                    // Merge item values with global values (item takes precedence)
                    std::unordered_map<std::string, std::string> merged = values;
                    for (const auto& [k, v] : item) {
                        merged[k] = v;
                    }
                    // Add loop index
                    merged["#index"] = std::to_string(index);
                    merged["#position"] = std::to_string(index + 1);

                    std::string name = render_template(field_tmpl.name, merged);
                    std::string value = render_template(field_tmpl.value, merged);

                    if (!name.empty() && !value.empty()) {
                        embed.add_field(name, value, field_tmpl.is_inline);
                    }
                    ++index;
                }
            }
        } else {
            // Regular field
            std::string name = render_template(field_tmpl.name, values);
            std::string value = render_template(field_tmpl.value, values);

            if (!name.empty() && !value.empty()) {
                embed.add_field(name, value, field_tmpl.is_inline);
            }
        }
    }

    // Footer
    std::string footer_text = render_template(tmpl.footer_text, values);
    if (!footer_text.empty()) {
        dpp::embed_footer footer;
        footer.set_text(footer_text);
        std::string footer_icon = render_template(tmpl.footer_icon, values);
        if (!footer_icon.empty()) {
            footer.set_icon(footer_icon);
        }
        embed.set_footer(footer);
    }

    // Timestamp
    if (tmpl.show_timestamp) {
        embed.set_timestamp(time(nullptr));
    }

    return embed;
}

// ---------------------------------------------------------------------------
// Default Full Templates
// ---------------------------------------------------------------------------

FullEmbedTemplate get_default_full_template(const std::string& command_id,
                                            const std::string& preset) {
    FullEmbedTemplate tmpl;

    // Set common defaults
    tmpl.color = "osu_pink";
    tmpl.show_timestamp = true;

    if (command_id == "profile") {
        tmpl.author_name = "{username}: {pp}pp{?global_rank} (#{global_rank} {country_code}{country_rank}){/global_rank}";
        tmpl.author_url = "https://osu.ppy.sh/users/{user_id}";
        tmpl.author_icon = "https://osu.ppy.sh/images/flags/{country_code}.png";
        tmpl.thumbnail = "https://a.ppy.sh/{user_id}";
        tmpl.description =
            "Accuracy: `{accuracy}%` \xe2\x80\xa2 Level: `{level}.{level_progress}`\n"
            "Playcount: `{playcount}` (`{playtime_hours} hrs`)\n"
            "Medals: `{medal_count}`"
            "{?peak_rank}\nPeak rank: `#{peak_rank}` (<t:{peak_date}:d>){/peak_rank}";
        tmpl.footer_text = "{?join_duration}Joined osu! {join_duration} ago{/join_duration}";
        tmpl.show_timestamp = false;
    }
    else if (command_id == "osc") {
        tmpl.color = "osu_purple";
        tmpl.author_name = "{username}: {pp}pp{?global_rank} (#{global_rank} {country_code}{country_rank}){/global_rank}";
        tmpl.author_url = "https://osu.ppy.sh/users/{user_id}";
        tmpl.author_icon = "https://osu.ppy.sh/images/flags/{country_code}.png";
        tmpl.thumbnail = "https://a.ppy.sh/{user_id}";
        tmpl.title = "In how many top X {mode}map leaderboards is {username}?";
        tmpl.description =
            "```\n"
            "Top 1  : {top1}\n"
            "Top 8  : {top8}\n"
            "Top 15 : {top15}\n"
            "Top 25 : {top25}\n"
            "Top 50 : {top50}\n"
            "Top 100: {top100}\n"
            "```";
        tmpl.show_timestamp = false;
    }
    else if (command_id == "rs") {
        // RS templates based on preset
        tmpl.color = "viola_purple";
        tmpl.author_name = "{username}";
        tmpl.author_url = "https://osu.ppy.sh/users/{user_id}";
        tmpl.author_icon = "https://a.ppy.sh/{user_id}";
        tmpl.title = "{title} [{version}] {sr}\xe2\x98\x85 {mods_suffix}";
        tmpl.title_url = "{beatmap_url}";
        tmpl.thumbnail = "https://b.ppy.sh/thumb/{beatmapset_id}l.jpg";
        tmpl.content = "{try_line}";

        if (preset == "compact") {
            tmpl.description =
                "\xe2\x96\xb8 {rank} {completion} \xe2\x96\xb8 **{pp}PP** \xe2\x96\xb8 {acc}%\n"
                "\xe2\x96\xb8 {score} \xe2\x96\xb8 **x{combo}/{max_combo}** \xe2\x96\xb8 [{300}/{100}/{50}/{miss}]";
            tmpl.footer_text = "{status} map by {creator}";
        }
        else if (preset == "classic") {
            tmpl.description =
                "\xe2\x96\xb8 {rank} {completion} \xe2\x96\xb8 **{pp}PP** {fc_line} \xe2\x96\xb8 {acc}%\n"
                "\xe2\x96\xb8 {score} \xe2\x96\xb8 **x{combo}/{max_combo}** \xe2\x96\xb8 [{300}/{100}/{50}/{miss}]\n"
                "\xe2\x96\xb8 {bpm} BPM \xe2\x80\xa2 {length} \xe2\x96\xb8 AR `{ar}` OD `{od}` CS `{cs}` HP `{hp}`";
            tmpl.footer_text = "{score_type} score";
        }
        else {  // extended
            tmpl.description =
                "\xe2\x96\xb8 {rank} {completion} \xe2\x96\xb8 **{pp}PP** {fc_line} \xe2\x96\xb8 {acc}%\n"
                "\xe2\x96\xb8 {score} \xe2\x96\xb8 **x{combo}/{max_combo}** \xe2\x96\xb8 [{300}/{100}/{50}/{miss}]\n"
                "\xe2\x96\xb8 Aim: **{aim_pp}**pp \xe2\x80\xa2 Speed: **{speed_pp}**pp \xe2\x80\xa2 Acc: **{acc_pp}**pp\n"
                "\xe2\x96\xb8 {bpm} BPM \xe2\x80\xa2 {length} \xe2\x96\xb8 AR `{ar}` OD `{od}` CS `{cs}` HP `{hp}`";
            tmpl.footer_text = "{score_type} score";
        }
    }

    return tmpl;
}

// ---------------------------------------------------------------------------
// EmbedTemplateService JSON Methods
// ---------------------------------------------------------------------------

FullEmbedTemplate EmbedTemplateService::get_full_template(const std::string& key) const {
    std::shared_lock lock(mutex_);

    // First check JSON cache
    auto json_it = json_cache_.find(key);
    if (json_it != json_cache_.end()) {
        return parse_json_template(json_it->second);
    }

    // Fall back to default
    // Parse key to extract command_id and preset
    std::string command_id = key;
    std::string preset = "default";

    size_t colon_pos = key.find(':');
    if (colon_pos != std::string::npos) {
        command_id = key.substr(0, colon_pos);
        preset = key.substr(colon_pos + 1);
    }

    return get_default_full_template(command_id, preset);
}

FullEmbedTemplate EmbedTemplateService::get_user_full_template(
    dpp::snowflake discord_id,
    const std::string& command_id,
    const std::string& preset_name) const {

    if (preset_name == "custom") {
        try {
            auto& db_inst = db::Database::instance();
            std::string api_id = to_api_command_id(command_id);
            auto custom_json = db_inst.get_user_custom_template(discord_id, api_id);
            if (!custom_json && api_id != command_id) {
                custom_json = db_inst.get_user_custom_template(discord_id, command_id);
            }

            if (custom_json) {
                auto j = nlohmann::json::parse(*custom_json);

                // If it has an "embed" key, use standard parser
                if (j.contains("embed")) {
                    return parse_json_template(*custom_json);
                }

                // Flat format: apply fields to the default template
                auto tmpl = get_default_full_template(command_id);
                if (j.contains("description") && j["description"].is_string()) {
                    tmpl.description = j["description"].get<std::string>();
                }
                if (j.contains("title") && j["title"].is_string()) {
                    tmpl.title = j["title"].get<std::string>();
                }
                if (j.contains("content") && j["content"].is_string()) {
                    tmpl.content = j["content"].get<std::string>();
                }
                if (j.contains("footer") && j["footer"].is_string()) {
                    tmpl.footer_text = j["footer"].get<std::string>();
                }
                if (j.contains("color") && j["color"].is_string()) {
                    tmpl.color = j["color"].get<std::string>();
                }
                return tmpl;
            }
        } catch (const std::exception& e) {
            spdlog::warn("[EmbedTemplateService] Failed to load custom full template for user {}: {}",
                discord_id.str(), e.what());
        }
    }

    // Fallback to standard lookup
    return get_full_template(command_id);
}

void EmbedTemplateService::set_full_template(const std::string& key, const FullEmbedTemplate& tmpl) {
    std::string json_str = serialize_template(tmpl);

    // Write to DB first
    auto& db = db::Database::instance();
    db.set_json_template(key, json_str);

    // Update cache
    std::unique_lock lock(mutex_);
    json_cache_[key] = json_str;
}

bool EmbedTemplateService::has_json_template(const std::string& key) const {
    std::shared_lock lock(mutex_);
    return json_cache_.find(key) != json_cache_.end();
}

std::optional<std::string> EmbedTemplateService::get_json_template(const std::string& key) const {
    std::shared_lock lock(mutex_);
    auto it = json_cache_.find(key);
    if (it != json_cache_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void EmbedTemplateService::set_json_template(const std::string& key, const std::string& json_str) {
    // Validate JSON first
    try {
        auto tmpl = parse_json_template(json_str);
        // If parsing succeeds, save it
        auto& db = db::Database::instance();
        db.set_json_template(key, json_str);

        std::unique_lock lock(mutex_);
        json_cache_[key] = json_str;
    } catch (const std::exception& e) {
        spdlog::error("[Templates] Invalid JSON template for '{}': {}", key, e.what());
        throw;
    }
}

void EmbedTemplateService::seed_json_defaults() {
    // This will be called to migrate legacy templates or seed new ones
    // Implementation depends on database methods
}

} // namespace services
