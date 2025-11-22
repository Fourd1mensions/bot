#include "fmt/format.h"
#include "osu.h"
#include <bot.h>
#include <requests.h>
#include <utils.h>

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <thread>
#include <type_traits>

#include <fmt/base.h>
#include <spdlog/spdlog.h>

namespace stdr = std::ranges;

template <typename T>
T Random::get_real(T min, T max) {
  static_assert(std::is_floating_point<T>::value, "Type must be a floating-point type");
  std::uniform_real_distribution<> distr(min, max);
  return distr(_gen);
}

template <typename T>
T Random::get_int(T min, T max) {
  static_assert(std::is_integral<T>::value, "Type must be an integral type");
  std::uniform_int_distribution<> distr(min, max);
  return distr(_gen);
}

bool Random::get_bool() {
  std::bernoulli_distribution distr(0.5);
  return distr(_gen);
}

void Bot::update_chat_map(const std::string& msg, const dpp::snowflake& channel_id, const dpp::snowflake& msg_id) {
  std::regex url_reg(R"(https:\/\/osu\.ppy\.sh\/(beatmapsets\/\d+\/?#(?:osu|taiko|fruits|mania)\/|beatmaps\/|b\/)(\d+))");
  std::smatch m;

  if (std::regex_search(msg, m, url_reg) && m.size() > 2) {
    auto& p = chat_map[channel_id];
    p.first = msg_id;
    p.second = m.str(2);
  }
}

dpp::message Bot::build_lb_page(const LeaderboardState& state) {
  constexpr size_t SCORES_PER_PAGE = 5;

  auto embed = dpp::embed()
    .set_color(dpp::colors::viola_purple)
    .set_title(state.beatmap.to_string())
    .set_url(state.beatmap.get_beatmap_url())
    .set_thumbnail(state.beatmap.get_image_url())
    .set_footer(dpp::embed_footer().set_text(
      fmt::format("Page {}/{} • {} total scores",
        state.current_page + 1,
        state.total_pages,
        state.scores.size())
    ));

  size_t start = state.current_page * SCORES_PER_PAGE;
  size_t end = std::min(start + SCORES_PER_PAGE, state.scores.size());

  for (size_t i = start; i < end; i++) {
    dpp::embed_field field;
    field.name      = fmt::format("{}) {}", i + 1, state.scores[i].get_header());
    field.value     = state.scores[i].get_body(state.beatmap.get_max_combo());
    field.is_inline = false;
    embed.fields.push_back(field);
  }

  dpp::message msg;
  msg.add_embed(embed);

  // Add pagination buttons if there's more than one page
  if (state.total_pages > 1) {
    dpp::component action_row;
    action_row.set_type(dpp::cot_action_row);

    dpp::component prev_button;
    prev_button.set_type(dpp::cot_button)
      .set_id("lb_prev")
      .set_label("Previous")
      .set_style(dpp::cos_primary)
      .set_emoji("⬅️")
      .set_disabled(state.current_page == 0);

    dpp::component page_indicator;
    page_indicator.set_type(dpp::cot_button)
      .set_id("lb_page_info")
      .set_label(fmt::format("{}/{}", state.current_page + 1, state.total_pages))
      .set_style(dpp::cos_secondary)
      .set_disabled(true);

    dpp::component next_button;
    next_button.set_type(dpp::cot_button)
      .set_id("lb_next")
      .set_label("Next")
      .set_style(dpp::cos_primary)
      .set_emoji("➡️")
      .set_disabled(state.current_page >= state.total_pages - 1);

    action_row.add_component(prev_button);
    action_row.add_component(page_indicator);
    action_row.add_component(next_button);

    msg.add_component(action_row);
  }

  return msg;
}

void Bot::invalidate_leaderboard(dpp::snowflake channel_id, dpp::snowflake message_id) {
  auto it = leaderboard_states.find(message_id);
  if (it == leaderboard_states.end()) {
    return; // Already cleaned up
  }

  const auto& state = it->second;

  // Build message without components
  auto embed = dpp::embed()
    .set_color(dpp::colors::viola_purple)
    .set_title(state.beatmap.to_string())
    .set_url(state.beatmap.get_beatmap_url())
    .set_thumbnail(state.beatmap.get_image_url())
    .set_footer(dpp::embed_footer().set_text(
      fmt::format("Page {}/{} • {} total scores • Pagination expired",
        state.current_page + 1,
        state.total_pages,
        state.scores.size())
    ));

  constexpr size_t SCORES_PER_PAGE = 5;
  size_t start = state.current_page * SCORES_PER_PAGE;
  size_t end = std::min(start + SCORES_PER_PAGE, state.scores.size());

  for (size_t i = start; i < end; i++) {
    dpp::embed_field field;
    field.name      = fmt::format("{}) {}", i + 1, state.scores[i].get_header());
    field.value     = state.scores[i].get_body(state.beatmap.get_max_combo());
    field.is_inline = false;
    embed.fields.push_back(field);
  }

  dpp::message msg(channel_id, embed);
  msg.id = message_id;

  // Edit message to remove components
  bot.message_edit(msg, [this, message_id](const dpp::confirmation_callback_t& callback) {
    if (!callback.is_error()) {
      // Clean up state
      leaderboard_states.erase(message_id);
      spdlog::info("Leaderboard pagination expired for message {}", message_id.str());
    }
  });
}

void Bot::button_click_event(const dpp::button_click_t& event) {
  const std::string& button_id = event.custom_id;

  // Handle leaderboard pagination
  if (button_id == "lb_prev" || button_id == "lb_next") {
    auto msg_id = event.command.message_id;
    auto it = leaderboard_states.find(msg_id);

    if (it == leaderboard_states.end()) {
      event.reply(dpp::ir_channel_message_with_source,
        dpp::message("Leaderboard data expired. Please run !lb again.").set_flags(dpp::m_ephemeral));
      return;
    }

    auto& state = it->second;

    // Update page number
    if (button_id == "lb_prev" && state.current_page > 0) {
      state.current_page--;
    } else if (button_id == "lb_next" && state.current_page < state.total_pages - 1) {
      state.current_page++;
    } else {
      // Button shouldn't be clickable if at boundary, but just in case
      return;
    }

    // Build updated message with new page
    dpp::message updated_msg = build_lb_page(state);

    // Update the message
    event.reply(dpp::ir_update_message, updated_msg);
  }
}

void Bot::create_lb_message(const dpp::message_create_t& event) {
  const std::string& channel_id = event.msg.channel_id.str(); 
  const std::string& beatmap_id = chat_map[channel_id].second;

  dpp::message m{};
  m.to_json();

  if (beatmap_id.empty()) {
    event.reply(dpp::message("Can't find the map. Please send the map link and use this command again."));
    return;
  }

  std::string response_beatmap = request.get_beatmap(beatmap_id);
  if (response_beatmap.empty()) {
    spdlog::error("Unable to send request");
    event.reply(dpp::message("Peppy didn't respond"));
    return;
  }

  Beatmap            beatmap(response_beatmap);
  std::vector<Score> scores(disid_osuid_map.size());

  // Create stable index mapping to avoid race condition
  std::unordered_map<std::string, size_t> user_to_index;
  size_t idx = 0;
  for (const auto& [dis_id, user_id] : disid_osuid_map) {
    user_to_index[user_id] = idx++;
  }

  // force tbb parallelization ???
  arena.execute([&]() { tbb::parallel_for_each(std::begin(disid_osuid_map), std::end(disid_osuid_map),
    [&](const auto& pair) {
      const auto& [dis_id, user_id] = pair;
      auto& score = scores[user_to_index[user_id]];
      std::string scores_j = request.get_user_beatmap_score(beatmap_id, user_id, true);
      if (!scores_j.empty()) {
        json j = json::parse(scores_j);
        j = j["scores"];
        // sort specific user's scores
        std::sort(j.begin(), j.end(), [](const json& a, const json& b) {
          return std::make_tuple(a["pp"], a["score"]) > std::make_tuple(b["pp"], b["score"]);
        });
        score.from_json(j.at(0));
        std::string usr_j = request.get_user(fmt::format("{}", score.get_user_id()), true); // TODO: store usernames
        json usr = json::parse(usr_j);
        score.set_username(usr.at("username"));
      }
    });
  });
  for (auto it = scores.begin(); it != scores.end();) {
    if (!it->is_empty) ++it;
    else scores.erase(it);
  }

  if (scores.empty()) {
    event.reply(dpp::message("Can't find any scores on " + beatmap.to_string()));
    return;
  }
  // sort best user scores
  if (scores.size() > 1) {
    stdr::sort(scores, [](const Score& a, const Score& b) {
      return std::make_tuple(a.get_pp(), a.get_total_score()) >
          std::make_tuple(b.get_pp(), b.get_total_score());
    });
  }

  // Create leaderboard state and build first page
  LeaderboardState lb_state(std::move(scores), std::move(beatmap), 0);
  dpp::message msg = build_lb_page(lb_state);

  // Reply and store the state for pagination
  event.reply(msg, false, [this, lb_state = std::move(lb_state)](const dpp::confirmation_callback_t& callback) {
    if (callback.is_error()) {
      spdlog::error("Failed to send leaderboard message");
      return;
    }
    // Store the state using the bot's reply message ID
    auto reply_msg = callback.get<dpp::message>();
    leaderboard_states[reply_msg.id] = lb_state;

    // Schedule invalidation after 5 minutes (only if there are multiple pages)
    if (lb_state.total_pages > 1) {
      dpp::snowflake msg_id = reply_msg.id;
      dpp::snowflake chan_id = reply_msg.channel_id;

      std::jthread([this, msg_id, chan_id]() {
        std::this_thread::sleep_for(std::chrono::minutes(5));
        invalidate_leaderboard(chan_id, msg_id);
      }).detach();
    }
  });
}

void Bot::message_create_event(const dpp::message_create_t& event) {
  fmt::print("{}: {}\n", event.msg.author.username, event.msg.content);

  std::lock_guard<std::mutex> lock(mutex);
  update_chat_map(event.raw_event, event.msg.channel_id.str(), event.msg.id.str());

  if (event.msg.content.find("!lb") == 0) {
    std::jthread(&Bot::create_lb_message, this, std::move(event)).detach();
  }
}

void Bot::message_update_event(const dpp::message_update_t& event) {
  const auto& channel_id = event.msg.channel_id.str();
  const auto& msg_id = chat_map[channel_id].first;
  if (msg_id == event.msg.id)
    update_chat_map(event.raw_event, channel_id, msg_id);
}

void Bot::member_add_event(const dpp::guild_member_add_t& event) {
  if (!event.added.get_user()->is_bot() && give_autorole) 
    bot.guild_member_add_role(guild_id, event.added.get_user()->id, autorole_id);
}

void Bot::member_remove_event(const dpp::guild_member_remove_t& event) {
  disid_osuid_map.erase(event.removed.id.str());
}

void Bot::slashcommand_event(const dpp::slashcommand_t& event) {
  // lol
  if (event.command.get_command_name() == "гандон") {
    float_t    f     = rand.get_real(0.0f, 100.0f);
    auto embed = dpp::embed()
      .set_color(dpp::colors::cream)
      .set_title("Тест на гандона")
      .set_description(fmt::format("**Вы гандон на {:.2f}%**", f))
      .set_timestamp(time(0));
    dpp::message msg(event.command.channel_id, embed);
    event.reply(msg);
  }

  // avatar
  if (event.command.get_command_name() == "avatar") {
    std::string username = std::get<std::string>(event.get_parameter("username"));
    std::string userid = request.get_userid_v1(username);
    auto embed = dpp::embed()
      .set_color(dpp::colors::cream)
      .set_author(username, "https://osu.ppy.sh/users/" + userid, "")
      .set_image("https://a.ppy.sh/" + userid)
      .set_timestamp(time(0));
    dpp::message msg(event.command.channel_id, embed);
    event.reply(msg);
  }

  // update_token
  if (event.command.get_command_name() == "update_token") {
    auto invoker_id = event.command.usr.id.str();
    if (invoker_id != "403958611367297024" && invoker_id != "249958340690575360") {
      event.reply(dpp::message("<:FRICK:1241513672480653475>"));
      return;
    }
    if (request.update_token())
      event.reply(dpp::message("Token update - success"));
    else
      event.reply(dpp::message("Token update - fail"));
  }

  // set
  if (event.command.get_command_name() == "set") {
    std::string u_from_com = std::get<std::string>(event.get_parameter("username"));
    std::string req        = request.get_user(u_from_com);
    if (req.empty()) {
      event.reply(dpp::message(fmt::format("Can't find {} on Bancho.", u_from_com)));
      return;
    }
    json        j          = json::parse(req);
    std::string key        = event.command.usr.id.str();
    std::string u_from_req = "";
    try {
      u_from_req = j.at("username").get<std::string>();
    } catch (json::exception e) {}
    disid_osuid_map[key] = fmt::to_string(j.value("id", 0));
    utils::map_to_file(disid_osuid_map, "users.json");
    event.reply(dpp::message(fmt::format("Your osu username: {}", u_from_req)));
  }

  // score
  if (event.command.get_command_name() == "score") {
    const auto& user = disid_osuid_map[event.command.usr.id.str()];
    if (user.empty()) {
      event.reply(dpp::message("Please /set your osu username before using this command."));
      return;
    }

    const std::string& beatmap_id = chat_map[event.command.channel_id.str()].second;
    if (beatmap_id.empty()) {
      event.reply(dpp::message("Can't find the map. Please send the map link and use this command again."));
      return;
    }

    std::string response_beatmap = request.get_beatmap(beatmap_id);
    std::string response_score   = request.get_user_beatmap_score(beatmap_id, user);
    if (response_score.empty()) {
      event.reply(dpp::message("Can't find score on this map."));
      return;
    }

    Score      score(response_score);
    Beatmap    beatmap(response_beatmap);
    auto embed = dpp::embed()
      .set_color(dpp::colors::cream)
      .set_title(beatmap.to_string())
      .set_url(beatmap.get_beatmap_url())
      .set_thumbnail(beatmap.get_image_url())
      .add_field("Your best score on map", score.to_string(beatmap.get_max_combo()));
    event.reply(embed);
  }

  // autorole_switch
  if (event.command.get_command_name() == "autorole_switch") {
    auto invoker_id = event.command.usr.id.str();
    if (invoker_id != "403958611367297024" && invoker_id != "249958340690575360") {
      event.reply(dpp::message("<:FRICK:1241513672480653475>"));
      return;
    }
    if (give_autorole) {
      give_autorole = false;
      event.reply("Giving autorole switched to off");
    }
    else {
      give_autorole = true;
      event.reply("Giving autorole switched to on");
    }
  }

  // weather
  if (event.command.get_command_name() == "weather") {
    const std::string city = std::get<std::string>(event.get_parameter("city"));
    std::string w = request.get_weather(city);
    json j = json::parse(w);

    std::string c  = j.value("name", "Unknown");
    std::string desc  = j["weather"].at(0).value("description", "");
    double temp       = j["main"].value("temp", 0.0);
    double feels      = j["main"].value("feels_like", 0.0);
    int humidity      = j["main"].value("humidity", 0);
    double wind       = j["wind"].value("speed", 0.0);

    std::time_t timestamp = j.value("dt", 0);
    timestamp += j.value("timezone", 0);
    std::tm tm = *std::gmtime(&timestamp);
    std::ostringstream time;
    time << std::put_time(&tm, "%d.%m.%Y %H:%M");

    auto embed = dpp::embed()
        .set_color(dpp::colors::cream)
        .set_title(c + " - " + desc)
        //.set_description(desc)
        .add_field("Температура", fmt::format("{:.1f}°C, ощущается как {:.1f}°C", temp, feels ))
        .add_field("Влажность", fmt::format("{}%", humidity), true)
        .add_field("Ветер", fmt::format("{:.1f}м/с", wind), true)
        .set_footer(dpp::embed_footer().set_text(time.str()));

    event.reply(embed);
  }
}

void Bot::ready_event(const dpp::ready_t& event, bool delete_commands) {
  if (dpp::run_once<struct register_bot_commands>()) {
    if (delete_commands) 
      bot.global_bulk_command_delete();

    bot.global_command_create(dpp::slashcommand("гандон", "Проверка", bot.me.id));
    bot.global_command_create(dpp::slashcommand("pages", "test", bot.me.id));
    bot.global_command_create(dpp::slashcommand("avatar", "Display osu! profile avatar", bot.me.id)
      .add_option(dpp::command_option(dpp::co_string, "username", "osu! profile username", true))
    );
    bot.global_command_create(dpp::slashcommand("update_token", "If peppy doesn't respond", bot.me.id));
    bot.global_command_create(dpp::slashcommand("set", "Set osu username", bot.me.id)
      .add_option(dpp::command_option(dpp::co_string, "username", "Your osu! profile username", true))
    );
    bot.global_command_create(dpp::slashcommand("autorole_switch", "Manage autorole issuance", bot.me.id));
    /*bot.global_command_create(
        dpp::slashcommand("score", "Displays your score", bot.me.id));*/
    bot.global_command_create(dpp::slashcommand("weather", "Shows current weather", bot.me.id)
      .add_option(dpp::command_option(dpp::co_string, "city", "Location", true))
    );
  }
  guild_id = utils::read_field("GUILD_ID", "config.json");
  autorole_id =  utils::read_field("AUTOROLE_ID", "config.json"); 
  utils::file_to_map(disid_osuid_map, "users.json");
}

Bot::Bot(const std::string& token, bool delete_commands) : bot(token), arena(tbb::task_arena(16)) {
  bot.intents = dpp::i_default_intents | dpp::i_message_content;

  bot.on_log(dpp::utility::cout_logger());
  bot.on_button_click([this](const dpp::button_click_t& event) {
    button_click_event(event);
  });
  bot.on_message_create([this](const dpp::message_create_t& event) {
    message_create_event(event);
  });
  bot.on_message_update([this](const dpp::message_update_t& event){
    message_update_event(event);
  });
  bot.on_guild_member_add([this](const dpp::guild_member_add_t& event) {
    member_add_event(event);
  });
  bot.on_guild_member_remove([this](const dpp::guild_member_remove_t& event) {
    member_remove_event(event);
  });
  bot.on_slashcommand([this](const dpp::slashcommand_t& event) {
    std::jthread(&Bot::slashcommand_event, this, std::move(event)).detach();
  });
  bot.on_ready([this, delete_commands](const dpp::ready_t& event) { 
    ready_event(event, delete_commands); 
  });
  

  bot.start(dpp::st_wait);
}
