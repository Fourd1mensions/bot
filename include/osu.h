#pragma once

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace cache { class MemcachedCache; }

/**
 * Beatmap ranked status from osu! API
 * https://osu.ppy.sh/docs/index.html#beatmapset-rank-status
 */
enum class BeatmapStatus : int8_t {
  Graveyard = -2,
  WIP = -1,
  Pending = 0,
  Ranked = 1,
  Approved = 2,
  Qualified = 3,
  Loved = 4
};

inline std::string beatmap_status_to_string(BeatmapStatus status) {
  switch (status) {
    case BeatmapStatus::Graveyard: return "graveyard";
    case BeatmapStatus::WIP: return "wip";
    case BeatmapStatus::Pending: return "pending";
    case BeatmapStatus::Ranked: return "ranked";
    case BeatmapStatus::Approved: return "approved";
    case BeatmapStatus::Qualified: return "qualified";
    case BeatmapStatus::Loved: return "loved";
    default: return "unknown";
  }
}

inline bool beatmap_has_pp(BeatmapStatus status) {
  return status == BeatmapStatus::Ranked || status == BeatmapStatus::Approved;
}

class Score {
private:
  float_t     accuracy;
  uint32_t    max_combo;
  std::string mods;
  uint32_t    count_miss, count_50, count_100, count_300;
  float_t     pp;
  uint32_t    total_score;
  std::string rank;
  std::string created_at;
  std::string username;
  size_t      user_id;
  uint32_t    beatmap_id;
  bool        passed;
  std::string mode;  // "osu", "taiko", "fruits", "mania"
  float_t     weight_percentage = 0.0f;  // Weight % in best scores (100, 95, 90.25...)
  float_t     weight_pp = 0.0f;          // Weighted PP value
  bool        set_on_lazer = false;      // Was score set on lazer client

  // Beatmap metadata (parsed from nested beatmap/beatmapset in best scores)
  uint32_t    beatmapset_id = 0;
  std::string beatmap_title;
  std::string beatmap_artist;
  std::string beatmap_version;
  uint32_t    beatmap_max_combo = 0;

  friend class cache::MemcachedCache;

public:
  bool        is_empty = true;

  void        from_json(const json& j);
  std::string to_string(const uint32_t beatmap_combo) const;

  std::string get_header() const;
  std::string get_body(const uint32_t beatmap_combo) const;
  std::string get_created_at() const;
  inline auto get_pp() const { return pp; }
  inline auto get_total_score() const { return total_score; }
  inline auto get_user_id() const { return user_id; }
  inline const std::string& get_mods() const { return mods; }
  inline auto get_accuracy() const { return accuracy; }
  inline auto get_max_combo() const { return max_combo; }
  inline const std::string& get_rank() const { return rank; }
  inline auto get_count_300() const { return count_300; }
  inline auto get_count_100() const { return count_100; }
  inline auto get_count_50() const { return count_50; }
  inline auto get_count_miss() const { return count_miss; }
  inline auto get_beatmap_id() const { return beatmap_id; }
  inline auto get_passed() const { return passed; }
  inline const std::string& get_username() const { return username; }
  inline const std::string& get_mode() const { return mode; }
  inline auto get_weight_percentage() const { return weight_percentage; }
  inline auto get_set_on_lazer() const { return set_on_lazer; }
  inline auto get_weight_pp() const { return weight_pp; }
  inline auto get_beatmapset_id() const { return beatmapset_id; }
  inline const std::string& get_beatmap_title() const { return beatmap_title; }
  inline const std::string& get_beatmap_artist() const { return beatmap_artist; }
  inline const std::string& get_beatmap_version() const { return beatmap_version; }
  inline auto get_beatmap_max_combo() const { return beatmap_max_combo; }

  inline void set_username(const std::string& name) { username = name; }
  inline void set_pp(float_t value) { pp = value; }
  
  Score() {}
  Score(const json& json) { from_json(json); }
  Score(const std::string& json_str);
};

class Beatmap {
private:
  uint32_t      beatmap_id;
  uint32_t      beatmapset_id;
  float_t       difficulty_rating;
  float_t       modded_difficulty_rating; // Star rating with mods applied
  bool          has_modded_rating;        // Whether modded rating has been set
  std::string   mode;          // Game mode: osu, taiko, fruits, mania
  std::string   artist;
  std::string   title;
  std::string   version; // diff name
  std::string   creator;
  std::string   beatmap_url;
  std::string   thumbnail_url;
  std::string   image_url;
  uint32_t      max_combo;
  float_t       bpm;
  uint32_t      total_length;  // in seconds
  float_t       ar = 0.0f;     // Approach Rate
  float_t       od = 0.0f;     // Overall Difficulty
  float_t       cs = 0.0f;     // Circle Size
  float_t       hp = 0.0f;     // HP Drain
  BeatmapStatus status;        // Ranked status (loved, ranked, etc.)

  void        from_json(const json& j);

  friend class cache::MemcachedCache;

public:
  std::string to_string() const;
  std::string get_beatmap_url() const;
  std::string get_thumbnail_url() const;
  std::string get_image_url() const;
  std::string get_mode() const;
  uint32_t    get_max_combo() const;
  uint32_t    get_beatmap_id() const;
  uint32_t    get_beatmapset_id() const;
  void        set_modded_attributes(const json& attributes_json);
  float_t     get_difficulty_rating(bool use_modded = false) const;
  inline auto get_bpm() const { return bpm; }
  inline auto get_total_length() const { return total_length; }
  inline auto get_ar() const { return ar; }
  inline auto get_od() const { return od; }
  inline auto get_cs() const { return cs; }
  inline auto get_hp() const { return hp; }
  inline BeatmapStatus get_status() const { return status; }
  inline const std::string& get_artist() const { return artist; }
  inline const std::string& get_title() const { return title; }
  inline const std::string& get_version() const { return version; }
  inline const std::string& get_creator() const { return creator; }
  inline bool has_pp() const { return beatmap_has_pp(status); }
  inline bool is_loved() const { return status == BeatmapStatus::Loved; }
  inline bool is_ranked() const { return status == BeatmapStatus::Ranked || status == BeatmapStatus::Approved; }

  Beatmap() : modded_difficulty_rating(0.0f), has_modded_rating(false), status(BeatmapStatus::Pending) {}

  Beatmap(const json& json) : modded_difficulty_rating(0.0f), has_modded_rating(false), status(BeatmapStatus::Pending) {
    from_json(json);
  }

  Beatmap(const std::string& json_str);
};
