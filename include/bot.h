#pragma once

#include <tbb/tbb.h>

#include <osu.h>
#include <requests.h>

#include <dpp/dpp.h>
#include <dpp/unicode_emoji.h>
#include <fmt/core.h>
#include <snowflake.h>
#include <spdlog/spdlog.h>

#include <random>
#include <unordered_map>

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
  dpp::cluster bot;
  Random       rand;
  Request      request;
  std::mutex   mutex;

  // Contains channel_id: last_beatmap_id
  std::unordered_map<std::string, std::string> chat_map;
  // Loads from map.json on bot start, filled via slashcommand /set
  std::unordered_map<std::string, std::string> disid_userid_map;

  void update_chat_map(const std::string& msg, const std::string& channel_id);
  void write_map_json();
  auto read_map_json(const dpp::snowflake& guild_id);
  void create_lb_message(const dpp::message_create_t& event);

  // Handle events

  void handle_button_click(const dpp::button_click_t& event);
  void handle_message(const dpp::message_create_t& event);
  void handle_slashcommand(const dpp::slashcommand_t& event);
  void ready_event(const dpp::ready_t& event, bool);

public:
  Bot(const std::string& token, bool delete_commands);
};
