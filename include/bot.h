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

class Bot {
private:
  bool            give_autorole = true;
  dpp::snowflake  guild_id,
                  autorole_id;
  dpp::cluster    bot;
  Random          rand;
  Request         request;
  std::mutex      mutex;
  tbb::task_arena arena;

  // Contains channel_id : {message_id : beatmap_id}
  std::unordered_map<std::string, std::pair<std::string, std::string>> chat_map;

  // Contains discord_member_id: osu_user_id. Loads from map.json on bot start, filled via slashcommand /set
  std::unordered_map<std::string, std::string> disid_osuid_map;

  void update_chat_map(const std::string& msg, const std::string& channel_id, const std::string& msg_id);
  void write_users_json();
  auto read_users_json(const dpp::snowflake& guild_id)
      -> std::unordered_map<std::string, std::string>;
  void create_lb_message(const dpp::message_create_t& event);

  // Handle events

  void handle_button_click(const dpp::button_click_t& event);
  void handle_message_create(const dpp::message_create_t& event);
  void handle_message_update(const dpp::message_update_t& event);
  void handle_member_add(const dpp::guild_member_add_t& event);
  void handle_member_remove(const dpp::guild_member_remove_t& event);
  void handle_slashcommand(const dpp::slashcommand_t& event);
  void ready_event(const dpp::ready_t& event, bool);

public:
  Bot(const std::string& token, bool delete_commands);
};
