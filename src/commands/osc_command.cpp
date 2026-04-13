#include "commands/osc_command.h"
#include "services/service_container.h"
#include "services/user_resolver_service.h"
#include "services/message_presenter_service.h"
#include "services/embed_template_service.h"
#include "services/user_settings_service.h"
#include <requests.h>
#include <database.h>
#include <cache.h>
#include <utils.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <random>

namespace commands {

std::vector<std::string> OscCommand::get_aliases() const {
    return {"osc"};
}

void OscCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[osc] ServiceContainer is null");
        return;
    }

    // Show typing indicator (only for text commands)
    if (!ctx.is_slash()) {
        s->bot.channel_typing(ctx.channel_id());
    }

    std::string username;
    std::string mode = "osu";
    int gamemode = 0;  // 0=osu, 1=taiko, 2=catch, 3=mania

    if (ctx.is_slash()) {
        // Parse slash command parameters
        if (auto u = ctx.get_string_param("username")) {
            username = *u;
        }
        if (auto m = ctx.get_string_param("mode")) {
            mode = *m;
        }
    } else {
        // Parse text command: !osc [username] [-m mode]
        std::string content = ctx.content;

        // Find and extract mode flag
        size_t mode_pos = content.find("-m ");
        if (mode_pos != std::string::npos) {
            size_t mode_start = mode_pos + 3;
            while (mode_start < content.length() && content[mode_start] == ' ') {
                mode_start++;
            }
            size_t mode_end = content.find(' ', mode_start);
            if (mode_end == std::string::npos) {
                mode_end = content.length();
            }
            mode = content.substr(mode_start, mode_end - mode_start);
            std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
            // Remove the -m part from content
            content.erase(mode_pos, mode_end - mode_pos);
        }

        // Extract username (everything after !osc that's not a flag)
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

    // Convert mode string to gamemode int
    if (mode == "taiko") {
        gamemode = 1;
    } else if (mode == "catch" || mode == "fruits" || mode == "ctb") {
        gamemode = 2;
    } else if (mode == "mania") {
        gamemode = 3;
    } else {
        gamemode = 0;
        mode = "osu";
    }

    // Resolve username and get user profile
    std::string resolved_username = username;
    std::string user_json;
    int64_t osu_user_id = 0;
    size_t scores_first_count = 0;  // Top 1 count from profile
    double pp = 0.0;
    int global_rank = 0;
    int country_rank = 0;
    std::string country_code;

    if (resolved_username.empty()) {
        // Try to get from linked account
        auto resolve_result = s->user_resolver_service.resolve("", ctx.author_id());
        if (!resolve_result) {
            // Generate link token and show options
            std::string token;
            try {
                auto& mc = cache::MemcachedCache::instance();
                token = utils::generate_secure_token();

                nlohmann::json token_data;
                token_data["discord_id"] = ctx.author_id().str();
                token_data["link_url"] = s->config.public_url + "/osu/link/" + token;
                mc.set("osu_link_token:" + token, token_data.dump(), std::chrono::seconds(300));
            } catch (const std::exception& e) {
                spdlog::error("[osc] Failed to generate link token: {}", e.what());
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

        // Get user profile
        user_json = s->request.get_user(std::to_string(osu_user_id), true);
    } else {
        // Fetch user by username
        user_json = s->request.get_user(resolved_username, false);
    }

    if (user_json.empty()) {
        ctx.reply(s->message_presenter.build_error_message("Failed to fetch user information."));
        return;
    }

    try {
        auto j = nlohmann::json::parse(user_json);
        resolved_username = j.value("username", "");
        osu_user_id = j.value("id", 0);
        country_code = j.value("country_code", "");

        // Get scores_first_count (top 1s) from profile - more accurate than osustats
        if (j.contains("scores_first_count") && !j["scores_first_count"].is_null()) {
            scores_first_count = j["scores_first_count"].get<size_t>();
        }

        // Get statistics for author display
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
        }
    } catch (const std::exception& e) {
        spdlog::error("[osc] Failed to parse user info: {}", e.what());
        ctx.reply(s->message_presenter.build_error_message("Failed to parse user information."));
        return;
    }

    if (resolved_username.empty()) {
        ctx.reply(s->message_presenter.build_error_message("Could not determine username."));
        return;
    }

    // Fetch osustats counts (top8, top15, top25, top50, top100)
    spdlog::info("[osc] Fetching osustats counts for user={} mode={}", resolved_username, mode);
    auto counts = s->request.get_osustats_counts(resolved_username, gamemode);

    // Use top1 from user profile (more accurate, updates immediately)
    counts.top1 = scores_first_count;

    // Format mode for title (like Bathbot)
    std::string mode_prefix;
    switch (gamemode) {
        case 1: mode_prefix = "taiko "; break;
        case 2: mode_prefix = "ctb "; break;
        case 3: mode_prefix = "mania "; break;
        default: mode_prefix = ""; break;
    }

    // Build values map for template rendering
    std::unordered_map<std::string, std::string> values;
    values["username"] = resolved_username;
    values["user_id"] = std::to_string(osu_user_id);
    values["mode"] = mode_prefix;
    values["pp"] = fmt::format("{:.2f}", pp);
    values["global_rank"] = std::to_string(global_rank);
    values["country_code"] = country_code;
    values["country_rank"] = std::to_string(country_rank);
    values["top1"] = fmt::format("{:L}", counts.top1);
    values["top8"] = fmt::format("{:L}", counts.top8);
    values["top15"] = fmt::format("{:L}", counts.top15);
    values["top25"] = fmt::format("{:L}", counts.top25);
    values["top50"] = fmt::format("{:L}", counts.top50);
    values["top100"] = fmt::format("{:L}", counts.top100);

    // Get full embed template and render (supports custom preset)
    services::FullEmbedTemplate tmpl;
    if (s->template_service) {
        auto preset = s->user_settings_service.get_preset(ctx.author_id());
        if (preset == services::EmbedPreset::Custom) {
            tmpl = s->template_service->get_user_full_template(ctx.author_id(), "osc", "custom");
        } else {
            tmpl = s->template_service->get_full_template("osc");
        }
    } else {
        tmpl = services::get_default_full_template("osc");
    }

    dpp::embed embed = services::render_full_embed(tmpl, values);

    dpp::message msg;
    msg.add_embed(embed);
    ctx.reply(msg);
}

} // namespace commands
