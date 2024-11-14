#pragma once

#include "requests.h"
#include "osu.h"

#include "snowflake.h"
#include <dpp/dpp.h>
#include <dpp/unicode_emoji.h>
#include <spdlog/spdlog.h>
#include <fmt/core.h>

#include <string>
#include <random>
#include <unordered_map>
#include <vector>

class Random {
private:
    std::random_device _rd;
    std::mt19937 _gen;

public:
    template<typename T> T get_real(T min, T max);
    template<typename T> T get_int(T min, T max);
    bool get_bool();
    
    Random() : _rd(), _gen(_rd()) {}
};

class Temp_data {
private:
    struct Data {
        Beatmap beatmap;
        std::vector<Score> scores;
        uint32_t page = 0;
        Data() = default;
        Data(Beatmap b, std::vector<Score> s) : beatmap(b), scores(s) {}
    };
    void delayed_del(const dpp::snowflake& msg_id);

public:
    std::unordered_map<dpp::snowflake, Data> map;
    void add_temp(const dpp::snowflake& msg_id, const Beatmap& beatmap, std::vector<Score>& scores);
    Temp_data() = default;
};

class Bot { 
private:
    dpp::cluster bot;
    Random rand;
    Request request; 
    Temp_data temp;
    std::mutex mutex;

    std::unordered_map<std::string, std::string> channel_beatmapid_map;
    std::unordered_map<dpp::snowflake, int32_t> user_page_map;
    std::unordered_map<std::string, std::string> disid_userid_map;

    std::vector<std::string> content = {
    "Увлажнение и питание:\nМолочко пачули отлично увлажняет и питает кожу, благодаря чему она становится более мягкой, эластичной и здоровой на вид.", 
    "Антисептические и антибактериальные свойства:\nЭфирное масло пачули обладает природными антисептическими и антибактериальными свойствами, что помогает в борьбе с кожными инфекциями и воспалениями.", 
    "Антивозрастное действие:\nБлагодаря своим регенерирующим и антиоксидантным свойствам, молочко пачули способствует замедлению процессов старения кожи, способствует разглаживанию морщин и повышению упругости кожи.", 
    "Восстановление и регенерация:\nМолочко пачули помогает в восстановлении поврежденной кожи, способствует заживлению мелких порезов и ссадин.", 
    "Успокаивающее воздействие:\nАромат пачули обладает успокаивающими и расслабляющими свойствами, что может снизить уровень стресса и улучшить общее самочувствие."
    };

    void write_cbid_map(const std::string& msg, const std::string& channel_id);
    void to_json_un_map();
    auto from_json_un_map(const dpp::snowflake& guild_id); 

    // Handle events 

    void handle_button_click(const dpp::button_click_t& event);
    void handle_message(const dpp::message_create_t& event);
    void handle_slashcommand(const dpp::slashcommand_t& event);
    void ready_event(const dpp::ready_t& event);

    void create_lb_message(const dpp::message_create_t& event);

public:
    void start ();
    Bot(const std::string& token);
};