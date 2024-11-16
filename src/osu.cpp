#include "osu.h"

#include "fmt/format.h"
#include "spdlog/spdlog.h"

void Score::from_json(const json &j) {
  auto from_json_mods = [this](const json &j) {
    if (j.contains("mods") && j["mods"].is_array()) {
      const json &mods_j = j.at("mods");
      for (const auto &mod : mods_j) {
        mods += mod.get<std::string>();
      }
    }
    if (mods.empty())
      mods = "NM";
  };
  try {
    const json &score_j = j.at("score");
    total_score = score_j.value("score", 0);
    rank = score_j.value("rank", "");
    accuracy = score_j.value("accuracy", 0.0);
    created_at = score_j.value("created_at", "");
    max_combo = score_j.value("max_combo", 0);
    score_j.at("pp").is_null() ? pp = 0 : pp = score_j.value("pp", 0.0);
    count_miss = score_j.at("statistics").value("count_miss", 0);
    count_50 = score_j.at("statistics").value("count_50", 0);
    count_100 = score_j.at("statistics").value("count_100", 0);
    count_300 = score_j.at("statistics").value("count_300", 0);
    username = score_j.at("user").value("username", "");
    from_json_mods(score_j);
  } catch (const json::exception &e) {
    spdlog::error("Failed to parse score: {}", e.what());
  }
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

  std::string result = fmt::format(
      "**{}  •  {:.2f}pp  •  {} ({:.2f}%)  +{}**\n{:L}  **•**  x{}/{}  **•**  "
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
  auto ISO8601_to_UNIX = [](const std::string &datetime) {
    std::tm tm = {};
    std::istringstream ss(datetime);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return std::mktime(&tm) - timezone;
  };
  std::string time = fmt::format("<t:{}:R>", ISO8601_to_UNIX(created_at));

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
                                 emoji_id, accuracy * 100, total_score,
                                 max_combo, beatmap_combo, count_300, count_100,
                                 count_50, count_miss, time));
}

std::string Score::get_created_at() const { return created_at; }

void Beatmap::from_json(const json &j) {
  try {
    beatmap_id = j.at("id").get<int>();
    beatmapset_id = j.at("beatmapset_id").get<int>();
    difficulty_rating = j.at("difficulty_rating").get<double>();
    max_combo = j.at("max_combo").get<int>();
    artist = j.at("beatmapset").at("artist").get<std::string>();
    title = j.at("beatmapset").at("title").get<std::string>();
    version = j.at("version").get<std::string>();
    beatmap_url = j.at("url").get<std::string>();
    image_url = j.at("beatmapset").at("covers").at("list").get<std::string>();
  } catch (json::exception e) {
    spdlog::error("Failed to parse beatmap: {}", e.what());
  }
}

std::string Beatmap::to_string() const {
  return fmt::format("{} - {} [{}] {}★", artist, title, version,
                     difficulty_rating);
}

std::string Beatmap::get_beatmap_url() const { return beatmap_url; }

std::string Beatmap::get_image_url() const { return image_url; }

uint32_t Beatmap::get_max_combo() const { return max_combo; }

Score::Score(const std::string &json_str) {
  try {
    json json = json::parse(json_str);
    from_json(json);
  } catch (const json::exception &e) {
    spdlog::error("Failed to parse score: {}", e.what());
  }
}

Beatmap::Beatmap(const std::string &json_str) {
  try {
    json json = json::parse(json_str);
    from_json(json);
  } catch (const json::exception &e) {
    spdlog::error("Failed to parse beatmap: {}", e.what());
  }
}
