#include "commands/users_command.h"
#include "services/service_container.h"
#include "services/user_resolver_service.h"
#include <database.h>
#include <cache.h>
#include <state/session_state.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <dpp/dpp.h>

namespace commands {

std::vector<std::string> UsersCommand::get_aliases() const {
    return {"!users", "!tracked"};
}

namespace {
dpp::message build_users_page(const UsersState& state) {
    dpp::embed embed;
    embed.set_color(0xff66ab);  // osu! pink
    embed.set_title(fmt::format("Tracked Users ({})", state.users.size()));

    std::string description;
    size_t start_idx = state.current_page * UsersState::USERS_PER_PAGE;
    size_t end_idx = std::min(start_idx + UsersState::USERS_PER_PAGE, state.users.size());

    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& user = state.users[i];
        description += fmt::format("[{}](https://osu.ppy.sh/users/{}) • <@{}>\n",
            user.osu_username, user.osu_user_id, user.discord_id.str());
    }

    embed.set_description(description);

    // Footer with page info
    if (state.total_pages > 1) {
        embed.set_footer(dpp::embed_footer()
            .set_text(fmt::format("Page {}/{}", state.current_page + 1, state.total_pages)));
    }

    dpp::message msg;
    msg.add_embed(embed);

    // Add pagination buttons if more than one page
    if (state.total_pages > 1) {
        dpp::component row;
        row.set_type(dpp::cot_action_row);

        dpp::component prev_btn;
        prev_btn.set_type(dpp::cot_button)
            .set_style(dpp::cos_secondary)
            .set_label("◀")
            .set_id("users_prev")
            .set_disabled(state.current_page == 0);

        dpp::component next_btn;
        next_btn.set_type(dpp::cot_button)
            .set_style(dpp::cos_secondary)
            .set_label("▶")
            .set_id("users_next")
            .set_disabled(state.current_page >= state.total_pages - 1);

        row.add_component(prev_btn);
        row.add_component(next_btn);
        msg.add_component(row);
    }

    return msg;
}
} // anonymous namespace

void UsersCommand::execute_unified(const UnifiedContext& ctx) {
    auto* s = ctx.services;
    if (!s) {
        spdlog::error("[users] ServiceContainer is null");
        return;
    }

    // Get all user mappings from database
    auto mappings = db::Database::instance().get_all_user_mappings();

    if (mappings.empty()) {
        dpp::embed embed;
        embed.set_color(0x2f3136);
        embed.set_title("Tracked Users");
        embed.set_description("No tracked users found.\nUse `/set` to link your osu! account.");
        ctx.reply(dpp::message().add_embed(embed));
        return;
    }

    // Build user list with usernames
    std::vector<UserMapping> users;
    users.reserve(mappings.size());

    for (const auto& [discord_id, osu_user_id] : mappings) {
        std::string osu_username = s->user_resolver_service.get_username_cached(osu_user_id);
        if (osu_username.empty()) {
            osu_username = "Unknown";
        }
        users.emplace_back(discord_id, osu_user_id, osu_username);
    }

    // Create state
    UsersState state(std::move(users), ctx.author_id());
    spdlog::info("[users] Created state: {} users, {} pages", state.users.size(), state.total_pages);

    // Build first page
    dpp::message msg = build_users_page(state);

    // Send and cache if paginated
    if (state.total_pages > 1) {
        // For text commands, use reply with callback
        if (!ctx.is_slash()) {
            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, dpp::message_create_t>) {
                    e.reply(msg, false, [state](const dpp::confirmation_callback_t& callback) {
                        if (callback.is_error()) {
                            spdlog::error("[users] Failed to send message: {}", callback.get_error().message);
                            return;
                        }
                        auto reply_msg = callback.get<dpp::message>();
                        spdlog::info("[users] Message sent, id={}", reply_msg.id.str());

                        // Cache state for pagination
                        try {
                            auto& cache = cache::MemcachedCache::instance();
                            cache.cache_users(reply_msg.id.str(), state);
                            spdlog::info("[users] Cached state for message {}", reply_msg.id.str());
                        } catch (const std::exception& e) {
                            spdlog::error("[users] Failed to cache state: {}", e.what());
                        }
                    });
                }
            }, ctx.event);
        } else {
            // Slash command - just send without pagination for now
            ctx.reply(msg);
        }
    } else {
        ctx.reply(msg);
    }
}

} // namespace commands
