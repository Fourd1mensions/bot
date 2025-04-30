#pragma once

#include <osu.h>


#include <dpp/message.h>

using str_map = std::unordered_map<std::string, std::string>;

class Builder {
private:
  str_map emoji;


public:
  dpp::embed lb_embed(const std::vector<Score>& scores, const Beatmap& map) const;


};
