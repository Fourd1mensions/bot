#pragma once

#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <optional>
#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <unordered_set>
#include <dpp/dpp.h>

namespace db {

// Structure to represent beatmap .osz file metadata
struct BeatmapFile {
    int64_t beatmapset_id;
    std::string osz_path;
    std::optional<std::string> mirror_hostname;
    std::optional<std::chrono::system_clock::time_point> last_accessed;
    std::optional<std::chrono::system_clock::time_point> created_at;
};

// Structure to represent temporary beatmap extract
struct BeatmapExtract {
    std::string extract_id;
    int64_t beatmapset_id;
    std::string extract_path;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point expires_at;
};

// Structure to represent cached user data
struct CachedUser {
    int64_t user_id;
    std::string username;
    std::chrono::system_clock::time_point expires_at;
};

// Structure to represent a crawled Discord message
struct CrawledMessage {
    dpp::snowflake message_id;
    dpp::snowflake channel_id;
    dpp::snowflake author_id;
    std::string content;
    std::chrono::system_clock::time_point created_at;
    bool is_bot{false};

    // Reply tracking (0 means not a reply)
    dpp::snowflake reply_to_message_id{0};

    // Attachments
    bool has_attachments{false};
    std::vector<std::string> attachment_urls;

    // Edit tracking (nullopt if never edited)
    std::optional<std::chrono::system_clock::time_point> edited_at;
};

// Structure to represent channel crawl progress
struct ChannelCrawlProgress {
    dpp::snowflake channel_id;
    dpp::snowflake guild_id;
    dpp::snowflake oldest_message_id{0};
    dpp::snowflake newest_message_id{0};
    size_t total_messages{0};
    std::chrono::system_clock::time_point last_crawl;
    bool initial_crawl_complete{false};
};

// Structure for word statistics entry
struct WordStatEntry {
    std::string word;
    size_t count{0};
    std::string language;
};

// Structure for phrase statistics entry (bigrams/trigrams with PMI, NPMI, LLR)
struct PhraseStatEntry {
    std::string phrase;
    std::vector<std::string> words;
    int word_count{0};
    size_t count{0};
    std::optional<double> pmi_score;
    std::optional<double> npmi_score;      // Normalized PMI [-1, 1]
    std::optional<double> llr_score;       // Log-Likelihood Ratio
    std::optional<double> uniqueness_score; // User-specific: how unique is this phrase for user
    std::optional<double> trend_score;     // Growth rate over last week
    std::optional<std::chrono::system_clock::time_point> first_seen;
    bool is_new{false};                    // Appeared in last 7 days
    std::string language;
    std::string lemmatized_phrase;         // Lemmatized version for grouping
};

// Structure for crawl status summary
struct CrawlStatusSummary {
    size_t total_channels{0};
    size_t completed_channels{0};
    size_t total_messages{0};
};

// Structure for message author info
struct MessageAuthor {
    dpp::snowflake author_id;
    size_t message_count{0};
    std::string username;      // Discord username
    std::string display_name;  // Global name (may be empty)
    std::string avatar_hash;   // For CDN URL construction
};

// Structure for Discord user cache
struct DiscordUser {
    dpp::snowflake user_id;
    std::string username;
    std::string global_name;
    std::string avatar_hash;
    bool is_bot{false};
};

// Structure for channel info with message counts
struct MessageChannel {
    dpp::snowflake channel_id;
    size_t message_count{0};
    std::string channel_name;  // Cached from Discord
};

// Structure for Discord channel cache
struct DiscordChannel {
    dpp::snowflake channel_id;
    dpp::snowflake guild_id;
    std::string channel_name;
    int channel_type{0};  // Discord channel type (0 = text, 2 = voice, etc.)
};

// Structure for guild info cache
struct GuildInfo {
    dpp::snowflake guild_id;
    std::string name;
    std::string icon_hash;
    size_t member_count{0};
};

// Structure for template audit log entry
struct TemplateAuditEntry {
    int64_t id{0};
    dpp::snowflake discord_id;
    std::string discord_username;
    std::string action;               // "update" or "reset"
    std::string command_id;           // "rs", "compare", "map", etc.
    std::string preset;               // "compact", "classic", "extended", or empty
    std::string old_fields_json;      // Previous template fields as JSON
    std::string new_fields_json;      // New template fields as JSON
    std::chrono::system_clock::time_point created_at;
};

// Connection pool for PostgreSQL
class ConnectionPool {
public:
    ConnectionPool(const std::string& connection_string, size_t pool_size = 5);
    ~ConnectionPool();

    // Get a connection from the pool (blocks if none available)
    std::unique_ptr<pqxx::connection> acquire();

    // Return a connection to the pool
    void release(std::unique_ptr<pqxx::connection> conn);

private:
    std::string conn_string_;
    std::queue<std::unique_ptr<pqxx::connection>> connections_;
    std::mutex mutex_;
    std::condition_variable cv_;
    size_t pool_size_;
};

// Main database interface
class Database {
public:
    // Initialize database with connection parameters
    static void init(const std::string& host, int port,
                    const std::string& dbname, const std::string& user,
                    const std::string& password, size_t pool_size = 5);

    // Get the singleton instance
    static Database& instance();

    // Disable copy and move
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Users table operations
    void set_user_mapping(dpp::snowflake discord_id, int64_t osu_user_id, bool is_oauth = false);
    std::optional<int64_t> get_osu_user_id(dpp::snowflake discord_id);
    bool remove_user_mapping(dpp::snowflake discord_id);
    std::vector<std::pair<dpp::snowflake, int64_t>> get_all_user_mappings();
    bool is_user_oauth_linked(dpp::snowflake discord_id);

    // Chat map table operations
    void set_chat_context(dpp::snowflake channel_id, dpp::snowflake message_id,
                         const std::string& beatmap_id);
    std::optional<std::pair<dpp::snowflake, std::string>> get_chat_context(dpp::snowflake channel_id);
    bool remove_chat_context(dpp::snowflake channel_id);

    // Beatmap files table operations (.osz storage)
    void register_beatmap_file(int64_t beatmapset_id, const std::string& osz_path,
                              const std::optional<std::string>& mirror_hostname = std::nullopt,
                              int64_t file_size = 0);
    void update_file_access(int64_t beatmapset_id);
    std::optional<BeatmapFile> get_beatmap_file(int64_t beatmapset_id);
    std::vector<BeatmapFile> get_all_beatmap_files();
    bool beatmap_file_exists(int64_t beatmapset_id);
    bool remove_beatmap_file(int64_t beatmapset_id);
    size_t count_beatmap_files();

    // Beatmap extracts table operations (temporary extracted files)
    std::string create_beatmap_extract(int64_t beatmapset_id, const std::string& extract_path,
                                       std::chrono::hours ttl = std::chrono::hours(24));
    std::optional<BeatmapExtract> get_beatmap_extract(const std::string& extract_id);
    bool remove_beatmap_extract(const std::string& extract_id);
    std::vector<BeatmapExtract> cleanup_expired_extracts();

    // Beatmap ID cache operations (beatmap_id -> beatmapset_id mapping)
    void cache_beatmap_id(int64_t beatmap_id, int64_t beatmapset_id, const std::string& mode = "osu");
    std::optional<int64_t> get_beatmapset_id(int64_t beatmap_id);
    void update_beatmap_id_access(int64_t beatmap_id);

    // Individual .osu files tracking
    void register_osu_file(int64_t beatmap_id, int64_t beatmapset_id,
                          const std::string& file_path, int64_t file_size = 0);
    void update_osu_file_access(int64_t beatmap_id);
    bool osu_file_exists(int64_t beatmap_id);
    std::optional<std::string> get_osu_file_path(int64_t beatmap_id);

    // User cache table operations
    void cache_username(int64_t user_id, const std::string& username,
                       std::chrono::minutes ttl = std::chrono::minutes(60));
    std::optional<std::string> get_cached_username(int64_t user_id);
    void cleanup_expired_cache();

    // Pending button removals table operations
    void register_pending_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id,
                                         std::chrono::system_clock::time_point expires_at);
    std::vector<std::tuple<dpp::snowflake, dpp::snowflake, std::string>> get_expired_button_removals();
    std::vector<std::tuple<dpp::snowflake, dpp::snowflake, std::chrono::system_clock::time_point, std::string>> get_all_pending_removals();
    bool remove_pending_button_removal(dpp::snowflake channel_id, dpp::snowflake message_id);

    // Health check
    bool is_connected();

    // ============================================================================
    // User settings operations
    // ============================================================================

    void set_embed_preset(dpp::snowflake discord_id, const std::string& preset);
    void delete_embed_preset(dpp::snowflake discord_id);
    std::string get_embed_preset(dpp::snowflake discord_id);
    std::unordered_map<uint64_t, std::string> get_all_embed_presets();

    // ============================================================================
    // User custom template operations
    // ============================================================================

    /// Get all custom templates for a user
    std::unordered_map<std::string, std::string> get_user_custom_templates(dpp::snowflake discord_id);

    /// Get a specific custom template for a user and command
    std::optional<std::string> get_user_custom_template(dpp::snowflake discord_id, const std::string& command_id);

    /// Set/update a user's custom template for a command
    void set_user_custom_template(dpp::snowflake discord_id, const std::string& command_id, const std::string& json_config);

    /// Delete a user's custom template for a command
    void delete_user_custom_template(dpp::snowflake discord_id, const std::string& command_id);

    /// Delete all custom templates for a user
    void delete_all_user_custom_templates(dpp::snowflake discord_id);

    // ============================================================================
    // Embed preset template operations (legacy field-based)
    // ============================================================================

    std::vector<std::tuple<std::string, std::string, std::string>> get_all_preset_templates();
    void set_preset_template(const std::string& preset_name, const std::string& field_name, const std::string& template_text);
    void delete_preset_templates(const std::string& preset_name);

    // ============================================================================
    // JSON embed template operations (full embed config)
    // ============================================================================

    /// Get all JSON templates as key->json pairs
    std::vector<std::pair<std::string, std::string>> get_all_json_templates();

    /// Get a specific JSON template by key
    std::optional<std::string> get_json_template(const std::string& key);

    /// Set/update a JSON template
    void set_json_template(const std::string& key, const std::string& json_config);

    /// Delete a JSON template
    void delete_json_template(const std::string& key);

    // ============================================================================
    // Discord users cache operations
    // ============================================================================

    // Cache a Discord user's info
    void cache_discord_user(const DiscordUser& user);

    // Cache multiple users in a batch
    void cache_discord_users_batch(const std::vector<DiscordUser>& users);

    // Get cached user info
    std::optional<DiscordUser> get_discord_user(dpp::snowflake user_id);

    // Get total count of cached users
    size_t get_discord_user_count();

    // Clear all cached Discord users (for re-sync)
    void clear_discord_users();

    // ============================================================================
    // Guild info cache operations
    // ============================================================================

    // Cache guild info
    void cache_guild_info(const GuildInfo& guild);

    // Get cached guild info
    std::optional<GuildInfo> get_guild_info(dpp::snowflake guild_id);

    // ============================================================================
    // Channel info cache operations
    // ============================================================================

    // Cache channel info
    void cache_channel_info(const DiscordChannel& channel);

    // Cache multiple channels in a batch
    void cache_channels_batch(const std::vector<DiscordChannel>& channels);

    // Get cached channel info
    std::optional<DiscordChannel> get_channel_info(dpp::snowflake channel_id);

    // ============================================================================
    // Download log operations (for persistent download statistics)
    // ============================================================================

    // Record a successful download
    void log_download(int64_t beatmapset_id);

    // Get download count for last N hours
    size_t get_downloads_since(std::chrono::hours period) const;

    // Cleanup entries older than 24 hours
    void cleanup_old_downloads();

    // ============================================================================
    // Message crawler operations
    // ============================================================================

    // Store a single message
    void store_message(const CrawledMessage& msg);

    // Store multiple messages in a batch (more efficient)
    void store_messages_batch(const std::vector<CrawledMessage>& messages);

    // Update an existing message (for edited messages)
    void update_message(dpp::snowflake message_id, const std::string& new_content,
                        std::chrono::system_clock::time_point edited_at);

    // Check if a message already exists
    bool message_exists(dpp::snowflake message_id);

    // Get total message count
    size_t get_message_count();

    // Get all message contents for word stats calculation (streaming)
    // Callback returns true to continue, false to stop
    void process_all_messages(std::function<bool(const std::string&)> callback);

    // Get all message contents for a specific user (streaming)
    void process_user_messages(dpp::snowflake author_id, std::function<bool(const std::string&)> callback);

    // Get all message contents for a specific channel (streaming)
    void process_channel_messages(dpp::snowflake channel_id, std::function<bool(const std::string&)> callback);

    // Get all message contents for a specific user in a specific channel (streaming)
    void process_user_channel_messages(dpp::snowflake author_id, dpp::snowflake channel_id, std::function<bool(const std::string&)> callback);

    // Get list of all authors who have messages (sorted by message count)
    std::vector<MessageAuthor> get_message_authors();

    // Get list of all channels with message counts (sorted by message count)
    std::vector<MessageChannel> get_message_channels();

    // ============================================================================
    // Crawl progress operations
    // ============================================================================

    // Save or update crawl progress for a channel
    void save_crawl_progress(const ChannelCrawlProgress& progress);

    // Get crawl progress for a specific channel
    std::optional<ChannelCrawlProgress> get_crawl_progress(dpp::snowflake channel_id);

    // Get all crawl progress for a guild
    std::vector<ChannelCrawlProgress> get_all_crawl_progress(dpp::snowflake guild_id);

    // Get summary of crawl status for a guild
    CrawlStatusSummary get_crawl_status_summary(dpp::snowflake guild_id);

    // ============================================================================
    // Word statistics operations
    // ============================================================================

    // Get top words, optionally filtered by language ("ru", "en", or "" for all)
    // If exclude_stopwords is true, filters out words from the stopwords table
    std::vector<WordStatEntry> get_top_words(size_t limit, const std::string& language = "", bool exclude_stopwords = false);

    // Get count of unique words (optionally filtered by language)
    size_t get_unique_word_count(const std::string& language = "", bool exclude_stopwords = false);

    // Update word stats (upsert word counts)
    void update_word_stats(const std::vector<std::tuple<std::string, size_t, std::string>>& words);

    // Clear and rebuild word stats from messages
    void clear_word_stats();

    // Get stopwords set
    std::unordered_set<std::string> get_stopwords();

    // ============================================================================
    // Phrase statistics operations (bigrams/trigrams with PMI)
    // ============================================================================

    // Update phrase stats (batch upsert)
    void update_phrase_stats(const std::vector<std::tuple<
        std::string,              // phrase
        std::vector<std::string>, // words
        int,                      // word_count
        size_t,                   // count
        std::string               // language
    >>& phrases);

    // Update PMI scores for phrases
    void update_phrase_pmi_scores(const std::vector<std::tuple<std::string, std::string, double>>& phrase_pmi);

    // Update NPMI and LLR scores for phrases (batch update)
    void update_phrase_npmi_llr_scores(
        const std::vector<std::tuple<std::string, std::string, double, double>>& scores  // phrase, language, npmi, llr
    );

    // Get top phrases (sorted by count or PMI)
    std::vector<PhraseStatEntry> get_top_phrases(
        size_t limit,
        const std::string& language = "",
        bool sort_by_pmi = false,
        int word_count_filter = 0,
        size_t min_count = 5
    );

    // Get count of unique phrases
    size_t get_unique_phrase_count(const std::string& language = "", int word_count_filter = 0);

    // Clear phrase stats table
    void clear_phrase_stats();

    // Get global phrase frequencies (for uniqueness calculation)
    std::unordered_map<std::string, size_t> get_global_phrase_frequencies(
        const std::string& language = "",
        int word_count_filter = 0,
        size_t min_count = 5
    );

    // ============================================================================
    // Phrase history operations (for temporal analysis)
    // ============================================================================

    // Save daily snapshot of phrase counts
    void save_phrase_history_snapshot(
        const std::vector<std::tuple<std::string, std::string, size_t>>& phrases  // phrase, language, count
    );

    // Get last snapshot timestamp
    std::optional<std::chrono::system_clock::time_point> get_last_phrase_snapshot_time();

    // Get phrase counts from N days ago (for trend calculation)
    std::unordered_map<std::string, size_t> get_phrase_counts_from_days_ago(int days);

    // Update trend scores for all phrases
    void update_phrase_trend_scores(
        const std::vector<std::tuple<std::string, std::string, double>>& trends  // phrase, language, trend_score
    );

    // Update first_seen timestamps for new phrases
    void update_phrase_first_seen(
        const std::vector<std::tuple<std::string, std::string>>& phrases  // phrase, language
    );

    // ============================================================================
    // Incremental stats processing
    // ============================================================================

    // Get last processed message ID for a stats type ("word_stats" or "phrase_stats")
    int64_t get_stats_last_message_id(const std::string& stats_key);

    // Set last processed message ID for a stats type
    void set_stats_last_message_id(const std::string& stats_key, int64_t message_id);

    // Process messages incrementally (with id > last_id), returns max message_id processed
    // Callback receives (message_id, content), returns true to continue, false to stop
    int64_t process_messages_incremental(
        int64_t last_message_id,
        std::function<bool(int64_t, const std::string&)> callback
    );

    // Upsert word stats (add to existing counts)
    void upsert_word_stats(const std::vector<std::tuple<std::string, size_t, std::string>>& words);

    // Upsert phrase stats (add to existing counts)
    void upsert_phrase_stats(const std::vector<std::tuple<
        std::string,              // phrase
        std::vector<std::string>, // words
        int,                      // word_count
        size_t,                   // count
        std::string               // language
    >>& phrases);

    // Get all word stats as map for PMI calculation (word_lang -> count)
    std::unordered_map<std::string, size_t> get_word_stats_map();

    // Get total word count
    size_t get_total_word_count();

    // Get total bigram count
    size_t get_total_bigram_count();

    // Structure for phrase data needed for PMI calculation
    struct PhraseForPMI {
        std::string phrase;
        std::vector<std::string> words;
        size_t count;
        std::string language;
    };

    // Get all phrases for PMI calculation (with min_count filter)
    std::vector<PhraseForPMI> get_phrases_for_pmi(size_t min_count = 5);

    // ============================================================================
    // Word blacklist operations
    // ============================================================================

    // Get all blacklisted words
    std::unordered_set<std::string> get_word_blacklist();

    // Add word to blacklist
    void add_word_to_blacklist(const std::string& word, const std::string& language = "all");

    // Remove word from blacklist
    void remove_word_from_blacklist(const std::string& word);

    // ============================================================================
    // Template audit log operations
    // ============================================================================

    // Log a template change by an admin
    void log_template_change(
        dpp::snowflake admin_id,
        const std::string& admin_username,
        const std::string& action,           // "update" or "reset"
        const std::string& command_id,
        const std::string& preset,           // empty for single-preset commands
        const std::string& old_fields_json,  // previous template (empty for reset)
        const std::string& new_fields_json   // new template (empty for reset to default)
    );

    // Get recent template audit log entries
    std::vector<TemplateAuditEntry> get_template_audit_log(
        size_t limit = 50,
        size_t offset = 0
    );

    // Get audit log entries for a specific admin
    std::vector<TemplateAuditEntry> get_template_audit_log_by_admin(
        dpp::snowflake admin_id,
        size_t limit = 50
    );

    // Get audit log entries for a specific command
    std::vector<TemplateAuditEntry> get_template_audit_log_by_command(
        const std::string& command_id,
        size_t limit = 50
    );

    // Get a specific audit entry by ID
    std::optional<TemplateAuditEntry> get_template_audit_entry(int64_t id);

    // Raw SQL execution (for migrations)
    template<typename Func>
    auto execute(Func&& func) -> decltype(func(std::declval<pqxx::connection&>())) {
        auto conn = pool_->acquire();
        try {
            if constexpr (std::is_void_v<decltype(func(*conn))>) {
                func(*conn);
                pool_->release(std::move(conn));
            } else {
                auto result = func(*conn);
                pool_->release(std::move(conn));
                return result;
            }
        } catch (...) {
            pool_->release(std::move(conn));
            throw;
        }
    }

private:
    Database(const std::string& connection_string, size_t pool_size);

    static std::unique_ptr<Database> instance_;
    static std::mutex init_mutex_;

    std::unique_ptr<ConnectionPool> pool_;
};

} // namespace db
