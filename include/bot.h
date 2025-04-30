#pragma once

#include <tbb/tbb.h>
#include <random>
#include <unordered_map>

#include <osu.h>
#include <requests.h>

#include <dpp/dpp.h>

class Random {
private:
  std::random_device _rd;
  std::mt19937       _gen;

public:
  template <typename T>
  T get_real(T min, T max);
  template <typename T>
  T    get_int(T min, T max);
  bool get_bool();

  Random() : _rd(), _gen(_rd()) {}
};

using snowflake_string_map = std::unordered_map<dpp::snowflake, std::string>;

class Bot {
private:
  bool                  give_autorole = true;

  dpp::cluster          bot;
  dpp::snowflake        guild_id,
                        autorole_id;

  Random                rand;
  Request               request;
  
  std::mutex            mutex;
  tbb::task_arena       arena;

  // Contains channel_id : {message_id : beatmap_id}
  std::unordered_map<dpp::snowflake, std::pair<dpp::snowflake, std::string>> chat_map;

  // Contains discord_member_id: osu_user_id. Loads from map.json on bot start, filled via slashcommand /set
  snowflake_string_map  disid_osuid_map;
  void                  update_chat_map(const std::string& msg, const dpp::snowflake& channel_id, const dpp::snowflake& msg_id);

  // TODO: delete all this shit
 void                  create_lb_message(const dpp::message_create_t& event);
  // TODO: check guild members 

  // Handle events

  void button_click_event(const dpp::button_click_t& event);
  void message_create_event(const dpp::message_create_t& event);
  void message_update_event(const dpp::message_update_t& event);
  void member_add_event(const dpp::guild_member_add_t& event);
  void member_remove_event(const dpp::guild_member_remove_t& event);
  void slashcommand_event(const dpp::slashcommand_t& event);
  void ready_event(const dpp::ready_t& event, bool);

public:
  Bot(const std::string& token, bool delete_commands);
};
