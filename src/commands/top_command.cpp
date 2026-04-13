#include "commands/top_command.h"
#include "services/service_container.h"
#include "services/user_resolver_service.h"
#include "services/message_presenter_service.h"
#include "services/user_settings_service.h"
#include <requests.h>
#include <osu.h>
#include <cache.h>
#include <database.h>
#include <error_messages.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ranges>
#include <random>

using json = nlohmann::json;
namespace stdr = std::ranges;

namespace commands {

std::vector<std::string> TopCommand::get_aliases() const {
    return {"top", "ещз"};
}

bool TopCommand::matches(const CommandContext& ctx) const {
    auto check_boundary = [](const std::string& str, size_t len) {
        if (str.length() == len) return true;
        char next = str[len];
        return next == ' ' || next == ':' || next == '\t';
    };

    std::string p = ctx.prefix;
    if (ctx.content_lower.find(p + "top") == 0 && check_boundary(ctx.content_lower, p.size() + 3)) return true;
    // Cyrillic: tolower doesn't work with UTF-8, check both cases
    std::string cyr_lo = p + "ещз", cyr_up = p + "ЕЩЗ";
    if ((ctx.content.find(cyr_lo) == 0 || ctx.content.find(cyr_up) == 0) && check_boundary(ctx.content, cyr_lo.size())) return true;
    return false;
}

TopCommand::ParsedParams TopCommand::parse(const std::string& content) const {
    ParsedParams result;

    // Find command end
    size_t cmd_end = content.find(' ');
    if (cmd_end == std::string::npos) {
        cmd_end = content.length();
    }

    // Check for mode specification (e.g., !top:taiko)
    size_t colon_pos = content.find(':');
    if (colon_pos != std::string::npos && colon_pos < cmd_end) {
        size_t mode_end = content.find(' ', colon_pos);
        if (mode_end == std::string::npos) {
            mode_end = content.length();
        }
        result.mode = content.substr(colon_pos + 1, mode_end - colon_pos - 1);
        std::transform(result.mode.begin(), result.mode.end(), result.mode.begin(), ::tolower);

        // Validate mode
        if (result.mode != "osu" && result.mode != "taiko" && result.mode != "catch" &&
            result.mode != "mania" && result.mode != "fruits" && result.mode != "ctb") {
            result.valid = false;
            result.error_message = "Invalid mode. Supported modes: `osu`, `taiko`, `catch`/`fruits`, `mania`";
            return result;
        }

        // Normalize mode names
        if (result.mode == "ctb") result.mode = "fruits";
        if (result.mode == "catch") result.mode = "fruits";

        cmd_end = mode_end;
    }

    // Extract params after command
    std::string params = content.length() > cmd_end ? content.substr(cmd_end) : "";

    // Trim leading spaces
    size_t start = params.find_first_not_of(" \t");
    if (start != std::string::npos) {
        params = params.substr(start);
    } else {
        params = "";
    }

    // Parse parameters
    std::vector<std::string> tokens;
    std::istringstream iss(params);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& tok = tokens[i];

        // Sort option
        if (tok == "-s" || tok == "--sort") {
            if (i + 1 < tokens.size()) {
                std::string sort_str = tokens[++i];
                std::transform(sort_str.begin(), sort_str.end(), sort_str.begin(), ::tolower);

                if (sort_str == "pp") result.sort_method = TopSortMethod::PP;
                else if (sort_str == "acc" || sort_str == "accuracy") result.sort_method = TopSortMethod::Acc;
                else if (sort_str == "score") result.sort_method = TopSortMethod::Score;
                else if (sort_str == "combo") result.sort_method = TopSortMethod::Combo;
                else if (sort_str == "date" || sort_str == "recent") result.sort_method = TopSortMethod::Date;
                else if (sort_str == "miss" || sort_str == "misses") result.sort_method = TopSortMethod::Misses;
                else {
                    result.valid = false;
                    result.error_message = "Invalid sort method. Use: `pp`, `acc`, `score`, `combo`, `date`, `misses`";
                    return result;
                }
            }
            continue;
        }

        // Mods filter
        if (tok == "-m" || tok == "--mods") {
            if (i + 1 < tokens.size()) {
                result.mods_filter = tokens[++i];
                // Normalize: remove + prefix if present
                if (!result.mods_filter.empty() && result.mods_filter[0] == '+') {
                    result.mods_filter = result.mods_filter.substr(1);
                }
                std::transform(result.mods_filter.begin(), result.mods_filter.end(),
                               result.mods_filter.begin(), ::toupper);
            }
            continue;
        }

        // Grade filter
        if (tok == "-g" || tok == "--grade") {
            if (i + 1 < tokens.size()) {
                result.grade_filter = tokens[++i];
                std::transform(result.grade_filter.begin(), result.grade_filter.end(),
                               result.grade_filter.begin(), ::toupper);

                // Validate grade
                if (result.grade_filter != "SS" && result.grade_filter != "S" &&
                    result.grade_filter != "A" && result.grade_filter != "B" &&
                    result.grade_filter != "C" && result.grade_filter != "D" &&
                    result.grade_filter != "X" && result.grade_filter != "XH" &&
                    result.grade_filter != "SH") {
                    result.valid = false;
                    result.error_message = "Invalid grade. Use: `SS`, `S`, `A`, `B`, `C`, `D`";
                    return result;
                }
            }
            continue;
        }

        // Reverse flag
        if (tok == "-r" || tok == "--reverse") {
            result.reverse = true;
            continue;
        }

        // Index
        if (tok == "--index" || tok == "-i") {
            if (i + 1 < tokens.size()) {
                try {
                    result.index = std::stoul(tokens[++i]);
                    if (result.index < 1 || result.index > 100) {
                        result.valid = false;
                        result.error_message = "Index must be between 1 and 100";
                        return result;
                    }
                } catch (...) {
                    result.valid = false;
                    result.error_message = "Invalid index number";
                    return result;
                }
            }
            continue;
        }

        // Mods shorthand (starts with + or -)
        if (tok.length() > 1 && (tok[0] == '+' || tok[0] == '-')) {
            result.mods_filter = tok;
            continue;
        }

        // Otherwise, it's a username
        if (result.username.empty()) {
            result.username = tok;
        }
    }

    return result;
}

void TopCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[top] ServiceContainer is null");
        return;
    }

    ParsedParams params;

    if (ctx.is_slash()) {
        // Get parameters directly from slash command
        if (auto username = ctx.get_string_param("username")) {
            params.username = *username;
        }
        if (auto mode = ctx.get_string_param("mode")) {
            params.mode = *mode;
        }
        if (auto mods = ctx.get_string_param("mods")) {
            params.mods_filter = *mods;
            std::transform(params.mods_filter.begin(), params.mods_filter.end(),
                           params.mods_filter.begin(), ::toupper);
        }
        if (auto grade = ctx.get_string_param("grade")) {
            params.grade_filter = *grade;
        }
        if (auto sort = ctx.get_string_param("sort")) {
            std::string sort_str = *sort;
            std::transform(sort_str.begin(), sort_str.end(), sort_str.begin(), ::tolower);

            if (sort_str == "pp") params.sort_method = TopSortMethod::PP;
            else if (sort_str == "acc") params.sort_method = TopSortMethod::Acc;
            else if (sort_str == "score") params.sort_method = TopSortMethod::Score;
            else if (sort_str == "combo") params.sort_method = TopSortMethod::Combo;
            else if (sort_str == "date") params.sort_method = TopSortMethod::Date;
            else if (sort_str == "misses") params.sort_method = TopSortMethod::Misses;
        }
        if (auto reverse = ctx.get_bool_param("reverse"); reverse && *reverse) {
            params.reverse = true;
        }
        if (auto index = ctx.get_int_param("index")) {
            params.index = static_cast<size_t>(*index);
        }
    } else {
        params = parse(ctx.content);
        if (!params.valid) {
            ctx.reply(params.error_message);
            return;
        }
    }

    // Resolve osu user_id
    auto resolve_result = s->user_resolver_service.resolve(params.username, ctx.author_id());
    if (!resolve_result) {
        if (params.username.empty()) {
            std::string token;
            try {
                auto& mc = cache::MemcachedCache::instance();
                token = utils::generate_secure_token();

                json token_data;
                token_data["discord_id"] = ctx.author_id().str();
                token_data["link_url"] = s->config.public_url + "/osu/link/" + token;
                mc.set("osu_link_token:" + token, token_data.dump(), std::chrono::seconds(300));
            } catch (const std::exception& e) {
                spdlog::error("[top] Failed to generate link token: {}", e.what());
            }

            auto embed = dpp::embed()
                .set_color(0xff66aa)
                .set_title("Link your osu! Account")
                .set_description("Link your osu! account to use commands without specifying your username.")
                .add_field("Option 1: Website", fmt::format("[Open Settings]({})", s->config.public_url + "/osu/settings"), true)
                .add_field("Option 2: Direct Link", "Click the button below to get a link in DMs", true)
                .set_footer(dpp::embed_footer().set_text("Link expires in 5 minutes"));

            dpp::message msg;
            msg.add_embed(embed);
            if (!token.empty()) {
                msg.add_component(
                    dpp::component().add_component(
                        dpp::component()
                            .set_type(dpp::cot_button)
                            .set_label("Send Link to DMs")
                            .set_style(dpp::cos_primary)
                            .set_id("osu_link_dm:" + token)
                    )
                );
            }
            ctx.reply(std::move(msg));
        } else {
            ctx.reply(s->message_presenter.build_error_message(resolve_result.error_message));
        }
        return;
    }
    int64_t osu_user_id = resolve_result.osu_user_id;
    std::string resolved_username = s->user_resolver_service.get_username_cached(osu_user_id);

    // Check if user should be prompted to OAuth link
    bool suggest_oauth_link = false;
    if (params.username.empty()) {
        auto& db = db::Database::instance();
        if (!db.is_user_oauth_linked(ctx.author_id())) {
            suggest_oauth_link = true;
        }
    }

    // Show typing indicator
    if (!ctx.is_slash()) {
        s->bot.channel_typing(ctx.channel_id());
    }

    // Fetch best scores
    std::string scores_response = s->request.get_user_best_scores(
        std::to_string(osu_user_id), params.mode, 100, 0);

    if (scores_response.empty()) {
        ctx.reply(s->message_presenter.build_error_message("Failed to fetch scores from osu! API"));
        return;
    }

    // Parse scores
    std::vector<Score> scores;
    try {
        json scores_json = json::parse(scores_response);
        if (!scores_json.is_array() || scores_json.empty()) {
            ctx.reply(s->message_presenter.build_error_message("No top scores found"));
            return;
        }

        for (const auto& score_json : scores_json) {
            Score score;
            score.from_json(score_json);
            scores.push_back(score);
        }
    } catch (const json::exception& e) {
        ctx.reply(s->message_presenter.build_error_message("Failed to parse scores"));
        spdlog::error("Failed to parse scores: {}", e.what());
        return;
    }

    // Apply mods filter
    if (!params.mods_filter.empty()) {
        bool exclude = params.mods_filter[0] == '-';
        std::string mods_to_check = exclude ? params.mods_filter.substr(1) : params.mods_filter;

        // Parse mods into pairs
        std::vector<std::string> required_mods;
        for (size_t i = 0; i + 1 < mods_to_check.length(); i += 2) {
            required_mods.push_back(mods_to_check.substr(i, 2));
        }

        scores.erase(std::remove_if(scores.begin(), scores.end(),
            [&](const Score& score) {
                const std::string& score_mods = score.get_mods();

                for (const auto& mod : required_mods) {
                    bool has_mod = score_mods.find(mod) != std::string::npos;
                    if (exclude && has_mod) return true;   // Exclude if has mod
                    if (!exclude && !has_mod) return true; // Include only if has mod
                }
                return false;
            }
        ), scores.end());
    }

    // Apply grade filter
    if (!params.grade_filter.empty()) {
        scores.erase(std::remove_if(scores.begin(), scores.end(),
            [&](const Score& score) {
                const std::string& rank = score.get_rank();
                // Normalize rank comparison
                if (params.grade_filter == "SS") {
                    return rank != "X" && rank != "XH" && rank != "SS" && rank != "SSH";
                }
                if (params.grade_filter == "S") {
                    return rank != "S" && rank != "SH";
                }
                return rank != params.grade_filter;
            }
        ), scores.end());
    }

    // Sort scores
    auto compare = [&params](const Score& a, const Score& b) {
        bool result;
        switch (params.sort_method) {
            case TopSortMethod::Acc:
                result = a.get_accuracy() > b.get_accuracy();
                break;
            case TopSortMethod::Score:
                result = a.get_total_score() > b.get_total_score();
                break;
            case TopSortMethod::Combo:
                result = a.get_max_combo() > b.get_max_combo();
                break;
            case TopSortMethod::Date:
                result = a.get_created_at() > b.get_created_at();
                break;
            case TopSortMethod::Misses:
                result = a.get_count_miss() < b.get_count_miss();  // Less misses = better
                break;
            case TopSortMethod::PP:
            default:
                result = a.get_pp() > b.get_pp();
                break;
        }
        return params.reverse ? !result : result;
    };

    stdr::sort(scores, compare);

    if (scores.empty()) {
        std::string filter_msg;
        if (!params.mods_filter.empty()) filter_msg += fmt::format(" mods={}", params.mods_filter);
        if (!params.grade_filter.empty()) filter_msg += fmt::format(" grade={}", params.grade_filter);
        ctx.reply(s->message_presenter.build_error_message(
            fmt::format("No scores found matching filters:{}", filter_msg)));
        return;
    }

    // If specific index requested
    if (params.index > 0) {
        if (params.index > scores.size()) {
            ctx.reply(s->message_presenter.build_error_message(
                fmt::format("Score #{} not found (only {} scores after filtering)", params.index, scores.size())));
            return;
        }
        // Show single score (could reuse rs embed format)
        // For now, just show the first page with that score
        scores = {scores[params.index - 1]};
    }

    auto preset = s->user_settings_service.get_preset(ctx.author_id());

    // Create state for pagination
    TopState top_state(
        std::move(scores),
        resolved_username,
        osu_user_id,
        params.mode,
        params.mods_filter,
        params.grade_filter,
        params.sort_method,
        params.reverse,
        ctx.author_id(),
        preset
    );

    // Build first page
    dpp::message msg = s->message_presenter.build_top_page(top_state);

    // Add OAuth link suggestion if user is not OAuth linked
    if (suggest_oauth_link) {
        std::string hint = fmt::format("-# Link your osu! account with `{}link` or `/link` for a better experience", ctx.prefix);
        if (msg.content.empty()) {
            msg.content = hint;
        } else {
            msg.content = hint + "\n" + msg.content;
        }
    }

    // Reply with message
    ctx.reply(std::move(msg), false, [s, top_state = std::move(top_state)](const dpp::confirmation_callback_t& callback) mutable {
        if (callback.is_error()) {
            spdlog::error("Failed to send top scores message");
            return;
        }
        auto reply_msg = callback.get<dpp::message>();

        // Store state in Memcached
        try {
            auto& cache = cache::MemcachedCache::instance();
            cache.cache_top(reply_msg.id.str(), top_state);
            spdlog::debug("Stored top state for message {} in Memcached", reply_msg.id.str());
        } catch (const std::exception& e) {
            spdlog::error("Failed to cache top state: {}", e.what());
        }
    });
}

} // namespace commands
