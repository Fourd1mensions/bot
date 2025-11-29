#include "services/message_presenter_service.h"
#include "osu.h"
#include "state/session_state.h"
#include "utils.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

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
    const std::string& footer_text,
    const std::string& mods_filter,
    size_t total_pages,
    size_t current_page
) const {
    std::string title = beatmap.to_string();
    if (!mods_filter.empty()) {
        title += fmt::format(" +{}", mods_filter);
    }

    auto embed = dpp::embed()
        .set_color(dpp::colors::viola_purple)
        .set_title(title)
        .set_url(beatmap.get_beatmap_url())
        .set_thumbnail(beatmap.get_image_url())
        .set_footer(dpp::embed_footer().set_text(footer_text))
        .set_timestamp(time(0));

    for (const auto& score : scores_on_page) {
        dpp::embed_field field;
        field.name = fmt::format("{}) {}", score.rank, score.header);
        field.value = score.body;
        field.is_inline = false;
        embed.fields.push_back(field);
    }

    dpp::message msg;
    msg.add_embed(embed);

    // Add pagination buttons only if more than one page
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
    uint32_t modded_length
) const {
    RecentScoreCacheData data;

    // Build title with mods
    data.title = beatmap.to_string();
    if (!score.get_mods().empty() && score.get_mods() != "NM") {
        data.title += fmt::format(" +{}", score.get_mods());
    }

    data.url = beatmap.get_beatmap_url();
    data.thumbnail = beatmap.get_image_url();

    // Get rank emoji
    std::string emoji_id = get_rank_emoji(score.get_rank());

    // Build PP line
    std::string pp_line;
    if (pp_info.has_fc_pp && pp_info.fc_pp > pp_info.current_pp) {
        pp_line = fmt::format("▸ **{:.2f}PP** ({:.2f}PP for {:.2f}% FC) ▸ {:.2f}%",
            pp_info.current_pp,
            pp_info.fc_pp,
            pp_info.fc_accuracy,
            score.get_accuracy() * 100
        );
    } else {
        pp_line = fmt::format("▸ **{:.2f}PP** ▸ {:.2f}%",
            pp_info.current_pp,
            score.get_accuracy() * 100
        );
    }

    // Build rank line - only show completion% if < 100% or rank is F
    std::string rank_line;
    if (completion_percent < 100.0f || score.get_rank() == "F") {
        rank_line = fmt::format("▸ {} **({:.2f}%)**", emoji_id, completion_percent);
    } else {
        rank_line = fmt::format("▸ {}", emoji_id);
    }

    // Build description
    data.description = fmt::format(
        "{} {}\n▸ {:L} ▸ **x{}/{}** ▸ [{}/{}/{}/{}]",
        rank_line,
        pp_line,
        score.get_total_score(),
        score.get_max_combo(),
        beatmap.get_max_combo(),
        score.get_count_300(),
        score.get_count_100(),
        score.get_count_50(),
        score.get_count_miss()
    );

    // Build beatmap info line
    data.beatmap_info = fmt::format(
        "▸ {} BPM • {}:{:02d} ▸ AR `{:.1f}` OD `{:.1f}` CS `{:.1f}` HP `{:.1f}`",
        static_cast<int>(modded_bpm),
        modded_length / 60,
        modded_length % 60,
        difficulty.approach_rate,
        difficulty.overall_difficulty,
        difficulty.circle_size,
        difficulty.hp_drain_rate
    );

    // Build footer
    data.footer = fmt::format("{} score #{}/{}{}",
        score_type,
        pagination.current + 1,
        pagination.total,
        pagination.refresh_count > 0 ? fmt::format(" • refreshed {}x", pagination.refresh_count) : ""
    );

    data.timestamp = utils::ISO8601_to_UNIX(score.get_created_at());

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
    uint32_t modded_length
) const {
    // Build cache data (contains all computed strings)
    auto data = build_recent_score_cache_data(
        score, beatmap, difficulty, pp_info, pagination,
        score_type, completion_percent, modded_bpm, modded_length
    );

    auto embed = dpp::embed()
        .set_color(dpp::colors::viola_purple)
        .set_title(data.title)
        .set_url(data.url)
        .set_description(data.description)
        .set_thumbnail(data.thumbnail);

    embed.add_field("", data.beatmap_info, false);
    embed.set_footer(dpp::embed_footer().set_text(data.footer))
         .set_timestamp(data.timestamp);

    dpp::message msg;
    msg.add_embed(embed);

    // Add pagination buttons if there's more than one score
    if (pagination.total > 1) {
        msg.add_component(build_pagination_row("rs_", pagination.current, pagination.total, true));
    }

    return msg;
}

dpp::message MessagePresenterService::build_map_info(
    const Beatmap& beatmap,
    const DifficultyInfo& difficulty,
    const std::vector<double>& pp_values,
    const std::string& mods,
    uint32_t beatmapset_id,
    float modded_bpm,
    uint32_t modded_length
) const {
    std::string title = beatmap.to_string();
    if (!mods.empty()) {
        title += fmt::format(" +{}", mods);
    }

    dpp::embed embed;
    embed.set_title(title);
    embed.set_url(beatmap.get_beatmap_url());
    embed.set_image(beatmap.get_image_url());

    // Star rating in description
    std::string mode_display = beatmap.get_mode();
    std::transform(mode_display.begin(), mode_display.end(), mode_display.begin(), ::toupper);

    std::string description = fmt::format(":star: **{:.2f}★**", difficulty.star_rating);
    if (beatmap.get_mode() != "osu") {
        description += fmt::format(" [{}]", mode_display);
    }
    embed.set_description(description);

    // PP Values field
    std::vector<double> acc_levels = {90.0, 95.0, 99.0, 100.0};
    std::string pp_field;
    for (size_t i = 0; i < acc_levels.size() && i < pp_values.size(); i++) {
        pp_field += fmt::format("**{:.0f}%** — {:.0f}pp\n", acc_levels[i], pp_values[i]);
    }
    embed.add_field("Performance Points", pp_field, true);

    // Difficulty attributes field
    std::string diff_field = fmt::format(
        "**AR** {:.1f} • **OD** {:.1f}\n"
        "**CS** {:.1f} • **HP** {:.1f}\n"
        "**Aim** {:.2f}★ • **Speed** {:.2f}★\n"
        "**Max Combo:** {}x",
        difficulty.approach_rate,
        difficulty.overall_difficulty,
        difficulty.circle_size,
        difficulty.hp_drain_rate,
        difficulty.aim_difficulty,
        difficulty.speed_difficulty,
        difficulty.max_combo
    );
    embed.add_field("Difficulty", diff_field, true);

    // Map info field (BPM, length) - already adjusted for speed mods by caller
    int minutes = modded_length / 60;
    int seconds = modded_length % 60;
    std::string map_info = fmt::format(
        "**BPM:** {:.0f}\n**Length:** {}:{:02d}",
        modded_bpm,
        minutes,
        seconds
    );
    embed.add_field("Map Info", map_info, true);

    // Download links
    std::string download_links = fmt::format(
        "[osu!direct](https://osu.ppy.sh/d/{}) • "
        "[Kana](https://kana.nisemonic.net/osu/d/{}) • "
        "[Nerinyan](https://api.nerinyan.moe/d/{}) • "
        "[Catboy](https://catboy.best/d/{})",
        beatmapset_id, beatmapset_id, beatmapset_id, beatmapset_id
    );
    embed.add_field("Download", download_links, false);

    // Background and audio links
    std::string media_links = fmt::format(
        "[Background](https://kana.nisemonic.net/osu/bg/{}) • "
        "[Audio](https://kana.nisemonic.net/osu/audio/{})",
        beatmapset_id, beatmapset_id
    );
    embed.add_field("Media", media_links, false);

    // Color based on star rating
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

dpp::message MessagePresenterService::build_from_cache_data(
    const RecentScoreCacheData& cache_data,
    const PaginationInfo& pagination
) const {
    auto embed = dpp::embed()
        .set_color(dpp::colors::viola_purple)
        .set_title(cache_data.title)
        .set_url(cache_data.url)
        .set_description(cache_data.description)
        .set_thumbnail(cache_data.thumbnail);

    embed.add_field("", cache_data.beatmap_info, false);
    embed.set_footer(dpp::embed_footer().set_text(cache_data.footer))
         .set_timestamp(cache_data.timestamp);

    dpp::message msg;
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

} // namespace services
