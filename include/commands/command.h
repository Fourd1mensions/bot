#pragma once

#include <dpp/dpp.h>
#include <string>
#include <vector>
#include <functional>
#include <variant>
#include <optional>

// Forward declaration
struct ServiceContainer;

namespace commands {

/**
 * Unified context for command execution.
 * Works with both text messages and slash commands.
 */
struct UnifiedContext {
    using EventVariant = std::variant<dpp::message_create_t, dpp::slashcommand_t>;

    EventVariant event;
    std::string content;          // Original content or constructed args
    std::string content_lower;    // Lowercase content for matching
    ServiceContainer* services;
    std::string prefix = "!";

    // Unified accessors
    dpp::snowflake channel_id() const {
        return std::visit([](auto&& e) -> dpp::snowflake {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dpp::message_create_t>) {
                return e.msg.channel_id;
            } else {
                return e.command.channel_id;
            }
        }, event);
    }

    dpp::snowflake author_id() const {
        return std::visit([](auto&& e) -> dpp::snowflake {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dpp::message_create_t>) {
                return e.msg.author.id;
            } else {
                return e.command.usr.id;
            }
        }, event);
    }

    dpp::snowflake guild_id() const {
        return std::visit([](auto&& e) -> dpp::snowflake {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dpp::message_create_t>) {
                return e.msg.guild_id;
            } else {
                return e.command.guild_id;
            }
        }, event);
    }

    bool is_slash() const {
        return std::holds_alternative<dpp::slashcommand_t>(event);
    }

    // Reply methods - for slash commands after thinking(), use edit_original_response
    void reply(const std::string& text) const {
        std::visit([&text](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dpp::message_create_t>) {
                e.reply(text);
            } else {
                // After thinking(), must use edit_original_response
                e.edit_original_response(dpp::message(text));
            }
        }, event);
    }

    void reply(const dpp::message& msg) const {
        std::visit([&msg](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dpp::message_create_t>) {
                e.reply(msg);
            } else {
                // After thinking(), must use edit_original_response
                e.edit_original_response(msg);
            }
        }, event);
    }

    void reply(dpp::message&& msg, bool ephemeral, std::function<void(const dpp::confirmation_callback_t&)> callback) const {
        std::visit([&](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dpp::message_create_t>) {
                e.reply(std::move(msg), false, callback);
            } else {
                // After thinking(), must use edit_original_response with callback
                e.edit_original_response(std::move(msg), callback);
            }
        }, event);
    }

    // Get parameter from slash command with optional type
    template<typename T>
    std::optional<T> get_param(const std::string& name) const {
        if (auto* slash = std::get_if<dpp::slashcommand_t>(&event)) {
            try {
                auto param = slash->get_parameter(name);
                if (std::holds_alternative<T>(param)) {
                    return std::get<T>(param);
                }
            } catch (...) {}
        }
        return std::nullopt;
    }

    // Specialization for getting string from slash or empty for text
    std::optional<std::string> get_string_param(const std::string& name) const {
        return get_param<std::string>(name);
    }

    std::optional<int64_t> get_int_param(const std::string& name) const {
        return get_param<int64_t>(name);
    }

    std::optional<double> get_double_param(const std::string& name) const {
        return get_param<double>(name);
    }

    std::optional<bool> get_bool_param(const std::string& name) const {
        return get_param<bool>(name);
    }
};

/**
 * Context passed to command handlers from text messages.
 * Contains everything needed to execute a command.
 * Note: event is stored by value to ensure thread safety when
 * commands execute asynchronously.
 */
struct CommandContext {
    dpp::message_create_t event;  // Stored by value for thread safety
    std::string content;          // Original message content
    std::string content_lower;    // Lowercase content for matching
    std::string args;             // Arguments after command name
    ServiceContainer* services;   // Access to all services
    std::string prefix = "!";     // Command prefix

    // Convert to UnifiedContext
    UnifiedContext to_unified() const {
        return UnifiedContext{
            .event = event,
            .content = content,
            .content_lower = content_lower,
            .services = services,
            .prefix = prefix
        };
    }
};

/**
 * Context passed to command handlers from slash commands.
 * Provides unified interface for commands that can work with both text and slash input.
 */
struct SlashCommandContext {
    dpp::slashcommand_t event;    // Stored by value for thread safety
    std::string command_name;     // The slash command name (without /)
    std::string args;             // Constructed arguments string from parameters
    ServiceContainer* services;   // Access to all services
    std::string prefix = "!";    // Command prefix

    // Helper to get channel_id consistently
    dpp::snowflake channel_id() const { return event.command.channel_id; }

    // Helper to get user consistently
    const dpp::user& user() const { return event.command.usr; }

    // Convert to UnifiedContext
    UnifiedContext to_unified() const {
        std::string content_lower = args;
        std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return UnifiedContext{
            .event = event,
            .content = args,
            .content_lower = content_lower,
            .services = services,
            .prefix = prefix
        };
    }
};

/**
 * Base interface for text commands (!lb, !rs, etc.)
 */
class ICommand {
public:
    virtual ~ICommand() = default;

    /**
     * Get command aliases (e.g., {"!lb", "!ди"})
     */
    virtual std::vector<std::string> get_aliases() const = 0;

    /**
     * Get slash command name this command maps to (e.g., "lb", "rs").
     * Return empty string if this command doesn't support slash commands.
     */
    virtual std::string get_slash_name() const { return ""; }

    /**
     * Check if this command matches the message.
     * Default implementation checks if content starts with alias followed by space or end.
     */
    virtual bool matches(const CommandContext& ctx) const {
        for (const auto& alias : get_aliases()) {
            std::string full = ctx.prefix + alias;
            if (ctx.content_lower.find(full) == 0) {
                size_t full_len = full.length();
                if (ctx.content_lower.length() == full_len) {
                    return true;
                }
                char next_char = ctx.content_lower[full_len];
                if (next_char == ' ' || next_char == ':' || next_char == '\t') {
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * Execute the command from text message.
     * Called in a separate thread by the router.
     * Default implementation converts to unified context.
     */
    virtual void execute(const CommandContext& ctx) {
        execute_unified(ctx.to_unified());
    }

    /**
     * Execute the command from slash command.
     * Default implementation converts to unified context.
     */
    virtual void execute(const SlashCommandContext& ctx) {
        execute_unified(ctx.to_unified());
    }

    /**
     * Execute command with unified context.
     * Override this for commands that work with both text and slash.
     * Default implementation does nothing.
     */
    virtual void execute_unified(const UnifiedContext& ctx) {
        // Default: not implemented
    }

    /**
     * Check if this command supports slash commands.
     */
    bool supports_slash() const { return !get_slash_name().empty(); }
};

} // namespace commands
