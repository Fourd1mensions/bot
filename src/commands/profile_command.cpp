#include "commands/profile_command.h"
#include "services/service_container.h"
#include "services/user_resolver_service.h"
#include "services/message_presenter_service.h"
#include "services/embed_template_service.h"
#include "services/user_settings_service.h"
#include <requests.h>
#include <database.h>
#include <utils.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <cache.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <random>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace commands {

std::vector<std::string> ProfileCommand::get_aliases() const {
    return {"osu", "taiko", "mania", "ctb", "catch"};
}

void ProfileCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[profile] ServiceContainer is null");
        return;
    }

    // Show typing indicator
    if (!ctx.is_slash()) {
        s->bot.channel_typing(ctx.channel_id());
    }

    std::string username;
    std::string mode = "osu";

    if (ctx.is_slash()) {
        if (auto u = ctx.get_string_param("username")) {
            username = *u;
        }
        if (auto m = ctx.get_string_param("mode")) {
            mode = *m;
        }
    } else {
        // Determine mode from command alias
        std::string content = ctx.content;
        std::string p = ctx.prefix;
        if (content.find(p + "taiko") != std::string::npos) {
            mode = "taiko";
        } else if (content.find(p + "mania") != std::string::npos) {
            mode = "mania";
        } else if (content.find(p + "ctb") != std::string::npos || content.find(p + "catch") != std::string::npos) {
            mode = "fruits";
        }

        // Extract username (everything after command)
        size_t cmd_end = content.find(' ');
        if (cmd_end != std::string::npos) {
            username = content.substr(cmd_end + 1);
            // Trim whitespace
            size_t start = username.find_first_not_of(' ');
            size_t end = username.find_last_not_of(' ');
            if (start != std::string::npos && end != std::string::npos) {
                username = username.substr(start, end - start + 1);
            } else {
                username.clear();
            }
        }
    }

    show_profile(ctx, username, mode);
}

void ProfileCommand::show_profile(const UnifiedContext& ctx, const std::string& username, const std::string& mode) {
    auto* s = ctx.services;

    // Resolve username
    std::string resolved_username = username;
    std::string user_json;
    int64_t osu_user_id = 0;

    // Convert mode for API (catch -> fruits)
    std::string api_mode = mode;
    if (mode == "catch") api_mode = "fruits";

    if (resolved_username.empty()) {
        auto resolve_result = s->user_resolver_service.resolve("", ctx.author_id());
        if (!resolve_result) {
            // Generate link token and show options
            std::string token;
            std::string link_url;
            try {
                auto& mc = cache::MemcachedCache::instance();
                token = utils::generate_secure_token();
                link_url = s->config.public_url + "/osu/link/" + token;

                json token_data;
                token_data["discord_id"] = ctx.author_id().str();
                token_data["link_url"] = link_url;
                mc.set("osu_link_token:" + token, token_data.dump(), std::chrono::seconds(300));
            } catch (const std::exception& e) {
                spdlog::error("[profile] Failed to generate link token: {}", e.what());
            }

            auto embed = dpp::embed()
                .set_color(0xff66aa)
                .set_title("Link your osu! Account")
                .set_description("Link your osu! account to use commands without specifying your username.")
                .add_field("Option 1: Website", fmt::format("[Open Settings]({})", s->config.public_url + "/osu/settings"), true)
                .add_field("Option 2: Direct Link", "Click the button below to get a link in DMs", true)
                .set_footer(dpp::embed_footer().set_text("Link expires in 5 minutes"));

            dpp::message msg;
            msg.add_embed(embed);
            if (!token.empty()) {
                msg.add_component(
                    dpp::component().add_component(
                        dpp::component()
                            .set_type(dpp::cot_button)
                            .set_label("Send Link to DMs")
                            .set_style(dpp::cos_primary)
                            .set_id("osu_link_dm:" + token)
                    )
                );
            }
            ctx.reply(std::move(msg));
            return;
        }
        osu_user_id = resolve_result.osu_user_id;
        user_json = s->request.get_user(std::to_string(osu_user_id), true, api_mode);
    } else {
        user_json = s->request.get_user(resolved_username, false, api_mode);
    }

    // Check if user should be prompted to OAuth link (only if using own account)
    bool suggest_oauth_link = false;
    if (resolved_username.empty()) {
        auto& db = db::Database::instance();
        if (!db.is_user_oauth_linked(ctx.author_id())) {
            suggest_oauth_link = true;
        }
    }

    if (user_json.empty()) {
        ctx.reply(s->message_presenter.build_error_message("User not found."));
        return;
    }

    // Parse user data
    json j;
    try {
        j = json::parse(user_json);
    } catch (const std::exception& e) {
        spdlog::error("[profile] Failed to parse user JSON: {}", e.what());
        ctx.reply(s->message_presenter.build_error_message("Failed to parse user data."));
        return;
    }

    resolved_username = j.value("username", "");
    osu_user_id = j.value("id", 0);
    std::string country_code = j.value("country_code", "");
    std::string avatar_url = j.value("avatar_url", "");

    // Get statistics
    double pp = 0.0;
    int global_rank = 0;
    int country_rank = 0;
    double accuracy = 0.0;
    int playcount = 0;
    int playtime_seconds = 0;
    int64_t ranked_score = 0;
    int64_t total_score = 0;
    int level = 0;
    int level_progress = 0;
    int max_combo = 0;
    int replays_watched = 0;

    // Grade counts
    int ssh_count = 0, ss_count = 0, sh_count = 0, s_count = 0, a_count = 0;

    if (j.contains("statistics") && j["statistics"].is_object()) {
        const auto& stats = j["statistics"];
        if (stats.contains("pp") && !stats["pp"].is_null()) {
            pp = stats["pp"].get<double>();
        }
        if (stats.contains("global_rank") && !stats["global_rank"].is_null()) {
            global_rank = stats["global_rank"].get<int>();
        }
        if (stats.contains("country_rank") && !stats["country_rank"].is_null()) {
            country_rank = stats["country_rank"].get<int>();
        }
        if (stats.contains("hit_accuracy") && !stats["hit_accuracy"].is_null()) {
            accuracy = stats["hit_accuracy"].get<double>();
        }
        if (stats.contains("play_count") && !stats["play_count"].is_null()) {
            playcount = stats["play_count"].get<int>();
        }
        if (stats.contains("play_time") && !stats["play_time"].is_null()) {
            playtime_seconds = stats["play_time"].get<int>();
        }
        if (stats.contains("ranked_score") && !stats["ranked_score"].is_null()) {
            ranked_score = stats["ranked_score"].get<int64_t>();
        }
        if (stats.contains("total_score") && !stats["total_score"].is_null()) {
            total_score = stats["total_score"].get<int64_t>();
        }
        if (stats.contains("maximum_combo") && !stats["maximum_combo"].is_null()) {
            max_combo = stats["maximum_combo"].get<int>();
        }
        if (stats.contains("replays_watched_by_others") && !stats["replays_watched_by_others"].is_null()) {
            replays_watched = stats["replays_watched_by_others"].get<int>();
        }
        if (stats.contains("level") && stats["level"].is_object()) {
            level = stats["level"].value("current", 0);
            level_progress = stats["level"].value("progress", 0);
        }
        if (stats.contains("grade_counts") && stats["grade_counts"].is_object()) {
            const auto& grades = stats["grade_counts"];
            ssh_count = grades.value("ssh", 0);
            ss_count = grades.value("ss", 0);
            sh_count = grades.value("sh", 0);
            s_count = grades.value("s", 0);
            a_count = grades.value("a", 0);
        }
    }

    // Get medal count and join date
    int medal_count = 0;
    if (j.contains("user_achievements") && j["user_achievements"].is_array()) {
        medal_count = static_cast<int>(j["user_achievements"].size());
    }

    std::string join_date = j.value("join_date", "");

    // Peak rank
    int peak_rank = 0;
    std::string peak_date;
    if (j.contains("rank_highest") && j["rank_highest"].is_object() && !j["rank_highest"].is_null()) {
        peak_rank = j["rank_highest"].value("rank", 0);
        peak_date = j["rank_highest"].value("updated_at", "");
    }

    // Format mode display
    std::string mode_display;
    if (mode == "osu") mode_display = "osu!";
    else if (mode == "taiko") mode_display = "osu!taiko";
    else if (mode == "fruits" || mode == "catch") mode_display = "osu!catch";
    else if (mode == "mania") mode_display = "osu!mania";
    else mode_display = "osu!";

    int playtime_hours = playtime_seconds / 3600;

    // Calculate join date values
    int64_t join_timestamp = 0;
    std::string join_duration;

    if (!join_date.empty()) {
        // Parse ISO8601 date manually (format: "2015-10-05T14:21:23+00:00" or "2015-10-05T14:21:23Z")
        std::tm tm = {};
        std::string date_copy = join_date;
        // Replace +00:00 with Z for simpler parsing
        size_t plus_pos = date_copy.find('+');
        if (plus_pos != std::string::npos) {
            date_copy = date_copy.substr(0, plus_pos) + "Z";
        }

        std::istringstream ss(date_copy);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");

        if (!ss.fail()) {
            join_timestamp = static_cast<int64_t>(timegm(&tm));
            if (join_timestamp > 0) {
                int64_t now = static_cast<int64_t>(std::time(nullptr));
                int64_t join_days = (now - join_timestamp) / 86400;
                int join_years = static_cast<int>(join_days / 365);
                int join_months = static_cast<int>((join_days % 365) / 30);

                // Format join duration string
                if (join_years > 0) {
                    join_duration = fmt::format("{} year{} {} month{}",
                        join_years, join_years == 1 ? "" : "s",
                        join_months, join_months == 1 ? "" : "s");
                } else if (join_months > 0) {
                    join_duration = fmt::format("{} month{}", join_months, join_months == 1 ? "" : "s");
                } else {
                    join_duration = fmt::format("{} day{}", join_days, join_days == 1 ? "" : "s");
                }
            }
        }
    }

    // Build values map for template rendering
    std::unordered_map<std::string, std::string> values;
    values["username"] = resolved_username;
    values["user_id"] = std::to_string(osu_user_id);
    values["pp"] = fmt::format("{:.2f}", pp);
    values["global_rank"] = std::to_string(global_rank);
    values["country_code"] = country_code;
    values["country_rank"] = std::to_string(country_rank);
    values["accuracy"] = fmt::format("{:.2f}", accuracy);
    values["level"] = std::to_string(level);
    values["level_progress"] = fmt::format("{:02d}", level_progress);
    values["playcount"] = fmt::format("{:L}", playcount);
    values["playtime_hours"] = fmt::format("{:L}", playtime_hours);
    values["medal_count"] = std::to_string(medal_count);
    values["mode"] = mode_display;
    values["join_duration"] = join_duration;
    values["join_date_unix"] = std::to_string(join_timestamp);

    // Peak rank values
    if (peak_rank > 0 && !peak_date.empty()) {
        int64_t peak_timestamp = utils::ISO8601_to_UNIX(peak_date);
        values["peak_rank"] = fmt::format("{:L}", peak_rank);
        values["peak_date"] = std::to_string(peak_timestamp);
    } else {
        values["peak_rank"] = "";
        values["peak_date"] = "";
    }

    // Get full embed template and render (supports custom preset)
    services::FullEmbedTemplate tmpl;
    if (s->template_service) {
        auto preset = s->user_settings_service.get_preset(ctx.author_id());
        if (preset == services::EmbedPreset::Custom) {
            tmpl = s->template_service->get_user_full_template(ctx.author_id(), "profile", "custom");
        } else {
            tmpl = s->template_service->get_full_template("profile");
        }
    } else {
        tmpl = services::get_default_full_template("profile");
    }

    dpp::embed embed = services::render_full_embed(tmpl, values);

    dpp::message msg;
    msg.add_embed(embed);

    // Add OAuth link suggestion if user is not OAuth linked
    if (suggest_oauth_link) {
        std::string hint = fmt::format("-# Link your osu! account with `{}link` or `/link` for a better experience", ctx.prefix);
        if (msg.content.empty()) {
            msg.content = hint;
        } else {
            msg.content = hint + "\n" + msg.content;
        }
    }

    ctx.reply(msg);
}

} // namespace commands
