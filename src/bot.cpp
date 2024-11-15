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
#include <chrono>
#include <cstdlib>
#include <regex>
#include <thread>
#include <type_traits>

// test
std::string page_content(const std::vector<std::string> &v, size_t page,
                         size_t items_per_page = 5) {
  std::stringstream ss;
  size_t start = page * items_per_page;
  size_t end = std::min((size_t)v.size(), start + items_per_page);
  for (size_t i = start; i < end; ++i) {
    ss << v[i] << "\n";
  }
  return ss.str();
}

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

void Temp_data::delayed_del(const dpp::snowflake &msg_id) {
  std::jthread([this, msg_id]() {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    map.erase(msg_id);
  }).detach();
}

void Temp_data::add_temp(const dpp::snowflake &msg_id, const Beatmap &beatmap,
                         std::vector<Score> &scores) {
  Data data(beatmap, scores);
  map[msg_id] = data;
  delayed_del(msg_id);
}

void Bot::write_cbid_map(const std::string &msg,
                         const std::string &channel_id) {
  std::regex url_reg(
      R"(https:\/\/osu\.ppy\.sh\/(beatmapsets\/\d+\/?#(?:osu|taiko|fruits|mania)\/|beatmaps\/|b\/)(\d+))");
  std::smatch m;
  if (std::regex_search(msg, m, url_reg) && m.size() > 2)
    channel_beatmapid_map[channel_id] = m.str(2);
}

void Bot::to_json_un_map() {
  std::lock_guard<std::mutex> lock(mutex);
  json j(disid_userid_map);
  std::ofstream file("map.json");
  if (file.is_open()) {
    file << j.dump(4);
    file.close();
  }
}

auto Bot::from_json_un_map(const dpp::snowflake &guild_id) {
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

void Bot::handle_button_click(const dpp::button_click_t &event) {
  dpp::snowflake user_id = event.command.usr.id;
  size_t current_page = user_page_map[user_id];
  size_t total_pages = temp.map[event.command.message_id].scores.size();
  if (event.custom_id == "next_page" && current_page < total_pages - 1) {
    current_page++;
  } else if (event.custom_id == "prev_page" && current_page > 0) {
    current_page--;
  }
  auto back_button = dpp::component()
                         .set_type(dpp::cot_button)
                         .set_emoji(dpp::unicode_emoji::arrow_left)
                         .set_style(dpp::cos_secondary)
                         .set_id("prev_page")
                         .set_disabled(current_page == 0);
  auto next_button = dpp::component()
                         .set_type(dpp::cot_button)
                         .set_emoji(dpp::unicode_emoji::arrow_right)
                         .set_style(dpp::cos_secondary)
                         .set_id("next_page")
                         .set_disabled(current_page == total_pages - 1);
  dpp::message m = dpp::message(event.command.channel_id,
                                "**Свойства молочка пачули**\n\n" +
                                    page_content(content, current_page, 1));
  m.add_component(
      dpp::component().add_component(back_button).add_component(next_button));
  event.reply(dpp::ir_update_message, m);
}

void Bot::create_lb_message(const dpp::message_create_t &event) {
  std::string beatmap_id;
  auto beatmap_it = channel_beatmapid_map.find(event.msg.channel_id.str());
  if (beatmap_it == channel_beatmapid_map.end()) {
    event.reply(dpp::message("Can't find the map. Please send the map link and "
                             "use the command again."));
    return;
  }
  beatmap_id = beatmap_it->second;
  std::string response_beatmap = request.get_beatmap(beatmap_id);
  if (response_beatmap.empty()) {
    spdlog::warn("Unable to send request");
    event.reply(dpp::message("Peppy didn't respond"));
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
                           spdlog::info("for_each");
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
  write_cbid_map(event.raw_event, event.msg.channel_id.str());
  if (event.msg.content.find("!lb") == 0) {
    std::jthread(&Bot::create_lb_message, this, std::move(event)).detach();
  }
}

void Bot::handle_slashcommand(const dpp::slashcommand_t &event) {
  if (event.command.get_command_name() == "pages") {
    size_t total_pages = content.size();
    dpp::snowflake user_id = event.command.usr.id;
    user_page_map[user_id] = 0;
    auto back_button = dpp::component()
                           .set_type(dpp::cot_button)
                           .set_emoji(dpp::unicode_emoji::arrow_left)
                           .set_style(dpp::cos_secondary)
                           .set_id("prev_page")
                           .set_disabled(true);
    auto next_button = dpp::component()
                           .set_type(dpp::cot_button)
                           .set_label("")
                           .set_emoji(dpp::unicode_emoji::arrow_right)
                           .set_style(dpp::cos_secondary)
                           .set_id("next_page")
                           .set_disabled(total_pages <= 1);
    dpp::message m(event.command.channel_id, "**Свойства молочка пачули**\n\n" +
                                                 page_content(content, 0, 1));
    m.add_component(
        dpp::component().add_component(back_button).add_component(next_button));
    event.reply(m);
  }
  if (event.command.get_command_name() == "patchouli") {

    dpp::embed embed =
        dpp::embed()
            .set_color(dpp::colors::cream)
            .set_title("Основные свойства молочка пачули")
            .add_field("Увлажнение и питание",
                       "Молочко пачули отлично увлажняет и питает кожу, "
                       "благодаря чему она становится более мягкой, эластичной "
                       "и здоровой на вид.")
            .add_field("Антисептические и антибактериальные свойства",
                       "Эфирное масло пачули обладает природными "
                       "антисептическими и антибактериальными свойствами, что "
                       "помогает в борьбе с кожными инфекциями и воспалениями.")
            .add_field("Антивозрастное действие",
                       "Благодаря своим регенерирующим и антиоксидантным "
                       "свойствам, молочко пачули способствует замедлению "
                       "процессов старения кожи, способствует разглаживанию "
                       "морщин и повышению упругости кожи.")
            .add_field("Восстановление и регенерация",
                       "Молочко пачули помогает в восстановлении поврежденной "
                       "кожи, способствует заживлению мелких порезов и ссадин.")
            .add_field("Успокаивающее воздействие",
                       "Аромат пачули обладает успокаивающими и расслабляющими "
                       "свойствами, что может снизить уровень стресса и "
                       "улучшить общее самочувствие.")
            .set_timestamp(time(0));

    dpp::message msg(event.command.channel_id, embed);

    event.reply(msg);
  }
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
    if (event.command.usr.id.str() != "403958611367297024") {
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
    to_json_un_map();
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
    auto beatmap_it =
        channel_beatmapid_map.find(event.command.channel_id.str());
    if (beatmap_it == channel_beatmapid_map.end()) {
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

void Bot::ready_event(const dpp::ready_t &event) {
  if (dpp::run_once<struct register_bot_commands>()) {
    // bot.global_bulk_command_delete();
    bot.global_command_create(
        dpp::slashcommand("patchouli", "Information", bot.me.id));
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
    bot.global_command_create(
        dpp::slashcommand("score", "Displays your score", bot.me.id));
    // bot.message_create(dpp::message(1217055390261317662, "Hello, world!"));
  }
  disid_userid_map = from_json_un_map(1030424871173361704);
}

void Bot::start() { bot.start(dpp::st_wait); }

Bot::Bot(const std::string &token) : bot(token) {
  bot.intents = dpp::i_default_intents | dpp::i_message_content;
  bot.on_log(dpp::utility::cout_logger());
  bot.on_button_click(
      [this](const dpp::button_click_t &event) { handle_button_click(event); });
  bot.on_message_create(
      [this](const dpp::message_create_t &event) { handle_message(event); });
  bot.on_slashcommand([this](const dpp::slashcommand_t &event) {
    std::jthread(&Bot::handle_slashcommand, this, std::move(event)).detach();
  });
  bot.on_ready([this](const dpp::ready_t &event) { ready_event(event); });
}
