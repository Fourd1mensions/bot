#pragma once

#include <vector>
#include <string>
#include <chrono>
#include <unordered_map>
#include <tuple>
#include <dpp/dpp.h>
#include "state/ipaginable.h"

// Forward declarations
class Score;
class Beatmap;

/**
 * Sort methods for leaderboard (duplicated here to avoid circular deps)
 */
enum class LbSortMethod {
  PP,      // Sort by PP (default)
  Score,   // Sort by score
  Acc,     // Sort by accuracy
  Combo,   // Sort by max combo
  Date     // Sort by date (most recent first)
};

inline std::string sort_method_to_string(LbSortMethod method) {
  switch (method) {
    case LbSortMethod::Score: return "score";
    case LbSortMethod::Acc: return "accuracy";
    case LbSortMethod::Combo: return "combo";
    case LbSortMethod::Date: return "date";
    case LbSortMethod::PP:
    default: return "pp";
  }
}

struct LeaderboardState : public IPaginable {
  std::vector<Score> scores;
  Beatmap beatmap;
  std::string mode;
  std::string mods_filter;
  LbSortMethod sort_method;
  size_t current_page;
  size_t total_pages;
  std::chrono::steady_clock::time_point created_at;
  dpp::snowflake caller_discord_id;  // Discord ID of user who called !lb

  LeaderboardState() : mode("osu"), sort_method(LbSortMethod::PP), current_page(0), total_pages(0), created_at(std::chrono::steady_clock::now()), caller_discord_id(0) {}
  LeaderboardState(std::vector<Score> s, Beatmap b, size_t page = 0, std::string m = "osu", std::string mods = "", LbSortMethod sort = LbSortMethod::PP, dpp::snowflake caller_id = 0)
    : scores(std::move(s)), beatmap(std::move(b)), mode(std::move(m)), mods_filter(std::move(mods)), sort_method(sort), current_page(page),
      created_at(std::chrono::steady_clock::now()), caller_discord_id(caller_id) {
    constexpr size_t SCORES_PER_PAGE = 5;
    total_pages = (scores.size() + SCORES_PER_PAGE - 1) / SCORES_PER_PAGE;
    if (total_pages == 0) total_pages = 1;
  }

  // IPaginable implementation
  size_t get_current_position() const override { return current_page; }
  void set_current_position(size_t pos) override { current_page = pos; }
  size_t get_total_items() const override { return total_pages; }
};

struct RecentScoreState : public IPaginable {
  std::vector<Score> scores;
  size_t current_index;
  std::string mode;
  bool include_fails;
  bool use_best_scores;
  int64_t osu_user_id;  // For refresh functionality
  size_t refresh_count; // Track number of refreshes
  std::chrono::steady_clock::time_point created_at;
  dpp::snowflake caller_discord_id;
  std::unordered_map<uint32_t, std::tuple<float, float, float, float, int>> beatmap_difficulty_cache; // beatmap_id -> (AR, OD, CS, HP, total_objects)
  std::unordered_map<size_t, std::string> page_content_cache; // page_index -> cached message content (for fast navigation)

  RecentScoreState() : current_index(0), mode("osu"), include_fails(false), use_best_scores(false), osu_user_id(0), refresh_count(0),
                       created_at(std::chrono::steady_clock::now()), caller_discord_id(0) {}
  RecentScoreState(std::vector<Score> s, size_t index = 0, std::string m = "osu", bool fails = false, bool best = false, int64_t user_id = 0, dpp::snowflake caller_id = 0)
    : scores(std::move(s)), current_index(index), mode(std::move(m)), include_fails(fails), use_best_scores(best), osu_user_id(user_id), refresh_count(0),
      created_at(std::chrono::steady_clock::now()), caller_discord_id(caller_id) {}

  // IPaginable implementation
  size_t get_current_position() const override { return current_index; }
  void set_current_position(size_t pos) override { current_index = pos; }
  size_t get_total_items() const override { return scores.size(); }
};

struct CompareState : public IPaginable {
  static constexpr size_t SCORES_PER_PAGE = 5;

  std::vector<Score> scores;
  Beatmap beatmap;
  std::string username;
  std::string mods_filter;
  size_t current_page;
  size_t total_pages;
  std::chrono::steady_clock::time_point created_at;
  dpp::snowflake caller_discord_id;

  CompareState() : current_page(0), total_pages(0), created_at(std::chrono::steady_clock::now()), caller_discord_id(0) {}
  CompareState(std::vector<Score> s, Beatmap b, std::string user, std::string mods = "", dpp::snowflake caller_id = 0)
    : scores(std::move(s)), beatmap(std::move(b)), username(std::move(user)), mods_filter(std::move(mods)),
      current_page(0), created_at(std::chrono::steady_clock::now()), caller_discord_id(caller_id) {
    total_pages = (scores.size() + SCORES_PER_PAGE - 1) / SCORES_PER_PAGE;
    if (total_pages == 0) total_pages = 1;
  }

  // IPaginable implementation
  size_t get_current_position() const override { return current_page; }
  void set_current_position(size_t pos) override { current_page = pos; }
  size_t get_total_items() const override { return total_pages; }
};

/**
 * User mapping entry for !users command pagination
 */
struct UserMapping {
  dpp::snowflake discord_id;
  int64_t osu_user_id;
  std::string osu_username;

  UserMapping() : discord_id(0), osu_user_id(0) {}
  UserMapping(dpp::snowflake d_id, int64_t o_id, std::string name = "")
    : discord_id(d_id), osu_user_id(o_id), osu_username(std::move(name)) {}
};

struct UsersState : public IPaginable {
  static constexpr size_t USERS_PER_PAGE = 10;

  std::vector<UserMapping> users;
  size_t current_page;
  size_t total_pages;
  std::chrono::steady_clock::time_point created_at;
  dpp::snowflake caller_discord_id;

  UsersState() : current_page(0), total_pages(0), created_at(std::chrono::steady_clock::now()), caller_discord_id(0) {}
  UsersState(std::vector<UserMapping> u, dpp::snowflake caller_id = 0)
    : users(std::move(u)), current_page(0), created_at(std::chrono::steady_clock::now()), caller_discord_id(caller_id) {
    total_pages = (users.size() + USERS_PER_PAGE - 1) / USERS_PER_PAGE;
    if (total_pages == 0) total_pages = 1;
  }

  // IPaginable implementation
  size_t get_current_position() const override { return current_page; }
  void set_current_position(size_t pos) override { current_page = pos; }
  size_t get_total_items() const override { return total_pages; }
};
