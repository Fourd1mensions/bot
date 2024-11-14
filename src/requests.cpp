#include "requests.h"

#include <fstream>
#include <string_view>

#include <cpr/cpr.h>
#include "cpr/api.h"
#include "cpr/cprtypes.h"
#include "cpr/payload.h"
#include "cpr/response.h"
#include "cpr/parameters.h"
#include "fmt/base.h"
#include "fmt/format.h"
#include "spdlog/spdlog.h"

std::string Request::from_file_tokens(const std::string_view file_name, const std::string_view key) {
    std::ifstream file(file_name.data());
    if (!file.is_open()) return "";
    json j;
    file >> j; 
    file.close();
    try {
        return j.value(key, "");
    } catch(json::exception e) {
        fmt::print("{}", e.what());
        return "";
    }
}

bool Request::to_file_tokens() {
    std::ofstream file("token.json");
    if (!file.is_open()) return false;
    try {
        file << json{
            {"AUTH_CODE", tokens.auth_code},
            {"ACCESS_TOKEN", tokens.access_token},
            {"REFRESH_TOKEN", tokens.refresh_token},
            {"EXPIRES_IN", tokens.expires_in}
        }.dump(4);
        return true;    
    } catch(json::exception e) {
        fmt::print("{}", e.what());
        return false;
    }
}

bool Request::set_tokens(const std::string_view grant_type) { 
    cpr::Payload payload{};
    if (grant_type == "auth_code") {
        payload = cpr::Payload{
            {"client_id", tokens.client_id},
            {"client_secret", tokens.client_secret},
            {"grant_type", grant_type.data()},
            {"code", tokens.auth_code},
            {"redirect_uri", "http://localhost:4000"}
        };
    } else {
        payload = cpr::Payload{
            {"client_id", tokens.client_id},
            {"client_secret", tokens.client_secret},
            {"grant_type", grant_type.data()},
            {"refresh_token", tokens.refresh_token},
            {"redirect_uri", "http://localhost:4000"}
        };
    }
    cpr::Response r = cpr::Post(
        cpr::Url{"https://osu.ppy.sh/oauth/token"},
        cpr::Header{
            {"Accept", "application/json"},
            {"Content-Type", "application/x-www-form-urlencoded"}
        },
        payload
    );
    if (r.status_code == 200) {
        auto j = json::parse(r.text);
        tokens.access_token = j.value("access_token", "");
        tokens.refresh_token = j.value("refresh_token", "");
        tokens.expires_in = j.value("expires_in", 86399);
        to_file_tokens();
        fmt::print("get access_key success\n");
        is_refresh_needed = false;
        return true;
    }
    fmt::print("get access_key failed\n");
    return false;
}

std::string Request::get_userid_v1(const std::string_view username) {
    cpr::Response r = cpr::Get(
        cpr::Url{"http://osu.ppy.sh/api/get_user"},
        cpr::Parameters{{"k", tokens.api_v1_key}, {"u", username.data()}}
    );
    size_t pos = r.text.find_first_of("0123456789");
    size_t endpos = r.text.find_first_not_of("0123456789", pos);
    return std::string(r.text.substr(pos, endpos - pos));
}

std::string Request::get_user(const std::string_view username) {
    cpr::Response r = cpr::Get(
        cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/users/@{}/osu", username)},
        cpr::Header{
            {"Authorization", "Bearer " + tokens.access_token},
            {"Content-Type", "application/json"},
            {"Accept", "application/json"}
        }
    );
    if (r.status_code == 200) {
        spdlog::info("get_user success");
        return r.text;
    }
    spdlog::info("get_user fail");
    return "";
}

std::string Request::get_user_score(const std::string_view beatmap, const std::string_view user) {
    cpr::Response r = cpr::Get(
        cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}/scores/users/{}", beatmap, user)},
        cpr::Header{
            {"Authorization", "Bearer " + tokens.access_token},
            {"Content-Type", "application/json"},
            {"Accept", "application/json"}
        }
    );
    if (r.status_code == 200) {
        spdlog::info("get_user_score success");
        return r.text;
    }
    spdlog::info("get_user_score failed");
    return "";
}

std::string Request::get_beatmap(const std::string_view beatmap) {
    cpr::Response r = cpr::Get(
        cpr::Url{fmt::format("https://osu.ppy.sh/api/v2/beatmaps/{}", beatmap)},
        cpr::Header{
            {"Authorization", "Bearer " + tokens.access_token},
            {"Content-Type", "application/json"},
            {"Accept", "application/json"}
        }
    );
    if (r.status_code == 200) {
        spdlog::info("get_beatmap success");
        return r.text;
    }
    spdlog::info("get_beatmap failed");
    return "";
}

