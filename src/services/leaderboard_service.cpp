#include "services/leaderboard_service.h"
#include "services/chat_context_service.h"
#include "services/beatmap_resolver_service.h"
#include "services/user_mapping_service.h"
#include "services/user_resolver_service.h"
#include "services/message_presenter_service.h"
#include "services/beatmap_performance_service.h"
#include <requests.h>
#include <beatmap_downloader.h>
#include <osu.h>
#include <osu_tools.h>
#include <utils.h>
#include <database.h>
#include <cache.h>
#include <error_messages.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <ranges>
#include <thread>

using json = nlohmann::json;
namespace stdr = std::ranges;

namespace services {

LeaderboardService::LeaderboardService(
    Request& request,
    BeatmapDownloader& beatmap_downloader,
    ChatContextService& chat_context_service,
    BeatmapResolverService& beatmap_resolver_service,
    UserMappingService& user_mapping_service,
    UserResolverService& user_resolver_service,
    MessagePresenterService& message_presenter,
    BeatmapPerformanceService& performance_service,
    dpp::cluster& bot
)
    : request_(request)
    , beatmap_downloader_(beatmap_downloader)
    , chat_context_service_(chat_context_service)
    , beatmap_resolver_service_(beatmap_resolver_service)
    , user_mapping_service_(user_mapping_service)
    , user_resolver_service_(user_resolver_service)
    , message_presenter_(message_presenter)
    , performance_service_(performance_service)
    , bot_(bot)
{}

dpp::message LeaderboardService::build_page(const LeaderboardState& state, const std::string& mods_filter) {
    constexpr size_t SCORES_PER_PAGE = 5;

    size_t start = state.current_page * SCORES_PER_PAGE;
    size_t end = std::min(start + SCORES_PER_PAGE, state.scores.size());

    // Use mods_filter from state if available, otherwise use parameter
    const std::string& active_mods = state.mods_filter.empty() ? mods_filter : state.mods_filter;

    // Parse .osu file once for PP calculation (in case of Loved maps)
    float approach_rate = 9.0f;
    float overall_difficulty = 9.0f;
    bool has_beatmap_data = false;
    uint32_t beatmap_id = state.beatmap.get_beatmap_id();
    uint32_t beatmapset_id = state.beatmap.get_beatmapset_id();

    // Get .osu file path and difficulty using performance service
    auto osu_file_path_opt = performance_service_.get_osu_file_path(beatmapset_id, beatmap_id);
    if (osu_file_path_opt) {
        auto diff_opt = performance_service_.get_difficulty(beatmapset_id, beatmap_id, active_mods);
        if (diff_opt) {
            approach_rate = diff_opt->approach_rate;
            overall_difficulty = diff_opt->overall_difficulty;
            has_beatmap_data = true;
            spdlog::debug("[LB] Got beatmap data from performance service: AR={:.2f} OD={:.2f}", approach_rate, overall_difficulty);
        }
    }

    std::string title = state.beatmap.to_string();
    if (!active_mods.empty()) {
        title += fmt::format(" +{}", active_mods);
    }

    // Find caller's rank if they have a score
    std::string caller_rank_text;
    if (state.caller_discord_id != 0) {
        try {
            auto& db = db::Database::instance();
            auto osu_user_id_opt = db.get_osu_user_id(state.caller_discord_id);

            if (osu_user_id_opt) {
                int64_t osu_user_id = *osu_user_id_opt;

                // Find user's position in leaderboard
                for (size_t i = 0; i < state.scores.size(); ++i) {
                    if (state.scores[i].get_user_id() == static_cast<size_t>(osu_user_id)) {
                        caller_rank_text = fmt::format(" • your rank: #{}", i + 1);
                        break;
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("Failed to look up caller rank: {}", e.what());
        }
    }

    // Build footer text based on number of pages
    std::string sort_text = (state.sort_method != LbSortMethod::PP)
        ? fmt::format(" • sorted by {}", sort_method_to_string(state.sort_method))
        : "";

    std::string footer_text;
    if (state.total_pages == 1) {
        // Simplified footer for single page
        footer_text = fmt::format("{} {} shown{}{}{}",
            state.scores.size(),
            state.scores.size() == 1 ? "score" : "scores",
            active_mods.empty() ? "" : fmt::format(" • Filter: +{}", active_mods),
            sort_text,
            caller_rank_text);
    } else {
        // Full footer for multiple pages
        footer_text = fmt::format("Page {}/{} • {}/{} {} shown{}{}{}",
            state.current_page + 1,
            state.total_pages,
            end,
            state.scores.size(),
            end == 1 ? "score" : "scores",
            active_mods.empty() ? "" : fmt::format(" • Filter: +{}", active_mods),
            sort_text,
            caller_rank_text);
    }

    // Build score presentations for the current page
    std::vector<ScorePresentation> scores_on_page;
    for (size_t i = start; i < end; i++) {
        const auto& score = state.scores[i];

        // Calculate PP if API returns 0 (for Loved maps)
        double display_pp = score.get_pp();
        if (display_pp <= 0.01 && osu_file_path_opt.has_value() && score.get_mode() == "osu") {
            // Use osu-tools for accurate PP calculation
            auto perf_opt = osu_tools::simulate_performance(
                *osu_file_path_opt,
                score.get_accuracy(),
                "osu",
                score.get_mods(),
                score.get_max_combo(),
                score.get_count_miss(),
                score.get_count_100(),
                score.get_count_50()
            );

            if (perf_opt.has_value()) {
                display_pp = perf_opt->pp;
                spdlog::debug("[LB] Calculated PP using osu-tools for score by {}: {:.2f}pp (aim: {:.2f}, speed: {:.2f}, acc: {:.2f})",
                    score.get_user_id(), display_pp, perf_opt->aim_pp, perf_opt->speed_pp, perf_opt->accuracy_pp);
            }
        }

        // Build header with calculated PP if needed
        std::string header;
        if (display_pp != score.get_pp()) {
            header = fmt::format("{} `{:.0f}pp` +{}", score.get_username(), display_pp, score.get_mods());
        } else {
            header = score.get_header();
        }

        scores_on_page.push_back({
            .rank = i + 1,
            .header = header,
            .body = score.get_body(state.beatmap.get_max_combo()),
            .display_pp = display_pp
        });
    }

    // Use presenter service to build the message
    return message_presenter_.build_leaderboard_page(
        state.beatmap,
        scores_on_page,
        footer_text,
        active_mods,
        state.total_pages,
        state.current_page
    );
}

void LeaderboardService::create_leaderboard(
    const dpp::message_create_t& event,
    const std::string& mods_filter,
    const std::optional<std::string>& beatmap_id_override,
    LbSortMethod sort_method)
{
    dpp::snowflake channel_id = event.msg.channel_id;

    // Use override if provided, otherwise get from chat context
    std::string beatmap_id;
    if (beatmap_id_override.has_value()) {
        beatmap_id = *beatmap_id_override;
    } else {
        std::string stored_id = chat_context_service_.get_beatmap_id(channel_id);
        beatmap_id = beatmap_resolver_service_.resolve_beatmap_id(stored_id);
    }

    if (beatmap_id.empty()) {
        event.reply(message_presenter_.build_error_message(error_messages::NO_BEATMAP_IN_CHANNEL));
        return;
    }

    // Show typing indicator
    bot_.channel_typing(event.msg.channel_id);

    auto start = std::chrono::steady_clock::now();

    std::string response_beatmap = request_.get_beatmap(beatmap_id);
    if (response_beatmap.empty()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        if (elapsed > 8) {
            event.reply(message_presenter_.build_error_message(
                fmt::format(error_messages::API_TIMEOUT_FORMAT, elapsed)));
        } else {
            event.reply(message_presenter_.build_error_message(error_messages::API_NO_RESPONSE));
        }
        spdlog::error("Unable to send request");
        return;
    }

    Beatmap beatmap(response_beatmap);

    // Download .osz file asynchronously
    uint32_t beatmapset_id = beatmap.get_beatmapset_id();
    std::jthread([this, beatmapset_id]() {
        beatmap_downloader_.download_osz(beatmapset_id);
    }).detach();

    // Fetch mod-adjusted beatmap attributes if mods filter is present
    if (!mods_filter.empty()) {
        uint32_t mods_bitset = utils::mods_string_to_bitset(mods_filter);
        std::string attributes_response = request_.get_beatmap_attributes(beatmap_id, mods_bitset);
        if (!attributes_response.empty()) {
            try {
                json attributes_json = json::parse(attributes_response);
                beatmap.set_modded_attributes(attributes_json);
            } catch (const json::exception& e) {
                spdlog::error("Failed to parse beatmap attributes: {}", e.what());
            }
        }
    }

    auto user_mappings = user_mapping_service_.get_all_mappings();
    std::vector<Score> scores(user_mappings.size());

    // Create stable index mapping to avoid race condition
    std::unordered_map<std::string, size_t> user_to_index;
    size_t idx = 0;
    for (const auto& [dis_id, user_id] : user_mappings) {
        user_to_index[user_id] = idx++;
    }

    // Parse required mods for filtering
    std::vector<std::string> required_mods;
    if (!mods_filter.empty()) {
        for (size_t i = 0; i + 1 < mods_filter.length(); i += 2) {
            required_mods.push_back(mods_filter.substr(i, 2));
        }
    }

    spdlog::info("[!lb] Fetching scores and usernames for {} users", user_mappings.size());

    // Use TBB for parallel score fetching
    arena_.execute([&]() { tbb::parallel_for_each(std::begin(user_mappings), std::end(user_mappings),
        [&](const auto& pair) {
            const auto& [dis_id, user_id] = pair;
            auto& score = scores[user_to_index[user_id]];
            std::string scores_j = request_.get_user_beatmap_score(beatmap_id, user_id, true);
            if (!scores_j.empty()) {
                json j = json::parse(scores_j);
                j = j["scores"];

                // Filter scores by mods if filter is specified
                if (!required_mods.empty()) {
                    auto filtered_scores = json::array();
                    for (const auto& score_json : j) {
                        // Parse mods from this score
                        std::string score_mods_str;
                        if (score_json.contains("mods") && score_json["mods"].is_array()) {
                            for (const auto& mod : score_json["mods"]) {
                                score_mods_str += mod.get<std::string>();
                            }
                        }
                        if (score_mods_str.empty()) score_mods_str = "NM";

                        // Check if all required mods are present
                        bool has_all_mods = true;
                        for (const auto& required_mod : required_mods) {
                            if (score_mods_str.find(required_mod) == std::string::npos) {
                                has_all_mods = false;
                                break;
                            }
                        }

                        if (has_all_mods) {
                            filtered_scores.push_back(score_json);
                        }
                    }
                    j = filtered_scores;
                }

                // If we have scores after filtering, take the best one
                if (!j.empty()) {
                    // sort specific user's scores
                    std::sort(j.begin(), j.end(), [](const json& a, const json& b) {
                        return std::make_tuple(a["pp"], a["score"]) > std::make_tuple(b["pp"], b["score"]);
                    });
                    score.from_json(j.at(0));
                    std::string username = user_resolver_service_.get_username_cached(score.get_user_id());
                    score.set_username(username);
                }
            }
        });
    });

    // Remove empty scores
    for (auto it = scores.begin(); it != scores.end();) {
        if (!it->is_empty) ++it;
        else scores.erase(it);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    if (scores.empty()) {
        if (elapsed > 8) {
            event.reply(message_presenter_.build_error_message(
                fmt::format(error_messages::API_TIMEOUT_FORMAT, elapsed)));
            spdlog::warn("[CMD] !lb took {}s and found no scores (slow API response)", elapsed);
        } else {
            event.reply(message_presenter_.build_error_message(
                fmt::format(error_messages::NO_SCORES_ON_BEATMAP_FORMAT, beatmap.to_string())));
        }
        return;
    }

    // Sort scores based on selected method
    if (scores.size() > 1) {
        switch (sort_method) {
            case LbSortMethod::Score:
                stdr::sort(scores, [](const Score& a, const Score& b) {
                    return a.get_total_score() > b.get_total_score();
                });
                break;
            case LbSortMethod::Acc:
                stdr::sort(scores, [](const Score& a, const Score& b) {
                    return a.get_accuracy() > b.get_accuracy();
                });
                break;
            case LbSortMethod::Combo:
                stdr::sort(scores, [](const Score& a, const Score& b) {
                    return a.get_max_combo() > b.get_max_combo();
                });
                break;
            case LbSortMethod::Date:
                stdr::sort(scores, [](const Score& a, const Score& b) {
                    return a.get_created_at() > b.get_created_at();
                });
                break;
            case LbSortMethod::PP:
            default:
                stdr::sort(scores, [](const Score& a, const Score& b) {
                    return std::make_tuple(a.get_pp(), a.get_total_score()) >
                        std::make_tuple(b.get_pp(), b.get_total_score());
                });
                break;
        }
    }

    if (elapsed > 8) {
        spdlog::warn("[CMD] !lb took {}s to complete (slow API response)", elapsed);
    }

    // Create leaderboard state and build first page
    std::string beatmap_mode = beatmap.get_mode();
    LeaderboardState lb_state(std::move(scores), std::move(beatmap), 0, beatmap_mode, mods_filter, sort_method, event.msg.author.id);
    dpp::message msg = build_page(lb_state);

    // Reply with leaderboard
    event.reply(msg, false, [this, lb_state = std::move(lb_state)](const dpp::confirmation_callback_t& callback) mutable {
        if (callback.is_error()) {
            spdlog::error("Failed to send leaderboard message");
            return;
        }
        auto reply_msg = callback.get<dpp::message>();

        // Store state in Memcached with 5-minute TTL
        bool cache_success = false;
        try {
            auto& cache = cache::MemcachedCache::instance();
            cache.cache_leaderboard(reply_msg.id.str(), lb_state);
            spdlog::debug("Stored leaderboard state for message {} in Memcached", reply_msg.id.str());
            cache_success = true;
        } catch (const std::exception& e) {
            spdlog::error("Failed to cache leaderboard state - pagination will not work: {}", e.what());
        }

        // Schedule button removal only if there's more than one page
        if (lb_state.total_pages > 1) {
            dpp::snowflake msg_id = reply_msg.id;
            dpp::snowflake chan_id = reply_msg.channel_id;

            // Remove buttons after 2 minutes
            auto ttl = std::chrono::minutes(2);
            schedule_button_removal(chan_id, msg_id, ttl);
        }
    });
}

void LeaderboardService::remove_message_components(dpp::snowflake channel_id, dpp::snowflake message_id) {
    // Get the message first, then remove components
    bot_.message_get(message_id, channel_id, [this, message_id](const dpp::confirmation_callback_t& callback) {
        if (callback.is_error()) {
            spdlog::warn("Failed to get message {} for button removal", message_id.str());
            return;
        }

        auto msg = callback.get<dpp::message>();
        msg.components.clear();

        // Edit message to remove components only
        bot_.message_edit(msg, [message_id](const dpp::confirmation_callback_t& edit_callback) {
            if (!edit_callback.is_error()) {
                // Delete from Memcached
                try {
                    auto& cache = cache::MemcachedCache::instance();
                    cache.delete_leaderboard(message_id.str());
                    spdlog::info("Buttons removed for leaderboard message {}", message_id.str());
                } catch (const std::exception& e) {
                    spdlog::debug("Cache cleanup for message {}: {}", message_id.str(), e.what());
                }
            } else {
                spdlog::warn("Failed to edit message {} to remove buttons: {}", message_id.str(), edit_callback.get_error().message);
            }
        });
    });
}

void LeaderboardService::schedule_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id, std::chrono::minutes ttl) {
    auto expires_at = std::chrono::system_clock::now() + ttl;

    // Store in database for persistence across restarts
    try {
        auto& db = db::Database::instance();
        db.register_pending_button_removal(channel_id, message_id, expires_at);
    } catch (const std::exception& e) {
        spdlog::error("Failed to register pending button removal in database: {}", e.what());
    }

    // Schedule removal thread
    std::jthread([this, channel_id, message_id, ttl]() {
        std::this_thread::sleep_for(ttl);

        // Remove components
        remove_message_components(channel_id, message_id);

        // Remove from database after invalidation
        try {
            auto& db = db::Database::instance();
            db.remove_pending_button_removal(channel_id, message_id);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to clear pending button removal from database: {}", e.what());
        }
    }).detach();
}

} // namespace services
