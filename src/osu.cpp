#include <osu.h>
#include <utils.h>

#include <spdlog/spdlog.h>

void Score::from_json(const json& j) {
  auto from_json_mods = [this](const json& j) {
    if (j.contains("mods") && j["mods"].is_array()) {
      const json& mods_j = j.at("mods");
      for (const auto& mod : mods_j) {
        mods += mod.get<std::string>();
      }
    }
    if (mods.empty())
      mods = "NM";
  };
  try {
    const json& score_j = j.contains("scores")  ? j.at("scores").at(0) : j;
    total_score         = score_j.value("score", 0);
    rank                = score_j.value("rank", "");
    accuracy            = score_j.value("accuracy", 0.0);
    created_at          = score_j.value("created_at", "");
    max_combo           = score_j.value("max_combo", 0);
    user_id             = score_j.value("user_id", 0);
    passed              = score_j.value("passed", true);

    pp        = !score_j["pp"].is_null() ? score_j.value("pp", 0.0) : 0.0;
    username  = score_j.contains("user") ? score_j.at("user").value("username", "") : "";

    // Parse beatmap_id from nested beatmap object
    if (score_j.contains("beatmap")) {
      beatmap_id = score_j.at("beatmap").value("id", 0);
    } else {
      beatmap_id = score_j.value("beatmap_id", 0);
    }

    if (score_j.contains("statistics") && score_j["statistics"].is_object()) {
      const auto& stat_j = score_j.at("statistics");
      count_miss = stat_j.contains("count_miss") && !stat_j["count_miss"].is_null() ? stat_j["count_miss"].get<uint32_t>() : 0;
      count_50   = stat_j.contains("count_50") && !stat_j["count_50"].is_null() ? stat_j["count_50"].get<uint32_t>() : 0;
      count_100  = stat_j.contains("count_100") && !stat_j["count_100"].is_null() ? stat_j["count_100"].get<uint32_t>() : 0;
      count_300  = stat_j.contains("count_300") && !stat_j["count_300"].is_null() ? stat_j["count_300"].get<uint32_t>() : 0;
    } else {
      count_miss = count_50 = count_100 = count_300 = 0;
    }

    mode = score_j.value("mode", "osu");

    from_json_mods(score_j);
    is_empty = false;
  } catch (const json::exception& e) { spdlog::error("Failed to parse score: {}", e.what()); }
}

std::string Score::to_string(const uint32_t beatmap_combo) const {
  std::string emoji_id;
  if (rank == "F")
    emoji_id = "<:RankingF:1278036373332295843>";
  else if (rank == "D")
    emoji_id = "<:RankingD:1278036354248474674>";
  else if (rank == "C")
    emoji_id = "<:RankingC:1278036342441250998>";
  else if (rank == "B")
    emoji_id = "<:RankingB:1278036331099852810>";
  else if (rank == "A")
    emoji_id = "<:RankingA:1278036315421671424>";
  else if (rank == "S")
    emoji_id = "<:RankingS:1278036387433680968>";
  else if (rank == "SH")
    emoji_id = "<:RankingSH:1278036405230108744>";
  else if (rank == "X")
    emoji_id = "<:RankingSS:1304449505873367130>";
  else if (rank == "XH")
    emoji_id = "<:RankingSSH:1304449533006057544>";

  std::string result =
      fmt::format("**{}  •  {:.2f}pp  •  {} ({:.2f}%)  +{}**\n{:L}  **•**  x{}/{}  **•**  "
                  "[{}/{}/{}/{}]",
                  username, pp, emoji_id, accuracy * 100, mods, total_score, max_combo,
                  beatmap_combo, count_300, count_100, count_50, count_miss);
  return result;
}

std::string Score::get_header() const {
  std::string result = fmt::format("{} `{:.0f}pp` +{}", username, pp, mods);
  return result;
}

std::string Score::get_body(const uint32_t beatmap_combo) const {

  std::string time = fmt::format("<t:{}:R>", utils::ISO8601_to_UNIX(created_at));

  std::string emoji_id;
  if (rank == "F")
    emoji_id = "<:RankingF:1278036373332295843>";
  else if (rank == "D")
    emoji_id = "<:RankingD:1278036354248474674>";
  else if (rank == "C")
    emoji_id = "<:RankingC:1278036342441250998>";
  else if (rank == "B")
    emoji_id = "<:RankingB:1278036331099852810>";
  else if (rank == "A")
    emoji_id = "<:RankingA:1278036315421671424>";
  else if (rank == "S")
    emoji_id = "<:RankingS:1278036387433680968>";
  else if (rank == "SH")
    emoji_id = "<:RankingSH:1278036405230108744>";
  else if (rank == "X")
    emoji_id = "<:RankingSS:1304449505873367130>";
  else if (rank == "XH")
    emoji_id = "<:RankingSSH:1304449533006057544>";

  return std::string(fmt::format("**▸**{}({:.2f}%) • {:L} • **x{}/{}** • "
                                 "[{}/{}/{}/{}]\n**▸** Score set {}",
                                 emoji_id, accuracy * 100, total_score, max_combo, beatmap_combo,
                                 count_300, count_100, count_50, count_miss, time));
}

std::string Score::get_created_at() const {
  return created_at;
}

void Beatmap::from_json(const json& j) {
  try {
    beatmap_id        = j.at("id").get<int>();
    beatmapset_id     = j.at("beatmapset_id").get<int>();
    difficulty_rating = j.at("difficulty_rating").get<double>();
    max_combo         = j.at("max_combo").get<int>();
    bpm               = j.value("bpm", 0.0);
    total_length      = j.value("total_length", 0);
    mode              = j.value("mode", "osu"); // Default to osu if not specified
    artist            = j.at("beatmapset").at("artist").get<std::string>();
    title             = j.at("beatmapset").at("title").get<std::string>();
    version           = j.at("version").get<std::string>();
    beatmap_url       = j.at("url").get<std::string>();
    // Use "cover" for full-size background image instead of "list"
    image_url         = j.at("beatmapset").at("covers").at("cover").get<std::string>();
  } catch (json::exception e) { spdlog::error("Failed to parse beatmap: {}", e.what()); }
}

std::string Beatmap::to_string() const {
  float_t rating = has_modded_rating ? modded_difficulty_rating : difficulty_rating;
  return fmt::format("{} - {} [{}] {:.3g}★", artist, title, version, rating);
}

std::string Beatmap::get_beatmap_url() const {
  return beatmap_url;
}

std::string Beatmap::get_image_url() const {
  return image_url;
}

std::string Beatmap::get_mode() const {
  return mode;
}

uint32_t Beatmap::get_max_combo() const {
  return max_combo;
}

uint32_t Beatmap::get_beatmap_id() const {
  return beatmap_id;
}

uint32_t Beatmap::get_beatmapset_id() const {
  return beatmapset_id;
}

void Beatmap::set_modded_attributes(const json& attributes_json) {
  try {
    if (attributes_json.contains("attributes")) {
      const json& attrs = attributes_json.at("attributes");
      if (attrs.contains("star_rating")) {
        modded_difficulty_rating = attrs.at("star_rating").get<double>();
        has_modded_rating = true;
      }
    }
  } catch (const json::exception& e) {
    spdlog::error("Failed to parse beatmap attributes: {}", e.what());
  }
}

float_t Beatmap::get_difficulty_rating(bool use_modded) const {
  if (use_modded && has_modded_rating) {
    return modded_difficulty_rating;
  }
  return difficulty_rating;
}

Score::Score(const std::string& json_str) {
  try {
    json json = json::parse(json_str);
    from_json(json);
  } catch (const json::exception& e) { spdlog::error("Failed to parse score: {}", e.what()); }
}

Beatmap::Beatmap(const std::string& json_str) : modded_difficulty_rating(0.0f), has_modded_rating(false) {
  try {
    json json = json::parse(json_str);
    from_json(json);
  } catch (const json::exception& e) { spdlog::error("Failed to parse beatmap: {}", e.what()); }
}
