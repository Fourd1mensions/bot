#pragma once

#include <vector>
#include <string>
#include <chrono>
#include <unordered_map>
#include <tuple>
#include <dpp/dpp.h>

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

struct LeaderboardState {
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
};

struct RecentScoreState {
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
};
