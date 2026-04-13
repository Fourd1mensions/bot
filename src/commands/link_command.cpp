#include "commands/link_command.h"
#include "services/service_container.h"
#include "database.h"
#include "cache.h"
#include "utils.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace commands {

std::vector<std::string> LinkCommand::get_aliases() const {
    return {"link", "линк"};
}

bool LinkCommand::matches(const CommandContext& ctx) const {
    auto check_boundary = [](const std::string& str, size_t len) {
        if (str.length() == len) return true;
        char next = str[len];
        return next == ' ' || next == '\t';
    };

    std::string p = ctx.prefix;
    if (ctx.content_lower.find(p + "link") == 0 && check_boundary(ctx.content_lower, p.size() + 4)) return true;
    // Cyrillic: tolower doesn't work with UTF-8, check both cases
    std::string cyr_lo = p + "линк", cyr_up = p + "ЛИНК";
    if ((ctx.content.find(cyr_lo) == 0 || ctx.content.find(cyr_up) == 0) && check_boundary(ctx.content, cyr_lo.size())) return true;
    return false;
}

void LinkCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        ctx.reply(dpp::message(":x: Internal service error."));
        return;
    }

    auto discord_id = ctx.author_id();

    // Check if user is already OAuth linked
    auto& db = db::Database::instance();
    auto osu_id = db.get_osu_user_id(discord_id);
    bool is_oauth = osu_id.has_value() && db.is_user_oauth_linked(discord_id);

    if (is_oauth) {
        auto embed = dpp::embed()
            .set_color(0x66bb6a)
            .set_title("Already Linked")
            .set_description("Your osu! account is already linked via OAuth.\n\nUse `/unlink` to unlink your account.");

        dpp::message msg;
        msg.add_embed(embed);
        ctx.reply(msg);
        return;
    }

    // Generate link token
    std::string token;
    std::string link_url;
    try {
        auto& mc = cache::MemcachedCache::instance();
        token = utils::generate_secure_token();
        link_url = s->config.public_url + "/osu/link/" + token;

        json token_data;
        token_data["discord_id"] = discord_id.str();
        token_data["link_url"] = link_url;
        mc.set("osu_link_token:" + token, token_data.dump(), std::chrono::seconds(300));
    } catch (const std::exception& e) {
        spdlog::error("[link] Failed to generate link token: {}", e.what());
        ctx.reply(dpp::message(":x: Failed to generate link. Please try again."));
        return;
    }

    auto embed = dpp::embed()
        .set_color(0xff66aa)
        .set_title("Link your osu! Account")
        .set_description("Connect your osu! account to use bot commands.")
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
    ctx.reply(msg);
}

} // namespace commands
