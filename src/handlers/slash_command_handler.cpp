#include "handlers/slash_command_handler.h"
#include <requests.h>
#include <osu.h>
#include <database.h>
#include <cache.h>
#include <beatmap_downloader.h>
#include <error_messages.h>
#include <services/chat_context_service.h>
#include <services/user_mapping_service.h>
#include <services/beatmap_resolver_service.h>
#include <services/message_presenter_service.h>
#include <bot.h>  // For Random class

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#include <random>

using json = nlohmann::json;

namespace handlers {

SlashCommandHandler::SlashCommandHandler(
    commands::CommandRouter& command_router,
    Request& request,
    Random& rand,
    const Config& config,
    services::ChatContextService& chat_context_service,
    services::UserMappingService& user_mapping_service,
    services::BeatmapResolverService& beatmap_resolver_service,
    services::MessagePresenterService& message_presenter,
    BeatmapDownloader& beatmap_downloader,
    dpp::cluster& bot
)
    : command_router_(command_router)
    , request_(request)
    , rand_(rand)
    , config_(config)
    , chat_context_service_(chat_context_service)
    , user_mapping_service_(user_mapping_service)
    , beatmap_resolver_service_(beatmap_resolver_service)
    , message_presenter_(message_presenter)
    , beatmap_downloader_(beatmap_downloader)
    , bot_(bot)
{}

bool SlashCommandHandler::is_admin(const std::string& user_id) const {
    return std::find(config_.admin_users.begin(), config_.admin_users.end(), user_id) != config_.admin_users.end();
}

void SlashCommandHandler::handle(const dpp::slashcommand_t& event) {
    const std::string& cmd_name = event.command.get_command_name();
    const std::string& user_id = event.command.usr.id.str();
    const std::string& username = event.command.usr.username;
    const std::string& channel_id = event.command.channel_id.str();

    spdlog::info("[CMD] user={} ({}) channel={} command=/{}", user_id, username, channel_id, cmd_name);

    // Try to route to registered ICommand first
    if (command_router_.route_slash(event)) {
        return;
    }

    // Handle slash-only commands
    if (cmd_name == "гандон") {
        handle_gandon(event);
    } else if (cmd_name == "avatar") {
        handle_avatar(event);
    } else if (cmd_name == "update_token") {
        handle_update_token(event);
    } else if (cmd_name == "score") {
        handle_score(event);
    } else if (cmd_name == "autorole_switch") {
        handle_autorole_switch(event);
    } else if (cmd_name == "weather") {
        handle_weather(event);
    }
}

void SlashCommandHandler::register_commands(bool delete_existing) {
    if (delete_existing) {
        bot_.global_bulk_command_delete();
    }

    bot_.global_command_create(dpp::slashcommand("гандон", "Проверка", bot_.me.id));
    bot_.global_command_create(dpp::slashcommand("pages", "test", bot_.me.id));
    bot_.global_command_create(dpp::slashcommand("avatar", "Display osu! profile avatar", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "username", "osu! profile username", true))
    );
    bot_.global_command_create(dpp::slashcommand("update_token", "If peppy doesn't respond", bot_.me.id));
    bot_.global_command_create(dpp::slashcommand("autorole_switch", "Manage autorole issuance", bot_.me.id));
    bot_.global_command_create(dpp::slashcommand("weather", "Shows current weather", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "city", "Location", true))
    );

    // Commands mapped to text command implementations
    bot_.global_command_create(dpp::slashcommand("rs", "Show recent score", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "username", "osu! username", false))
        .add_option(dpp::command_option(dpp::co_integer, "index", "Score index (1-100)", false)
            .set_min_value(1).set_max_value(100))
        .add_option(dpp::command_option(dpp::co_string, "mode", "Game mode", false)
            .add_choice(dpp::command_option_choice("osu!", std::string("osu")))
            .add_choice(dpp::command_option_choice("taiko", std::string("taiko")))
            .add_choice(dpp::command_option_choice("catch", std::string("catch")))
            .add_choice(dpp::command_option_choice("mania", std::string("mania"))))
        .add_option(dpp::command_option(dpp::co_boolean, "best", "Show best scores instead of recent", false))
        .add_option(dpp::command_option(dpp::co_boolean, "pass", "Show only passed scores", false))
    );
    bot_.global_command_create(dpp::slashcommand("bg", "Show beatmap background", bot_.me.id));
    bot_.global_command_create(dpp::slashcommand("audio", "Get beatmap audio file", bot_.me.id));
    bot_.global_command_create(dpp::slashcommand("map", "Show beatmap info with PP values", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "mods", "Mods (e.g., HDDT)", false))
    );
    bot_.global_command_create(dpp::slashcommand("compare", "Compare your scores on current beatmap", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "username", "osu! username", false))
        .add_option(dpp::command_option(dpp::co_string, "mods", "Filter by mods (e.g., HDDT)", false))
    );
    bot_.global_command_create(dpp::slashcommand("sim", "Simulate PP for accuracy", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_number, "accuracy", "Accuracy % (e.g., 99.5)", true)
            .set_min_value(0.0).set_max_value(100.0))
        .add_option(dpp::command_option(dpp::co_string, "mods", "Mods (e.g., HDDT)", false))
        .add_option(dpp::command_option(dpp::co_string, "mode", "Game mode", false)
            .add_choice(dpp::command_option_choice("osu!", std::string("osu")))
            .add_choice(dpp::command_option_choice("taiko", std::string("taiko")))
            .add_choice(dpp::command_option_choice("catch", std::string("catch")))
            .add_choice(dpp::command_option_choice("mania", std::string("mania"))))
        .add_option(dpp::command_option(dpp::co_integer, "combo", "Max combo achieved", false))
        .add_option(dpp::command_option(dpp::co_integer, "n100", "Number of 100s", false))
        .add_option(dpp::command_option(dpp::co_integer, "n50", "Number of 50s", false))
        .add_option(dpp::command_option(dpp::co_integer, "misses", "Number of misses", false))
    );
    bot_.global_command_create(dpp::slashcommand("lb", "Show server leaderboard for beatmap", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "mods", "Filter by mods (e.g., HDDT)", false))
        .add_option(dpp::command_option(dpp::co_string, "sort", "Sort method", false)
            .add_choice(dpp::command_option_choice("PP", std::string("pp")))
            .add_choice(dpp::command_option_choice("Score", std::string("score")))
            .add_choice(dpp::command_option_choice("Accuracy", std::string("acc")))
            .add_choice(dpp::command_option_choice("Combo", std::string("combo")))
            .add_choice(dpp::command_option_choice("Date", std::string("date"))))
        .add_option(dpp::command_option(dpp::co_string, "beatmap", "Beatmap URL or ID", false))
    );

    bot_.global_command_create(dpp::slashcommand("settings", "Configure bot preferences", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "embed_preset", "Embed layout preset", false)
            .add_choice(dpp::command_option_choice("Compact", std::string("compact")))
            .add_choice(dpp::command_option_choice("Classic", std::string("classic")))
            .add_choice(dpp::command_option_choice("Extended", std::string("extended"))))
    );

    bot_.global_command_create(dpp::slashcommand("osc", "Show score rank counts from osustats", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "username", "osu! username", false))
        .add_option(dpp::command_option(dpp::co_string, "mode", "Game mode", false)
            .add_choice(dpp::command_option_choice("osu!", std::string("osu")))
            .add_choice(dpp::command_option_choice("taiko", std::string("taiko")))
            .add_choice(dpp::command_option_choice("catch", std::string("catch")))
            .add_choice(dpp::command_option_choice("mania", std::string("mania"))))
    );

    bot_.global_command_create(dpp::slashcommand("osu", "Display user profile statistics", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "username", "osu! username", false))
        .add_option(dpp::command_option(dpp::co_string, "mode", "Game mode", false)
            .add_choice(dpp::command_option_choice("osu!", std::string("osu")))
            .add_choice(dpp::command_option_choice("taiko", std::string("taiko")))
            .add_choice(dpp::command_option_choice("catch", std::string("fruits")))
            .add_choice(dpp::command_option_choice("mania", std::string("mania"))))
    );

    bot_.global_command_create(dpp::slashcommand("top", "Show user's top plays", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "username", "osu! username", false))
        .add_option(dpp::command_option(dpp::co_string, "mode", "Game mode", false)
            .add_choice(dpp::command_option_choice("osu!", std::string("osu")))
            .add_choice(dpp::command_option_choice("taiko", std::string("taiko")))
            .add_choice(dpp::command_option_choice("catch", std::string("fruits")))
            .add_choice(dpp::command_option_choice("mania", std::string("mania"))))
        .add_option(dpp::command_option(dpp::co_string, "mods", "Filter by mods (e.g., HDDT or -HR to exclude)", false))
        .add_option(dpp::command_option(dpp::co_string, "grade", "Filter by grade", false)
            .add_choice(dpp::command_option_choice("SS", std::string("SS")))
            .add_choice(dpp::command_option_choice("S", std::string("S")))
            .add_choice(dpp::command_option_choice("A", std::string("A")))
            .add_choice(dpp::command_option_choice("B", std::string("B")))
            .add_choice(dpp::command_option_choice("C", std::string("C")))
            .add_choice(dpp::command_option_choice("D", std::string("D"))))
        .add_option(dpp::command_option(dpp::co_string, "sort", "Sort method", false)
            .add_choice(dpp::command_option_choice("PP", std::string("pp")))
            .add_choice(dpp::command_option_choice("Accuracy", std::string("acc")))
            .add_choice(dpp::command_option_choice("Score", std::string("score")))
            .add_choice(dpp::command_option_choice("Combo", std::string("combo")))
            .add_choice(dpp::command_option_choice("Date", std::string("date")))
            .add_choice(dpp::command_option_choice("Misses", std::string("misses"))))
        .add_option(dpp::command_option(dpp::co_boolean, "reverse", "Reverse sort order", false))
        .add_option(dpp::command_option(dpp::co_integer, "index", "Show specific score (1-100)", false)
            .set_min_value(1).set_max_value(100))
    );

    bot_.global_command_create(dpp::slashcommand("link", "Link your osu! account", bot_.me.id));
}

void SlashCommandHandler::handle_gandon(const dpp::slashcommand_t& event) {
    float_t f = rand_.get_real(0.0f, 100.0f);
    auto embed = dpp::embed()
        .set_color(dpp::colors::cream)
        .set_title("Тест на гандона")
        .set_description(fmt::format("**Вы гандон на {:.2f}%**", f))
        .set_timestamp(time(0));
    dpp::message msg(event.command.channel_id, embed);
    event.reply(msg);
}

void SlashCommandHandler::handle_avatar(const dpp::slashcommand_t& event) {
    std::string username;
    try {
        username = std::get<std::string>(event.get_parameter("username"));
    } catch (const std::exception& e) {
        event.edit_original_response(dpp::message("Username parameter is required."));
        return;
    }
    std::string userid = request_.get_userid_v1(username);
    if (userid.empty()) {
        event.edit_original_response(dpp::message(fmt::format("User '{}' not found.", username)));
        return;
    }
    auto embed = dpp::embed()
        .set_color(dpp::colors::cream)
        .set_author(username, "https://osu.ppy.sh/users/" + userid, "")
        .set_image("https://a.ppy.sh/" + userid)
        .set_timestamp(time(0));
    dpp::message msg(event.command.channel_id, embed);
    event.edit_original_response(msg);
}

void SlashCommandHandler::handle_update_token(const dpp::slashcommand_t& event) {
    auto invoker_id = event.command.usr.id.str();
    if (!is_admin(invoker_id)) {
        event.reply(dpp::message("<:FRICK:1241513672480653475>"));
        return;
    }
    if (request_.update_token())
        event.reply(dpp::message("Token update - success"));
    else
        event.reply(dpp::message("Token update - fail"));
}

void SlashCommandHandler::handle_score(const dpp::slashcommand_t& event) {
    auto user_opt = user_mapping_service_.get_osu_id(event.command.usr.id.str());
    if (!user_opt.has_value()) {
        // Generate link token and show options
        std::string token;
        try {
            auto& mc = cache::MemcachedCache::instance();
            token = utils::generate_secure_token();

            json token_data;
            token_data["discord_id"] = event.command.usr.id.str();
            token_data["link_url"] = config_.public_url + "/osu/link/" + token;
            mc.set("osu_link_token:" + token, token_data.dump(), std::chrono::seconds(300));
        } catch (const std::exception& e) {
            spdlog::error("[score] Failed to generate link token: {}", e.what());
        }

        auto embed = dpp::embed()
            .set_color(0xff66aa)
            .set_title("Link your osu! Account")
            .set_description("Link your osu! account to use this command.")
            .add_field("Option 1: Website", fmt::format("[Open Settings]({})", config_.public_url + "/osu/settings"), true)
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
        event.reply(msg);
        return;
    }
    const auto& user = user_opt.value();

    std::string stored_id = chat_context_service_.get_beatmap_id(event.command.channel_id);
    std::string beatmap_id = beatmap_resolver_service_.resolve_beatmap_id(stored_id);
    if (beatmap_id.empty()) {
        event.edit_original_response(message_presenter_.build_error_message(error_messages::NO_BEATMAP_IN_CHANNEL));
        return;
    }

    auto start = std::chrono::steady_clock::now();

    std::string response_beatmap = request_.get_beatmap(beatmap_id);
    std::string response_score = request_.get_user_beatmap_score(beatmap_id, user);

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    if (response_score.empty() || response_beatmap.empty()) {
        if (elapsed > 8) {
            event.edit_original_response(message_presenter_.build_error_message(
                fmt::format(error_messages::API_TIMEOUT_FORMAT, elapsed)));
        } else {
            event.edit_original_response(dpp::message("Can't find score on this map."));
        }
        return;
    }

    Score score(response_score);
    Beatmap beatmap(response_beatmap);

    // Download .osz file asynchronously
    uint32_t beatmapset_id = beatmap.get_beatmapset_id();
    std::jthread([this, beatmapset_id]() {
        beatmap_downloader_.download_osz(beatmapset_id);
    }).detach();

    auto embed = dpp::embed()
        .set_color(dpp::colors::cream)
        .set_title(beatmap.to_string())
        .set_url(beatmap.get_beatmap_url())
        .set_thumbnail(beatmap.get_image_url())
        .add_field("Your best score on map", score.to_string(beatmap.get_max_combo()));

    if (elapsed > 8) {
        spdlog::warn("[CMD] /score took {}s to complete (slow API response)", elapsed);
    }

    event.edit_original_response(dpp::message(event.command.channel_id, embed));
}

void SlashCommandHandler::handle_autorole_switch(const dpp::slashcommand_t& event) {
    auto invoker_id = event.command.usr.id.str();
    if (!is_admin(invoker_id)) {
        event.reply(dpp::message("<:FRICK:1241513672480653475>"));
        return;
    }
    if (autorole_enabled_) {
        autorole_enabled_ = false;
        event.reply("Giving autorole switched to off");
    } else {
        autorole_enabled_ = true;
        event.reply("Giving autorole switched to on");
    }
}

void SlashCommandHandler::handle_weather(const dpp::slashcommand_t& event) {
    std::string city;
    try {
        city = std::get<std::string>(event.get_parameter("city"));
    } catch (const std::exception& e) {
        event.edit_original_response(dpp::message("City parameter is required."));
        return;
    }
    std::string w = request_.get_weather(city);

    // Check if response is empty before parsing (happens on 404 or other errors)
    if (w.empty()) {
        event.edit_original_response(dpp::message("City not found. Please check the spelling and try again."));
        return;
    }

    try {
        json j = json::parse(w);

        if (j.empty() || j.is_null()) {
            event.edit_original_response(dpp::message("Failed to get weather data. Please try again later."));
            return;
        }

        std::string c = j.value("name", "Unknown");
        std::string desc = j["weather"].at(0).value("description", "");
        double temp = j["main"].value("temp", 0.0);
        double feels = j["main"].value("feels_like", 0.0);
        int humidity = j["main"].value("humidity", 0);
        double wind = j["wind"].value("speed", 0.0);

        std::time_t timestamp = j.value("dt", 0);
        timestamp += j.value("timezone", 0);
        std::tm tm = *std::gmtime(&timestamp);
        std::ostringstream time;
        time << std::put_time(&tm, "%d.%m.%Y %H:%M");

        auto embed = dpp::embed()
            .set_color(dpp::colors::cream)
            .set_title(c + " - " + desc)
            .add_field("Температура", fmt::format("{:.1f}°C, ощущается как {:.1f}°C", temp, feels))
            .add_field("Влажность", fmt::format("{}%", humidity), true)
            .add_field("Ветер", fmt::format("{:.1f}м/с", wind), true)
            .set_footer(dpp::embed_footer().set_text(time.str()));

        event.edit_original_response(dpp::message(event.command.channel_id, embed));
    } catch (const json::exception& e) {
        spdlog::error("[CMD] Failed to parse weather data for '{}': {}", city, e.what());
        event.edit_original_response(dpp::message("Failed to parse weather data. Please try again later."));
    }
}

} // namespace handlers
