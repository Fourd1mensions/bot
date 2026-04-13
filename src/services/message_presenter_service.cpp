#include "services/message_presenter_service.h"
#include "services/embed_template_service.h"
#include "osu.h"
#include "state/session_state.h"
#include "utils.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace {
std::string get_tmpl_field(const services::TemplateFields& fields, const std::string& name) {
    auto it = fields.find(name);
    return (it != fields.end()) ? it->second : "";
}
} // namespace

namespace services {

dpp::message MessagePresenterService::build_error_message(std::string_view error_text) const {
    auto embed = dpp::embed()
        .set_color(dpp::colors::red)
        .set_description(std::string(error_text));

    dpp::message msg;
    msg.add_embed(embed);
    return msg;
}

dpp::message MessagePresenterService::build_leaderboard_page(
    const Beatmap& beatmap,
    const std::vector<ScorePresentation>& scores_on_page,
    const std::unordered_map<std::string, std::string>& footer_values,
    const std::string& mods_filter,
    size_t total_pages,
    size_t current_page,
    dpp::snowflake discord_id
) const {
    TemplateFields tmpl_fields;
    if (template_service_) {
        // Use user template if discord_id is set (supports Custom preset)
        if (discord_id != 0) {
            tmpl_fields = template_service_->get_user_fields(discord_id, "leaderboard", "default");
        } else {
            tmpl_fields = template_service_->get_fields("leaderboard");
        }
    } else {
        tmpl_fields = EmbedTemplateService::get_default_fields("leaderboard");
    }

    auto get_field = [&](const std::string& name) { return get_tmpl_field(tmpl_fields, name); };

    std::string title = beatmap.to_string();
    if (!mods_filter.empty()) {
        title += fmt::format(" +{}", mods_filter);
    }

    // Render footer
    std::string footer_text = render_template(get_field("footer"), footer_values);

    // Resolve color from template
    std::string color_str = get_field("color");
    uint32_t embed_color = color_str.empty() ? dpp::colors::viola_purple : resolve_color(color_str, footer_values);

    auto embed = dpp::embed()
        .set_color(embed_color)
        .set_thumbnail(beatmap.get_thumbnail_url())
        .set_footer(dpp::embed_footer().set_text(footer_text))
        .set_timestamp(time(0));

    if (!scores_on_page.empty() && scores_on_page[0].user_id > 0) {
        std::string avatar_url = fmt::format("https://a.ppy.sh/{}", scores_on_page[0].user_id);
        embed.set_author(title, beatmap.get_beatmap_url(), avatar_url);
    } else {
        embed.set_author(title, beatmap.get_beatmap_url(), "");
    }

    // Get field templates
    std::string field_name_tmpl = get_field("field_name");
    std::string field_value_tmpl = get_field("field_value");

    // Create embed fields for each score
    for (const auto& score : scores_on_page) {
        std::unordered_map<std::string, std::string> values;
        values["rank"] = std::to_string(score.rank);
        values["username"] = score.username;
        values["user_id"] = std::to_string(score.user_id);
        values["pp"] = fmt::format("{:.0f}", score.display_pp);
        values["mods"] = score.mods;
        values["rank_emoji"] = get_rank_emoji(score.rank_letter);
        values["acc"] = fmt::format("{:.2f}", score.accuracy * 100.0);
        values["combo"] = std::to_string(score.combo);
        values["max_combo"] = std::to_string(score.max_combo);
        values["score"] = fmt::format("{:L}", score.total_score);
        values["300"] = std::to_string(score.count_300);
        values["100"] = std::to_string(score.count_100);
        values["50"] = std::to_string(score.count_50);
        values["miss"] = std::to_string(score.count_miss);
        values["date"] = score.date;

        dpp::embed_field field;
        field.name = render_template(field_name_tmpl, values);
        field.value = render_template(field_value_tmpl, values);
        field.is_inline = false;
        embed.fields.push_back(field);
    }

    dpp::message msg;
    msg.add_embed(embed);

    if (total_pages > 1) {
        msg.add_component(build_pagination_row("lb_", current_page, total_pages, false));
    }

    return msg;
}

MessagePresenterService::RecentScoreCacheData MessagePresenterService::build_recent_score_cache_data(
    const Score& score,
    const Beatmap& beatmap,
    const DifficultyInfo& difficulty,
    const PPInfo& pp_info,
    const PaginationInfo& pagination,
    const std::string& score_type,
    float completion_percent,
    float modded_bpm,
    uint32_t modded_length,
    EmbedPreset preset,
    int try_number,
    const MapPositionInfo& map_position,
    dpp::snowflake discord_id
) const {
    RecentScoreCacheData data;
    data.preset = preset;

    std::string preset_name = embed_preset_to_string(preset);

    std::unordered_map<std::string, std::string> values;

    values["title"]   = beatmap.get_title();
    values["artist"]  = beatmap.get_artist();
    values["version"] = beatmap.get_version();
    values["sr"]      = fmt::format("{:.3g}", beatmap.get_difficulty_rating(true));
    values["mods"]    = score.get_mods();

    if (!score.get_mods().empty() && score.get_mods() != "NM") {
        values["mods_suffix"] = fmt::format("+{}", score.get_mods());
    } else {
        values["mods_suffix"] = "";
    }

    values["rank"] = get_rank_emoji(score.get_rank());

    if (completion_percent < 100.0f || score.get_rank() == "F") {
        values["rank_line"] = fmt::format("\xe2\x96\xb8 {} **({:.2f}%)**", values["rank"], completion_percent);
    } else {
        values["rank_line"] = fmt::format("\xe2\x96\xb8 {}", values["rank"]);
    }
    values["completion"] = (completion_percent < 100.0f || score.get_rank() == "F")
        ? fmt::format("{:.2f}%", completion_percent) : "";

    values["pp"]     = fmt::format("{:.2f}", pp_info.current_pp);
    values["fc_pp"]  = fmt::format("{:.2f}", pp_info.fc_pp);
    values["fc_acc"] = fmt::format("{:.2f}", pp_info.fc_accuracy);

    if (pp_info.has_fc_pp && pp_info.fc_pp > pp_info.current_pp) {
        values["fc_line"] = fmt::format("({:.2f}PP for {:.2f}% FC)",
            pp_info.fc_pp, pp_info.fc_accuracy);
    } else {
        values["fc_line"] = "";
    }

    values["acc"]       = fmt::format("{:.2f}", score.get_accuracy() * 100);
    values["combo"]     = std::to_string(score.get_max_combo());
    values["max_combo"] = std::to_string(beatmap.get_max_combo());
    values["score"]     = fmt::format("{:L}", score.get_total_score());
    values["300"]       = std::to_string(score.get_count_300());
    values["100"]       = std::to_string(score.get_count_100());
    values["50"]        = std::to_string(score.get_count_50());
    values["miss"]      = std::to_string(score.get_count_miss());

    values["bpm"]    = std::to_string(static_cast<int>(modded_bpm));
    values["length"] = fmt::format("{}:{:02d}", modded_length / 60, modded_length % 60);
    values["ar"]     = fmt::format("{:.1f}", difficulty.approach_rate);
    values["od"]     = fmt::format("{:.1f}", difficulty.overall_difficulty);
    values["cs"]     = fmt::format("{:.1f}", difficulty.circle_size);
    values["hp"]     = fmt::format("{:.1f}", difficulty.hp_drain_rate);

    values["aim_pp"]   = fmt::format("{:.0f}", pp_info.aim_pp);
    values["speed_pp"] = fmt::format("{:.0f}", pp_info.speed_pp);
    values["acc_pp"]   = fmt::format("{:.0f}", pp_info.accuracy_pp);

    values["pp_int"]  = fmt::format("{:.0f}", pp_info.current_pp);
    values["fc_pp_int"] = fmt::format("{:.0f}", pp_info.fc_pp);

    std::string status_str = beatmap_status_to_string(beatmap.get_status());
    if (!status_str.empty()) status_str[0] = std::toupper(status_str[0]);
    values["status"]  = status_str;
    values["creator"] = beatmap.get_creator().empty() ? "Unknown" : beatmap.get_creator();

    values["username"]    = score.get_username();
    values["user_id"]     = std::to_string(score.get_user_id());
    values["rank_raw"]    = score.get_rank();
    values["mode"]        = score.get_mode();

    values["gamemode_string"] = utils::gamemode_to_string(score.get_mode());
    values["passed"]      = score.get_passed() ? "true" : "false";
    values["date"]        = score.get_created_at();
    {
        time_t unix_ts = utils::ISO8601_to_UNIX(score.get_created_at());
        values["date_unix"]     = std::to_string(unix_ts);
        values["date_relative"] = fmt::format("<t:{}:R>", unix_ts);
    }
    values["score_raw"]   = std::to_string(score.get_total_score());

    values["beatmap_id"]    = std::to_string(beatmap.get_beatmap_id());
    values["beatmapset_id"] = std::to_string(beatmap.get_beatmapset_id());
    values["beatmap_url"]   = beatmap.get_beatmap_url();
    values["image_url"]     = beatmap.get_image_url();
    values["thumbnail_url"] = beatmap.get_thumbnail_url();
    values["sr_raw"]        = fmt::format("{:.3g}", beatmap.get_difficulty_rating(false));
    values["bpm_raw"]       = fmt::format("{:.1f}", beatmap.get_bpm());
    values["length_raw"]    = std::to_string(beatmap.get_total_length());

    values["aim_diff"]      = fmt::format("{:.2f}", difficulty.aim_difficulty);
    values["speed_diff"]    = fmt::format("{:.2f}", difficulty.speed_difficulty);
    values["total_objects"]  = std::to_string(difficulty.total_objects);
    values["max_combo_diff"] = std::to_string(difficulty.max_combo);

    // Weight (only present for best scores, 0 otherwise)
    values["weight_pct"]  = fmt::format("{:.2f}", score.get_weight_percentage());
    values["weight_pp"]   = fmt::format("{:.2f}", score.get_weight_pp());

    values["page"]  = std::to_string(pagination.current + 1);
    values["total"] = std::to_string(pagination.total);

    std::string cap_type = score_type;
    if (!cap_type.empty()) cap_type[0] = std::toupper(cap_type[0]);
    values["score_type"] = cap_type;

    values["try_number"] = std::to_string(try_number);
    if (try_number > 0) {
        values["try_line"] = fmt::format("Try #{}", try_number);
    } else {
        values["try_line"] = "";
    }

    // Map position and personal best
    if (map_position.position > 0) {
        values["map_rank"] = std::to_string(map_position.position);
    } else {
        values["map_rank"] = "";
    }
    if (map_position.best_pp > 0.0) {
        values["pb_pp"] = fmt::format("{:.2f}", map_position.best_pp);
        values["pb_pp_int"] = fmt::format("{:.0f}", map_position.best_pp);
    } else {
        values["pb_pp"] = "";
        values["pb_pp_int"] = "";
    }

    // Color placeholders
    values["sr_color"] = fmt::format("#{:06x}", get_star_rating_color(beatmap.get_difficulty_rating(true)));
    // Rank-based color
    std::string rank = score.get_rank();
    uint32_t rank_color_val = 0x7c4dff; // default viola_purple
    if (rank == "SS" || rank == "SSH") {
        rank_color_val = 0xffd700; // gold
    } else if (rank == "S" || rank == "SH") {
        rank_color_val = 0xc0c0c0; // silver
    } else if (rank == "A") {
        rank_color_val = 0x4caf50; // green
    } else if (rank == "B") {
        rank_color_val = 0x2196f3; // blue
    } else if (rank == "C") {
        rank_color_val = 0x9c27b0; // purple
    } else if (rank == "D") {
        rank_color_val = 0xf44336; // red
    } else if (rank == "F") {
        rank_color_val = 0x9e9e9e; // grey
    }
    values["rank_color"] = fmt::format("#{:06x}", rank_color_val);

    EmbedTemplate tmpl;
    if (template_service_) {
        // Use user template if discord_id is provided (supports Custom preset)
        if (discord_id != 0) {
            tmpl = template_service_->get_user_template(discord_id, preset_name);
        } else {
            tmpl = template_service_->get_template(preset_name);
        }
    } else {
        // Construct defaults from the generic registry
        auto defaults = EmbedTemplateService::get_default_fields("rs:" + preset_name);
        tmpl.content = defaults.count("content") ? defaults.at("content") : "";
        tmpl.title = defaults.count("title") ? defaults.at("title") : "";
        tmpl.description = defaults.count("description") ? defaults.at("description") : "";
        tmpl.beatmap_info = defaults.count("beatmap_info") ? defaults.at("beatmap_info") : "";
        tmpl.footer = defaults.count("footer") ? defaults.at("footer") : "";
        tmpl.footer_icon = defaults.count("footer_icon") ? defaults.at("footer_icon") : "";
        tmpl.color = defaults.count("color") ? defaults.at("color") : "{rank_color}";
    }

    data.content      = utils::rtrim(render_template(tmpl.content, values));
    data.title        = utils::rtrim(render_template(tmpl.title, values));
    data.description  = render_template(tmpl.description, values);
    data.beatmap_info = render_template(tmpl.beatmap_info, values);
    data.footer       = utils::rtrim(render_template(tmpl.footer, values));
    data.footer_icon  = utils::rtrim(render_template(tmpl.footer_icon, values));

    // Resolve color from template
    data.color = resolve_color(tmpl.color, values);

    data.url       = beatmap.get_beatmap_url();
    data.thumbnail = beatmap.get_thumbnail_url();
    data.timestamp = utils::ISO8601_to_UNIX(score.get_created_at());
    data.username  = score.get_username();
    data.user_id   = score.get_user_id();

    return data;
}

dpp::message MessagePresenterService::build_recent_score_page(
    const Score& score,
    const Beatmap& beatmap,
    const DifficultyInfo& difficulty,
    const PPInfo& pp_info,
    const PaginationInfo& pagination,
    const std::string& score_type,
    float completion_percent,
    float modded_bpm,
    uint32_t modded_length,
    EmbedPreset preset,
    int try_number,
    const MapPositionInfo& map_position,
    dpp::snowflake discord_id
) const {
    auto data = build_recent_score_cache_data(
        score, beatmap, difficulty, pp_info, pagination,
        score_type, completion_percent, modded_bpm, modded_length, preset, try_number,
        map_position, discord_id
    );

    return build_from_cache_data(data, pagination);
}

dpp::message MessagePresenterService::build_map_info(
    const Beatmap& beatmap,
    const DifficultyInfo& difficulty,
    const std::vector<double>& pp_values,
    const std::string& mods,
    uint32_t beatmapset_id,
    float modded_bpm,
    uint32_t modded_length,
    EmbedPreset preset,
    dpp::snowflake discord_id
) const {
    std::string preset_name = embed_preset_to_string(preset);

    TemplateFields tmpl_fields;
    if (template_service_) {
        // Use user template if discord_id is set (supports Custom preset)
        if (discord_id != 0) {
            tmpl_fields = template_service_->get_user_fields(discord_id, "map", preset_name);
        } else {
            tmpl_fields = template_service_->get_fields("map:" + preset_name);
        }
    } else {
        tmpl_fields = EmbedTemplateService::get_default_fields("map:" + preset_name);
    }

    auto get_field = [&](const std::string& name) { return get_tmpl_field(tmpl_fields, name); };

    // Build values map
    std::unordered_map<std::string, std::string> values;
    values["title"] = beatmap.get_title();
    values["artist"] = beatmap.get_artist();
    values["version"] = beatmap.get_version();
    values["sr"] = fmt::format("{:.2f}", difficulty.star_rating);
    values["mode"] = beatmap.get_mode();

    values["gamemode_string"] = utils::gamemode_to_string(beatmap.get_mode());

    std::string status_str = beatmap_status_to_string(beatmap.get_status());
    if (!status_str.empty()) status_str[0] = std::toupper(status_str[0]);
    values["status"] = status_str;

    values["mods"] = mods;
    if (!mods.empty() && mods != "NM") {
        values["mods_suffix"] = fmt::format("+{}", mods);
    } else {
        values["mods_suffix"] = "";
    }

    values["ar"] = fmt::format("{:.1f}", difficulty.approach_rate);
    values["od"] = fmt::format("{:.1f}", difficulty.overall_difficulty);
    values["cs"] = fmt::format("{:.1f}", difficulty.circle_size);
    values["hp"] = fmt::format("{:.1f}", difficulty.hp_drain_rate);
    values["aim_diff"] = fmt::format("{:.2f}", difficulty.aim_difficulty);
    values["speed_diff"] = fmt::format("{:.2f}", difficulty.speed_difficulty);
    values["max_combo"] = std::to_string(difficulty.max_combo);
    values["bpm"] = fmt::format("{:.0f}", modded_bpm);
    values["length"] = fmt::format("{}:{:02d}", modded_length / 60, modded_length % 60);
    values["beatmapset_id"] = std::to_string(beatmapset_id);
    values["beatmap_url"] = beatmap.get_beatmap_url();
    values["image_url"] = beatmap.get_image_url();

    values["pp_90"] = pp_values.size() >= 1 ? fmt::format("{:.0f}", pp_values[0]) : "?";
    values["pp_95"] = pp_values.size() >= 2 ? fmt::format("{:.0f}", pp_values[1]) : "?";
    values["pp_99"] = pp_values.size() >= 3 ? fmt::format("{:.0f}", pp_values[2]) : "?";
    values["pp_100"] = pp_values.size() >= 4 ? fmt::format("{:.0f}", pp_values[3]) : "?";

    std::string title = utils::rtrim(render_template(get_field("title"), values));
    std::string description = render_template(get_field("description"), values);

    dpp::embed embed;
    embed.set_title(title);
    embed.set_url(beatmap.get_beatmap_url());
    embed.set_image(beatmap.get_image_url());
    embed.set_description(description);

    // Render named fields (skip if both name and content are empty)
    auto add_template_field = [&](const std::string& name_key, const std::string& content_key, bool is_inline) {
        std::string field_name = render_template(get_field(name_key), values);
        std::string field_value = render_template(get_field(content_key), values);
        if (!field_name.empty() && !field_value.empty()) {
            embed.add_field(field_name, field_value, is_inline);
        }
    };

    add_template_field("pp_field_name", "pp_field", true);
    add_template_field("difficulty_field_name", "difficulty_field", true);
    add_template_field("map_info_field_name", "map_info_field", true);
    add_template_field("download_field_name", "download_field", false);
    add_template_field("media_field_name", "media_field", false);

    embed.set_color(get_star_rating_color(difficulty.star_rating));

    dpp::message msg;
    msg.add_embed(embed);
    return msg;
}

dpp::message MessagePresenterService::build_background(
    const Beatmap& beatmap,
    const std::string& bg_url,
    const std::string& source
) const {
    auto embed = dpp::embed()
        .set_color(dpp::colors::viola_purple)
        .set_title(beatmap.to_string())
        .set_url(beatmap.get_beatmap_url())
        .set_image(bg_url)
        .set_footer(dpp::embed_footer().set_text(source));

    dpp::message msg;
    msg.add_embed(embed);
    return msg;
}

dpp::message MessagePresenterService::build_audio(
    const Beatmap& beatmap,
    const std::string& audio_url,
    const std::string& source
) const {
    auto embed = dpp::embed()
        .set_color(dpp::colors::viola_purple)
        .set_title(beatmap.to_string())
        .set_url(beatmap.get_beatmap_url())
        .set_description(fmt::format("[Download audio]({})", audio_url))
        .set_footer(dpp::embed_footer().set_text(source));

    dpp::message msg;
    msg.add_embed(embed);
    return msg;
}

dpp::message MessagePresenterService::build_audio_with_attachment(
    const Beatmap& beatmap,
    const std::string& filename,
    const std::string& source
) const {
    auto embed = dpp::embed()
        .set_color(dpp::colors::viola_purple)
        .set_title(beatmap.to_string())
        .set_url(beatmap.get_beatmap_url())
        .set_footer(dpp::embed_footer().set_text(source));

    dpp::message msg;
    msg.add_embed(embed);
    return msg;
}

dpp::message MessagePresenterService::build_compare_page(const CompareState& state) const {
    std::string preset_name = embed_preset_to_string(state.preset);

    TemplateFields tmpl_fields;
    if (template_service_) {
        // Use user template if caller_discord_id is set (supports Custom preset)
        if (state.caller_discord_id != 0) {
            tmpl_fields = template_service_->get_user_fields(state.caller_discord_id, "compare", preset_name);
        } else {
            tmpl_fields = template_service_->get_fields("compare:" + preset_name);
        }
    } else {
        tmpl_fields = EmbedTemplateService::get_default_fields("compare:" + preset_name);
    }

    auto get_field = [&](const std::string& name) { return get_tmpl_field(tmpl_fields, name); };

    // Common values for title/header/footer
    std::unordered_map<std::string, std::string> common_values;
    common_values["title"] = state.beatmap.get_title();
    common_values["artist"] = state.beatmap.get_artist();
    common_values["version"] = state.beatmap.get_version();
    common_values["beatmap_url"] = state.beatmap.get_beatmap_url();
    common_values["thumbnail_url"] = state.beatmap.get_thumbnail_url();
    common_values["image_url"] = state.beatmap.get_image_url();
    common_values["username"] = state.username;
    common_values["score_count"] = std::to_string(state.scores.size());
    common_values["page"] = std::to_string(state.current_page + 1);
    common_values["total"] = std::to_string(state.total_pages);

    common_values["sr"] = fmt::format("{:.3g}", state.beatmap.get_difficulty_rating(false));
    common_values["beatmap_id"] = std::to_string(state.beatmap.get_beatmap_id());
    common_values["beatmapset_id"] = std::to_string(state.beatmap.get_beatmapset_id());
    common_values["max_combo"] = std::to_string(state.beatmap.get_max_combo());
    common_values["bpm"] = std::to_string(static_cast<int>(state.beatmap.get_bpm()));
    common_values["length"] = fmt::format("{}:{:02d}", state.beatmap.get_total_length() / 60, state.beatmap.get_total_length() % 60);
    common_values["ar"] = fmt::format("{:.1f}", state.beatmap.get_ar());
    common_values["od"] = fmt::format("{:.1f}", state.beatmap.get_od());
    common_values["cs"] = fmt::format("{:.1f}", state.beatmap.get_cs());
    common_values["hp"] = fmt::format("{:.1f}", state.beatmap.get_hp());
    common_values["mode"] = state.beatmap.get_mode();
    common_values["gamemode_string"] = utils::gamemode_to_string(state.beatmap.get_mode());

    std::string status_str = beatmap_status_to_string(state.beatmap.get_status());
    if (!status_str.empty()) status_str[0] = std::toupper(status_str[0]);
    common_values["status"] = status_str;
    common_values["creator"] = state.beatmap.get_creator().empty() ? "Unknown" : state.beatmap.get_creator();

    if (!state.mods_filter.empty() && state.mods_filter != "NM") {
        common_values["mods_suffix"] = fmt::format("+{}", state.mods_filter);
    } else {
        common_values["mods_suffix"] = "";
    }

    std::string title = render_template(get_field("title"), common_values);
    title.erase(std::find_if(title.rbegin(), title.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), title.end());

    std::string header = render_template(get_field("header"), common_values);
    std::string footer_text = render_template(get_field("footer"), common_values);

    // Resolve color from template
    std::string color_str = get_field("color");
    uint32_t embed_color = color_str.empty() ? dpp::colors::viola_purple : resolve_color(color_str, common_values);

    auto embed = dpp::embed()
        .set_color(embed_color)
        .set_title(title)
        .set_url(state.beatmap.get_beatmap_url())
        .set_thumbnail(state.beatmap.get_thumbnail_url())
        .set_description(header)
        .set_footer(dpp::embed_footer().set_text(footer_text))
        .set_timestamp(time(0));

    // Get field templates
    std::string field_name_tmpl = get_field("field_name");
    std::string field_value_tmpl = get_field("field_value");

    // Create embed fields for each score on this page
    size_t start_idx = state.current_page * CompareState::SCORES_PER_PAGE;
    size_t end_idx = std::min(start_idx + CompareState::SCORES_PER_PAGE, state.scores.size());

    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& score = state.scores[i];
        std::string mods_str = score.get_mods().empty() ? "NM" : score.get_mods();

        std::unordered_map<std::string, std::string> values = common_values;
        values["index"] = std::to_string(i + 1);
        values["rank"] = get_rank_emoji(score.get_rank());
        values["rank_raw"] = score.get_rank();
        values["mods"] = mods_str;
        values["mods_suffix_score"] = (mods_str != "NM") ? fmt::format("+{}", mods_str) : "";
        values["pp"] = fmt::format("{:.2f}", score.get_pp());
        values["pp_int"] = fmt::format("{:.0f}", score.get_pp());
        values["acc"] = fmt::format("{:.2f}", score.get_accuracy() * 100.0);
        values["combo"] = std::to_string(score.get_max_combo());
        values["score"] = fmt::format("{:L}", score.get_total_score());
        values["score_raw"] = std::to_string(score.get_total_score());
        values["300"] = std::to_string(score.get_count_300());
        values["100"] = std::to_string(score.get_count_100());
        values["50"] = std::to_string(score.get_count_50());
        values["miss"] = std::to_string(score.get_count_miss());
        values["passed"] = score.get_passed() ? "true" : "false";
        values["failed_line"] = score.get_passed() ? "" : " \xe2\x80\xa2 **FAILED**";
        values["user_id"] = std::to_string(score.get_user_id());

        time_t unix_ts = utils::ISO8601_to_UNIX(score.get_created_at());
        values["date"] = score.get_created_at();
        values["date_unix"] = std::to_string(unix_ts);
        values["date_relative"] = fmt::format("<t:{}:R>", unix_ts);

        values["weight_pct"] = fmt::format("{:.2f}", score.get_weight_percentage());
        values["weight_pp"] = fmt::format("{:.2f}", score.get_weight_pp());

        dpp::embed_field field;
        field.name = render_template(field_name_tmpl, values);
        field.value = render_template(field_value_tmpl, values);
        field.is_inline = false;
        embed.fields.push_back(field);
    }

    dpp::message msg;
    msg.add_embed(embed);

    if (state.total_pages > 1) {
        msg.add_component(build_pagination_row("cmp_", state.current_page, state.total_pages, false));
    }

    return msg;
}

dpp::message MessagePresenterService::build_from_cache_data(
    const RecentScoreCacheData& cache_data,
    const PaginationInfo& pagination
) const {
    std::string avatar_url = fmt::format("https://a.ppy.sh/{}", cache_data.user_id);

    auto embed = dpp::embed()
        .set_color(cache_data.color)
        .set_author(cache_data.title, cache_data.url, avatar_url)
        .set_description(cache_data.description)
        .set_thumbnail(cache_data.thumbnail);

    if (!cache_data.beatmap_info.empty()) {
        embed.add_field("", cache_data.beatmap_info, false);
    }
    auto footer = dpp::embed_footer().set_text(cache_data.footer);
    if (!cache_data.footer_icon.empty()) {
        footer.set_icon(cache_data.footer_icon);
    }
    embed.set_footer(footer)
         .set_timestamp(cache_data.timestamp);

    dpp::message msg;
    if (!cache_data.content.empty()) {
        msg.set_content(cache_data.content);
    }
    msg.add_embed(embed);

    if (pagination.total > 1) {
        msg.add_component(build_pagination_row("rs_", pagination.current, pagination.total, true));
    }

    return msg;
}

dpp::component MessagePresenterService::build_pagination_row(
    const std::string& prefix,
    size_t current,
    size_t total,
    bool has_refresh
) const {
    dpp::component action_row;
    action_row.set_type(dpp::cot_action_row);

    // First button: refresh when at index 0 (if has_refresh), otherwise jump to first
    dpp::component first_button;
    first_button.set_type(dpp::cot_button)
        .set_style(dpp::cos_secondary);

    if (has_refresh && current == 0) {
        first_button.set_id(prefix + "refresh")
            .set_emoji("🔄");
    } else {
        first_button.set_id(prefix + "first")
            .set_emoji("⏮️")
            .set_disabled(current == 0);
    }

    dpp::component prev_button = dpp::component()
        .set_type(dpp::cot_button)
        .set_id(prefix + "prev")
        .set_style(dpp::cos_secondary)
        .set_emoji("⬅️")
        .set_disabled(current == 0);

    dpp::component page_indicator = dpp::component()
        .set_type(dpp::cot_button)
        .set_id(prefix + "select")
        .set_label(fmt::format("{}/{}", current + 1, total))
        .set_style(dpp::cos_secondary);

    dpp::component next_button = dpp::component()
        .set_type(dpp::cot_button)
        .set_id(prefix + "next")
        .set_style(dpp::cos_secondary)
        .set_emoji("➡️")
        .set_disabled(current >= total - 1);

    dpp::component last_button = dpp::component()
        .set_type(dpp::cot_button)
        .set_id(prefix + "last")
        .set_style(dpp::cos_secondary)
        .set_emoji("⏭️")
        .set_disabled(current >= total - 1);

    action_row.add_component(first_button);
    action_row.add_component(prev_button);
    action_row.add_component(page_indicator);
    action_row.add_component(next_button);
    action_row.add_component(last_button);

    return action_row;
}

std::string MessagePresenterService::get_rank_emoji(const std::string& rank) const {
    static const std::unordered_map<std::string, std::string> rank_emojis = {
        {"F",  "<:RankingF:1278036373332295843>"},
        {"D",  "<:RankingD:1278036354248474674>"},
        {"C",  "<:RankingC:1278036342441250998>"},
        {"B",  "<:RankingB:1278036331099852810>"},
        {"A",  "<:RankingA:1278036315421671424>"},
        {"S",  "<:RankingS:1278036387433680968>"},
        {"SH", "<:RankingSH:1278036405230108744>"},
        {"X",  "<:RankingSS:1304449505873367130>"},
        {"XH", "<:RankingSSH:1304449533006057544>"}
    };

    auto it = rank_emojis.find(rank);
    if (it != rank_emojis.end()) {
        return it->second;
    }
    return "";
}

uint32_t MessagePresenterService::get_star_rating_color(double star_rating) const {
    if (star_rating < 2.0) {
        return 0x4290F5; // Easy (blue)
    } else if (star_rating < 2.7) {
        return 0x4FC0FF; // Normal (light blue)
    } else if (star_rating < 4.0) {
        return 0xFFB641; // Hard (yellow)
    } else if (star_rating < 5.3) {
        return 0xFF6682; // Insane (pink)
    } else if (star_rating < 6.5) {
        return 0xC967E5; // Expert (purple)
    } else {
        return 0x000000; // Expert+ (black)
    }
}

dpp::message MessagePresenterService::build_top_page(const TopState& state) const {
    TemplateFields tmpl_fields;
    if (template_service_) {
        // Use user template if caller_discord_id is set (supports Custom preset)
        if (state.caller_discord_id != 0) {
            tmpl_fields = template_service_->get_user_fields(state.caller_discord_id, "top", "default");
        } else {
            tmpl_fields = template_service_->get_fields("top");
        }
    } else {
        tmpl_fields = EmbedTemplateService::get_default_fields("top");
    }

    auto get_field = [&](const std::string& name) { return get_tmpl_field(tmpl_fields, name); };

    // Common values for title/header/footer
    std::unordered_map<std::string, std::string> common_values;
    common_values["username"] = state.username;
    common_values["user_id"] = std::to_string(state.osu_user_id);
    common_values["mode"] = state.mode;
    common_values["gamemode_string"] = utils::gamemode_to_string(state.mode);
    common_values["total_scores"] = std::to_string(state.scores.size());
    common_values["page"] = std::to_string(state.current_page + 1);
    common_values["total_pages"] = std::to_string(state.total_pages);

    // Calculate shown scores on current page
    size_t start_idx = state.current_page * TopState::SCORES_PER_PAGE;
    size_t end_idx = std::min(start_idx + TopState::SCORES_PER_PAGE, state.scores.size());
    common_values["shown"] = std::to_string(end_idx - start_idx);

    // Build filter summary
    std::string filter_summary;
    if (!state.mods_filter.empty()) {
        bool exclude = state.mods_filter[0] == '-';
        filter_summary += fmt::format(" \xe2\x80\xa2 Mods: {}{}", exclude ? "exclude " : "", state.mods_filter);
    }
    if (!state.grade_filter.empty()) {
        filter_summary += fmt::format(" \xe2\x80\xa2 Grade: {}", state.grade_filter);
    }
    if (state.sort_method != TopSortMethod::PP) {
        filter_summary += fmt::format(" \xe2\x80\xa2 Sort: {}", top_sort_method_to_string(state.sort_method));
    }
    if (state.reverse) {
        filter_summary += " (reversed)";
    }
    common_values["filter"] = filter_summary;
    common_values["mods_filter"] = state.mods_filter;
    common_values["grade_filter"] = state.grade_filter;
    common_values["sort"] = top_sort_method_to_string(state.sort_method);
    common_values["reverse"] = state.reverse ? "reversed" : "";

    std::string footer_text = render_template(get_field("footer"), common_values);

    // Resolve color from template
    std::string color_str = get_field("color");
    uint32_t embed_color = color_str.empty() ? dpp::colors::viola_purple : resolve_color(color_str, common_values);

    std::string avatar_url = fmt::format("https://a.ppy.sh/{}", state.osu_user_id);
    std::string author_name = fmt::format("{}'s Top Plays", state.username);
    if (!state.mode.empty() && state.mode != "osu") {
        author_name += fmt::format(" ({})", common_values["gamemode_string"]);
    }

    auto embed = dpp::embed()
        .set_color(embed_color)
        .set_author(author_name, fmt::format("https://osu.ppy.sh/users/{}", state.osu_user_id), avatar_url)
        .set_footer(dpp::embed_footer().set_text(footer_text))
        .set_timestamp(time(0));

    // Get field templates
    std::string field_name_tmpl = get_field("field_name");
    std::string field_value_tmpl = get_field("field_value");

    // Create embed fields for each score on this page
    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& score = state.scores[i];
        std::string mods_str = score.get_mods().empty() ? "NM" : score.get_mods();

        std::unordered_map<std::string, std::string> values = common_values;
        values["index"] = std::to_string(i + 1);  // Original position (1-based)
        values["position"] = std::to_string(i - start_idx + 1);  // Position on page

        // Score-specific beatmap info (from nested beatmap in best scores response)
        values["beatmap_id"] = std::to_string(score.get_beatmap_id());
        values["beatmapset_id"] = std::to_string(score.get_beatmapset_id());
        values["title"] = score.get_beatmap_title();
        values["artist"] = score.get_beatmap_artist();
        values["version"] = score.get_beatmap_version();
        values["beatmap_url"] = fmt::format("https://osu.ppy.sh/b/{}", score.get_beatmap_id());

        values["rank"] = get_rank_emoji(score.get_rank());
        values["rank_raw"] = score.get_rank();
        values["mods"] = mods_str;
        values["mods_suffix"] = (mods_str != "NM") ? fmt::format("+{}", mods_str) : "";

        values["pp"] = fmt::format("{:.2f}", score.get_pp());
        values["pp_int"] = fmt::format("{:.0f}", score.get_pp());
        values["weight_pct"] = fmt::format("{:.2f}", score.get_weight_percentage());
        values["weight_pp"] = fmt::format("{:.2f}", score.get_weight_pp());

        values["acc"] = fmt::format("{:.2f}", score.get_accuracy() * 100.0);
        values["combo"] = std::to_string(score.get_max_combo());
        values["max_combo"] = score.get_beatmap_max_combo() > 0
            ? std::to_string(score.get_beatmap_max_combo())
            : "?";
        values["score"] = fmt::format("{:L}", score.get_total_score());
        values["score_raw"] = std::to_string(score.get_total_score());
        values["300"] = std::to_string(score.get_count_300());
        values["100"] = std::to_string(score.get_count_100());
        values["50"] = std::to_string(score.get_count_50());
        values["miss"] = std::to_string(score.get_count_miss());

        time_t unix_ts = utils::ISO8601_to_UNIX(score.get_created_at());
        values["date"] = fmt::format("<t:{}:R>", unix_ts);
        values["date_raw"] = score.get_created_at();

        dpp::embed_field field;
        field.name = render_template(field_name_tmpl, values);
        field.value = render_template(field_value_tmpl, values);
        field.is_inline = false;
        embed.fields.push_back(field);
    }

    dpp::message msg;
    msg.add_embed(embed);

    if (state.total_pages > 1) {
        msg.add_component(build_pagination_row("top_", state.current_page, state.total_pages, false));
    }

    return msg;
}

} // namespace services
