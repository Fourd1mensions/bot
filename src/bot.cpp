#include <bot.h>

#include <colors.h>
#include <dpp/cluster.h>
#include <exception.h>
#include <fmt/base.h>
#include <fmt/format.h>
#include <message.h>
#include <requests.h>
#include <snowflake.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <thread>
#include <type_traits>

template <typename T> T Random::get_real(T min, T max) {
  static_assert(std::is_floating_point<T>::value,
                "Type must be a floating-point type");
  std::uniform_real_distribution<> distr(min, max);
  return distr(_gen);
}

template <typename T> T Random::get_int(T min, T max) {
  static_assert(std::is_integral<T>::value, "Type must be an integral type");
  std::uniform_int_distribution<> distr(min, max);
  return distr(_gen);
}

bool Random::get_bool() {
  std::bernoulli_distribution distr(0.5);
  return distr(_gen);
}

void Bot::update_chat_map(const std::string &msg,
                          const std::string &channel_id) {
  std::regex url_reg(
      R"(https:\/\/osu\.ppy\.sh\/(beatmapsets\/\d+\/?#(?:osu|taiko|fruits|mania)\/|beatmaps\/|b\/)(\d+))");
  std::smatch m;
  if (std::regex_search(msg, m, url_reg) && m.size() > 2)
    chat_map[channel_id] = m.str(2);
}

void Bot::write_map_json() {
  std::lock_guard<std::mutex> lock(mutex);
  json j(disid_userid_map);
  std::ofstream file("map.json");
  if (file.is_open()) {
    file << j.dump(4);
    file.close();
  }
}

auto Bot::read_map_json(const dpp::snowflake &guild_id) {
  std::unordered_map<std::string, std::string> result;
  std::ifstream file("map.json");
  if (!file.is_open()) {
    spdlog::error("Failed to open map.json, cannot load users");
    return result;
  }
  json j = json::parse(file, nullptr, false);
  file.close();
  for (const auto &[id, username] : j.items()) {
    try {
      if (bot.guild_get_member_sync(guild_id, id).user_id == id.c_str()) {
        result[id] = username;
      }
    } catch (dpp::exception e) {
      spdlog::error("Failed to parse map.json");
    }
  }
  return result;
}

void Bot::handle_button_click(const dpp::button_click_t &event) {}

void Bot::create_lb_message(const dpp::message_create_t &event) {
  std::string beatmap_id;
  auto beatmap_it = chat_map.find(event.msg.channel_id.str());
  if (beatmap_it == chat_map.end()) {
    event.reply(dpp::message("Can't find the map. Please send the map link and "
                             "use the command again."));
    return;
  }
  beatmap_id = beatmap_it->second;
  std::string response_beatmap = request.get_beatmap(beatmap_id);
  if (response_beatmap.empty()) {
    spdlog::error("Unable to send request");
    event.reply(dpp::message("Peppy didn't respond"));
    return;
  }
  Beatmap beatmap(response_beatmap);
  std::vector<Score> scores;
  /*for (const auto& [dis_id, user_id]: disid_userid_map) {
      //spdlog::info("for");
      std::string score_j = request.get_user_score(beatmap_id, user_id);
      if (score_j.empty()) continue;
      scores.push_back(Score(score_j));
  }*/
  std::mutex m;
  // force tbb parallelization ???
  tbb::parallel_for_each(std::begin(disid_userid_map),
                         std::end(disid_userid_map), [&](const auto &pair) {
                           // spdlog::info("for_each");
                           const auto &[dis_id, user_id] = pair;
                           std::string score_j =
                               request.get_user_score(beatmap_id, user_id);
                           if (!score_j.empty()) {
                             Score score(score_j);
                             std::lock_guard<std::mutex> lock(m);
                             scores.push_back(std::move(score));
                           }
                         });
  if (scores.empty()) {
    event.reply(
        dpp::message("Can't find any scores on " + beatmap.to_string()));
    return;
  }
  if (scores.size() > 1) {
    std::ranges::sort(scores, [](const Score a, const Score b) {
      return std::make_tuple(a.get_pp(), a.get_total_score()) >
             std::make_tuple(b.get_pp(), b.get_total_score());
    });
  }
  auto msg_id = event.msg.id;
  auto embed = dpp::embed()
                   .set_color(dpp::colors::viola_purple)
                   .set_title(beatmap.to_string())
                   .set_url(beatmap.get_beatmap_url())
                   .set_thumbnail(beatmap.get_image_url());

  for (size_t i = 0; i < scores.size(); i++) {
    dpp::embed_field field;
    field.name = fmt::format("{}) {}", i + 1, scores[i].get_header());
    field.value = scores[i].get_body(beatmap.get_max_combo());
    field.is_inline = false;
    embed.fields.push_back(field);
    if (embed.fields.size() == 5)
      break;
  }
  event.reply(embed);
}

void Bot::handle_message(const dpp::message_create_t &event) {
  fmt::print("{}: {}\n", event.msg.author.username, event.msg.content);
  update_chat_map(event.raw_event, event.msg.channel_id.str());
  if (event.msg.content.find("!lb") == 0) {
    std::jthread(&Bot::create_lb_message, this, std::move(event)).detach();
  }
}

void Bot::handle_slashcommand(const dpp::slashcommand_t &event) {
  if (event.command.get_command_name() == "гандон") {
    float_t f = rand.get_real(0.0f, 100.0f);
    dpp::embed embed =
        dpp::embed()
            .set_color(dpp::colors::cream)
            .set_title("Тест на гандона")
            .set_description(fmt::format("**Вы гандон на {:.2f}%**", f))
            .set_timestamp(time(0));
    dpp::message msg(event.command.channel_id, embed);
    event.reply(msg);
  }
  if (event.command.get_command_name() == "avatar") {
    std::string username =
        std::get<std::string>(event.get_parameter("username"));
    std::string userid = request.get_userid_v1(username);
    dpp::embed embed =
        dpp::embed()
            .set_color(dpp::colors::cream)
            .set_author(username, "https://osu.ppy.sh/users/" + userid, "")
            .set_image("https://a.ppy.sh/" + userid)
            .set_timestamp(time(0));
    dpp::message msg(event.command.channel_id, embed);
    event.reply(msg);
  }
  if (event.command.get_command_name() == "update_token") {
    auto invoker_id = event.command.usr.id.str();
    if (invoker_id != "403958611367297024" &&
        invoker_id != "249958340690575360") {
      event.reply(dpp::message("<:FRICK:1241513672480653475>"));
      return;
    }
    if (request.set_tokens("refresh_token"))
      event.reply(dpp::message("Token update - success"));
    else
      event.reply(dpp::message("Token update - fail"));
  }
  if (event.command.get_command_name() == "set") {
    std::string u_from_com =
        std::get<std::string>(event.get_parameter("username"));
    std::string req = request.get_user(u_from_com);
    if (req.empty()) {
      event.reply(
          dpp::message(fmt::format("Can't find {} on Bancho.", u_from_com)));
      return;
    }
    json j = json::parse(req);
    std::string key = event.command.usr.id.str();
    std::string u_from_req = "";
    try {
      u_from_req = j.at("username").get<std::string>();
    } catch (json::exception e) {
    }
    disid_userid_map[key] = fmt::to_string(j.value("id", 0));
    write_map_json();
    event.reply(dpp::message(fmt::format("Your osu username: {}", u_from_req)));
  }
  if (event.command.get_command_name() == "score") {
    std::string user;
    auto user_it = disid_userid_map.find(event.command.usr.id.str());
    if (user_it == disid_userid_map.end()) {
      event.reply(dpp::message(
          "Please /set your osu username before using this command."));
      return;
    }
    user = user_it->second;
    std::string beatmap_id;
    auto beatmap_it = chat_map.find(event.command.channel_id.str());
    if (beatmap_it == chat_map.end()) {
      event.reply(dpp::message("Can't find the map. Please send the map link "
                               "and use the command again."));
      return;
    }
    beatmap_id = beatmap_it->second;
    std::string response_beatmap = request.get_beatmap(beatmap_id);
    std::string response_score = request.get_user_score(beatmap_id, user);
    if (response_score.empty()) {
      event.reply(dpp::message("Can't find score on this map."));
      return;
    }
    Score score(response_score);
    Beatmap beatmap(response_beatmap);
    dpp::embed embed = dpp::embed()
                           .set_color(dpp::colors::cream)
                           .set_title(beatmap.to_string())
                           .set_url(beatmap.get_beatmap_url())
                           .set_thumbnail(beatmap.get_image_url())
                           .add_field("Your best score on map",
                                      score.to_string(beatmap.get_max_combo()));
    event.reply(embed);
  }
}

void Bot::ready_event(const dpp::ready_t &event, bool delete_commands) {
  if (dpp::run_once<struct register_bot_commands>()) {
    if (delete_commands)
      bot.global_bulk_command_delete();

    bot.global_command_create(
        dpp::slashcommand("гандон", "Проверка", bot.me.id));
    bot.global_command_create(dpp::slashcommand("pages", "test", bot.me.id));
    bot.global_command_create(
        dpp::slashcommand("avatar", "Display osu! profile avatar", bot.me.id)
            .add_option(dpp::command_option(dpp::co_string, "username",
                                            "osu! profile username", true)));
    bot.global_command_create(dpp::slashcommand(
        "update_token", "If peppy doesn't respond", bot.me.id));
    bot.global_command_create(
        dpp::slashcommand("set", "Set osu username", bot.me.id)
            .add_option(dpp::command_option(dpp::co_string, "username",
                                            "Your osu! profile username",
                                            true)));
    /*bot.global_command_create(
        dpp::slashcommand("score", "Displays your score", bot.me.id));*/
  }
  disid_userid_map = read_map_json(1030424871173361704);
}

Bot::Bot(const std::string &token, bool delete_commands) : bot(token) {
  bot.intents = dpp::i_default_intents | dpp::i_message_content;
  bot.on_log(dpp::utility::cout_logger());
  bot.on_button_click(
      [this](const dpp::button_click_t &event) { handle_button_click(event); });
  bot.on_message_create(
      [this](const dpp::message_create_t &event) { handle_message(event); });
  bot.on_slashcommand([this](const dpp::slashcommand_t &event) {
    std::jthread(&Bot::handle_slashcommand, this, std::move(event)).detach();
  });
  bot.on_ready([this, delete_commands](const dpp::ready_t &event) {
    ready_event(event, delete_commands);
  });

  bot.start(dpp::st_wait);
}
