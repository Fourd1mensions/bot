#pragma once

#include <nlohmann/json.hpp>

using json = nlohmann::json;

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

  inline void set_username(const std::string& name) { username = name; }
  
  Score() {}
  Score(const json& json) { from_json(json); }
  Score(const std::string& json_str);
};

class Beatmap {
private:
  uint32_t    beatmap_id;
  uint32_t    beatmapset_id;
  float_t     difficulty_rating;
  std::string artist;
  std::string title;
  std::string version; // diff name
  std::string beatmap_url;
  std::string image_url;
  uint32_t    max_combo;

  void        from_json(const json& j);

public:
  std::string to_string() const;
  std::string get_beatmap_url() const;
  std::string get_image_url() const;
  uint32_t    get_max_combo() const;

  Beatmap() = default;

  Beatmap(const json& json) {
    from_json(json);
  }

  Beatmap(const std::string& json_str);
};
